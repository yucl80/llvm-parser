//===----------------------------------------------------------------------===//
// class_diff_source.cpp — the libclang AST backend for class_diff.
//
// Parses every C/C++ source file (using a compilation database when present,
// otherwise -std + -I<root> plus the user's -- arguments) and records, per
// class, its interface (kind, bases, fields, aliases) and, per method, the
// token stream of its declaration and its definition. clang_tokenize() keeps
// the comparison blind to comments, whitespace, formatting and line moves.
//===----------------------------------------------------------------------===//

#include "class_diff.h"

#include <clang-c/CXCompilationDatabase.h>
#include <clang-c/Index.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace classdiff {

std::string toStd(CXString S) {
    const char* C = clang_getCString(S);
    std::string R = C ? C : "";
    clang_disposeString(S);
    return R;
}

std::string accessStr(CX_CXXAccessSpecifier A) {
    switch (A) {
    case CX_CXXPublic: return "public";
    case CX_CXXProtected: return "protected";
    case CX_CXXPrivate: return "private";
    case CX_CXXInvalidAccessSpecifier: break;
    }
    return "none";
}

bool isClassKind(CXCursorKind K) {
    return K == CXCursor_ClassDecl || K == CXCursor_StructDecl ||
           K == CXCursor_UnionDecl || K == CXCursor_ClassTemplate ||
           K == CXCursor_ClassTemplatePartialSpecialization;
}

bool isMethodKind(CXCursorKind K) {
    return K == CXCursor_CXXMethod || K == CXCursor_Constructor ||
           K == CXCursor_Destructor || K == CXCursor_ConversionFunction ||
           K == CXCursor_FunctionTemplate;
}

bool isContainerKind(CXCursorKind K) {
    return isClassKind(K) || K == CXCursor_Namespace ||
           K == CXCursor_LinkageSpec || K == CXCursor_UnexposedDecl;
}

struct CompileArgs {
    std::vector<std::string> args;
    std::string directory;  ///< working directory for relative -I / -D paths
};

class CompilationDB {
public:
    CompilationDB(const std::string& Root, bool Enabled) {
        if (!Enabled)
            return;
        std::error_code EC;
        if (!fs::exists(fs::path(Root) / "compile_commands.json", EC))
            return;  // libclang prints an error when asked to open a missing DB
        CXCompilationDatabase_Error Err = CXCompilationDatabase_NoError;
        DB_ = clang_CompilationDatabase_fromDirectory(Root.c_str(), &Err);
    }
    ~CompilationDB() {
        if (DB_)
            clang_CompilationDatabase_dispose(DB_);
    }

    bool commandFor(const std::string& Abs, CompileArgs& Out) const {
        if (!DB_)
            return false;
        CXCompileCommands Cmds =
            clang_CompilationDatabase_getCompileCommands(DB_, Abs.c_str());
        if (!Cmds)
            return false;
        const unsigned N = clang_CompileCommands_getSize(Cmds);
        if (N != 0) {
            CXCompileCommand Cmd = clang_CompileCommands_getCommand(Cmds, 0);
            Out.directory = normPath(toStd(clang_CompileCommand_getDirectory(Cmd)));
            const std::string Src = toStd(clang_CompileCommand_getFilename(Cmd));
            const unsigned Argc = clang_CompileCommand_getNumArgs(Cmd);
            for (unsigned I = 1; I < Argc; ++I) {
                std::string A = toStd(clang_CompileCommand_getArg(Cmd, I));
                if (A.empty())
                    continue;
                // Drop the driver, output/dependency flags and the input file;
                // everything else (-I, -D, -std, -x, -include, ...) is useful.
                if (A == "-o" || A == "-MF" || A == "-MT" || A == "-MQ") {
                    ++I;
                    continue;
                }
                if (A == "-c" || A == "-MD" || A == "-MMD" || A == "-MP" ||
                    A == "-M" || A == "-MM" || A == "--" ||
                    startsWith(A, "-o"))
                    continue;
                if (A == Abs || A == Src || normPath(A) == Abs)
                    continue;
                Out.args.push_back(std::move(A));
            }
        }
        clang_CompileCommands_dispose(Cmds);
        return N != 0;
    }

private:
    CXCompilationDatabase DB_ = nullptr;
};

bool hasArg(const std::vector<std::string>& Args, const char* Prefix) {
    for (const std::string& A : Args)
        if (startsWith(A, Prefix))
            return true;
    return false;
}

CompileArgs buildArgs(const std::string& Abs, const Snapshot& S,
                      const CompilationDB& DB, const Options& O,
                      const std::string& ResourceArg) {
    CompileArgs CA;
    if (!DB.commandFor(Abs, CA))
        CA = CompileArgs{};
    if (CA.args.empty()) {
        // No compilation database (or no entry for this file): use defaults.
        CA.directory.clear();
        CA.args.push_back("-x");
        CA.args.push_back("c++");
        CA.args.push_back("-std=" + O.standard);
        CA.args.push_back("-I" + S.root.substr(0, S.root.size() - 1));
        const size_t Slash = Abs.find_last_of('/');
        if (Slash != std::string::npos)
            CA.args.push_back("-I" + Abs.substr(0, Slash));
        if (!ResourceArg.empty())
            CA.args.push_back(ResourceArg);
    } else if (!hasArg(O.clangArgs, "-resource-dir") && !ResourceArg.empty() &&
               !hasArg(CA.args, "-resource-dir")) {
        CA.args.push_back(ResourceArg);
    }
    CA.args.insert(CA.args.end(), O.clangArgs.begin(), O.clangArgs.end());
    return CA;
}

struct VisitCtx {
    Snapshot* snap;
    CXTranslationUnit tu;
    const std::vector<std::string>* ignores;
};

/// Resolve a cursor to a path relative to the project root. Returns false for
/// system headers, files outside the tree and explicitly ignored paths.
bool projectLocation(CXCursor C, const VisitCtx& Ctx, std::string& Rel,
                     unsigned& Line) {
    const CXSourceLocation Loc = clang_getCursorLocation(C);
    if (clang_Location_isInSystemHeader(Loc))
        return false;
    CXFile F = nullptr;
    unsigned Col = 0, Off = 0;
    clang_getFileLocation(Loc, &F, &Line, &Col, &Off);
    if (!F)
        return false;
    const std::string Abs = normPath(toStd(clang_getFileName(F)));
    if (Abs.empty() || !startsWith(Abs, Ctx.snap->root))
        return false;
    Rel = Abs.substr(Ctx.snap->root.size());
    return !isIgnored(Rel, *Ctx.ignores);
}

/// "ns::Outer::Foo" — spelling based so it stays stable across the two trees.
std::string qualifiedName(CXCursor C, const std::string& Rel, unsigned Line) {
    std::vector<std::string> Parts;
    for (CXCursor P = C; !clang_Cursor_isNull(P);
         P = clang_getCursorSemanticParent(P)) {
        const CXCursorKind K = clang_getCursorKind(P);
        if (K == CXCursor_TranslationUnit)
            break;
        if (K != CXCursor_Namespace && !isClassKind(K) &&
            K != CXCursor_EnumDecl)
            continue;
        std::string N = toStd(clang_getCursorSpelling(P));
        if (N.empty())
            N = toStd(clang_getCursorDisplayName(P));
        if (N.empty() || N == "(anonymous)" ||
            N.find("anonymous") != std::string::npos) {
            N = isClassKind(K)
                    ? "(anonymous@" + Rel + ":" + std::to_string(Line) + ")"
                    : std::string();
        }
        if (!N.empty())
            Parts.push_back(N);
    }
    std::reverse(Parts.begin(), Parts.end());
    return join(Parts, "::");
}

/// Lexical tokens of a cursor's extent, with comments removed.
std::vector<std::string> tokensOf(CXTranslationUnit TU, CXCursor C) {
    std::vector<std::string> Tokens;
    CXSourceRange Range = clang_getCursorExtent(C);
    if (clang_Range_isNull(Range))
        return Tokens;
    CXToken* Raw = nullptr;
    unsigned N = 0;
    clang_tokenize(TU, Range, &Raw, &N);
    Tokens.reserve(N);
    for (unsigned I = 0; I < N; ++I) {
        if (clang_getTokenKind(Raw[I]) == CXToken_Comment)
            continue;
        Tokens.push_back(toStd(clang_getTokenSpelling(TU, Raw[I])));
    }
    clang_disposeTokens(TU, Raw, N);
    return Tokens;
}

std::string parameterTypes(CXCursor M) {
    if (clang_getCursorKind(M) == CXCursor_FunctionTemplate)
        return toStd(clang_getTypeSpelling(clang_getCursorType(M)));
    std::vector<std::string> Types;
    const int N = clang_Cursor_getNumArguments(M);
    for (int I = 0; I < N; ++I) {
        CXCursor A = clang_Cursor_getArgument(M, static_cast<unsigned>(I));
        Types.push_back(toStd(clang_getTypeSpelling(clang_getCursorType(A))));
    }
    if (clang_Cursor_isVariadic(M))
        Types.push_back("...");
    return join(Types, ", ");
}

std::string methodTraits(CXCursor M) {
    const bool IsTemplate =
        clang_getCursorKind(M) == CXCursor_FunctionTemplate;
    auto bit = [](unsigned V) { return V ? "1" : "0"; };
    std::string T;
    T += "access=" + accessStr(clang_getCXXAccessSpecifier(M));
    T += ";static=" + std::string(bit(IsTemplate ? 0 : clang_CXXMethod_isStatic(M)));
    T += ";virtual=" + std::string(bit(IsTemplate ? 0 : clang_CXXMethod_isVirtual(M)));
    T += ";pure=" + std::string(bit(IsTemplate ? 0 : clang_CXXMethod_isPureVirtual(M)));
    T += ";const=" + std::string(bit(IsTemplate ? 0 : clang_CXXMethod_isConst(M)));
    T += ";deleted=" + std::string(bit(IsTemplate ? 0 : clang_CXXMethod_isDeleted(M)));
    T += ";defaulted=" + std::string(bit(IsTemplate ? 0 : clang_CXXMethod_isDefaulted(M)));
    return T;
}

std::string classKind(CXCursor C) {
    switch (clang_getCursorKind(C)) {
    case CXCursor_StructDecl: return "struct";
    case CXCursor_UnionDecl: return "union";
    case CXCursor_ClassTemplate: return "class template";
    case CXCursor_ClassTemplatePartialSpecialization: return "class template (partial)";
    default: return "class";
    }
}

/// One level of class children: bases, data members, aliases.
CXChildVisitResult classMemberVisitor(CXCursor C, CXCursor, CXClientData Data) {
    ClassRecord* R = static_cast<ClassRecord*>(Data);
    const std::string Access = accessStr(clang_getCXXAccessSpecifier(C));
    switch (clang_getCursorKind(C)) {
    case CXCursor_CXXBaseSpecifier:
        R->bases.insert(Access + " " +
                        std::string(clang_isVirtualBase(C) ? "virtual " : "") +
                        toStd(clang_getTypeSpelling(clang_getCursorType(C))));
        break;
    case CXCursor_FieldDecl:
    case CXCursor_VarDecl:
        R->fields.insert(Access + " " +
                         toStd(clang_getTypeSpelling(clang_getCursorType(C))) +
                         " " + toStd(clang_getCursorSpelling(C)));
        break;
    case CXCursor_TypedefDecl:
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypeAliasTemplateDecl:
    case CXCursor_UsingDeclaration:
        R->aliases.insert(Access + " " + toStd(clang_getCursorDisplayName(C)));
        break;
    default:
        break;
    }
    return CXChildVisit_Continue;
}

void handleClass(CXCursor C, const VisitCtx& Ctx) {
    std::string Rel;
    unsigned Line = 0;
    if (!projectLocation(C, Ctx, Rel, Line))
        return;
    const std::string Name = qualifiedName(C, Rel, Line);
    if (Name.empty() || !clang_isCursorDefinition(C))
        return;
    ClassRecord& R = Ctx.snap->classes[Name];
    if (R.name.empty()) {
        R.name = Name;
        R.kind = classKind(C);
        R.file = Rel;
        R.line = Line;
    }
    clang_visitChildren(C, &classMemberVisitor, &R);
}

void handleMethod(CXCursor M, const VisitCtx& Ctx) {
    CXCursor Parent = clang_getCursorSemanticParent(M);
    if (!isClassKind(clang_getCursorKind(Parent)))
        return;  // free function / function template, not a member
    std::string Rel;
    unsigned Line = 0;
    if (!projectLocation(M, Ctx, Rel, Line))
        return;

    const std::string Class = qualifiedName(Parent, Rel, Line);
    if (Class.empty())
        return;
    const std::string Name = toStd(clang_getCursorSpelling(M));
    const std::string Params = parameterTypes(M);
    const bool IsTemplate =
        clang_getCursorKind(M) == CXCursor_FunctionTemplate;
    const bool IsConst = !IsTemplate && clang_CXXMethod_isConst(M) != 0;

    std::string Key = Class + "::" + Name + "(" + Params + ")";
    if (IsConst)
        Key += " const";
    if (IsTemplate)
        Key += " [template]";
    std::string Display = Key;
    if (IsTemplate)
        Display = Class + "::" + toStd(clang_getCursorDisplayName(M));

    const bool InClass =
        isClassKind(clang_getCursorKind(clang_getCursorLexicalParent(M)));
    const bool IsDef = clang_isCursorDefinition(M) != 0;
    const std::vector<std::string> Tokens = tokensOf(Ctx.tu, M);
    const std::string Hash = fingerprint(Tokens);

    ClassRecord& C = Ctx.snap->classes[Class];
    if (C.name.empty())
        C.name = Class;
    MethodRecord& Rec = C.methods[Key];
    if (Rec.key.empty()) {
        Rec.key = Key;
        Rec.display = Display;
    }
    // Declarations and definitions may arrive from different translation
    // units (header + .cpp). Merge them field by field, preferring the
    // in-class declaration for the interface and the definition for the body.
    if (InClass && !Rec.hasDecl) {
        Rec.declFile = Rel;
        Rec.declLine = Line;
        Rec.declHash = Hash;
        Rec.traits = methodTraits(M);
        Rec.hasDecl = true;
    }
    if (IsDef && !Rec.hasDef) {
        Rec.defFile = Rel;
        Rec.defLine = Line;
        Rec.bodyHash = Hash;
        Rec.hasDef = true;
    }
    if (Rec.traits.empty())
        Rec.traits = methodTraits(M);
}

CXChildVisitResult translationUnitVisitor(CXCursor C, CXCursor,
                                          CXClientData Data) {
    VisitCtx* Ctx = static_cast<VisitCtx*>(Data);
    const CXCursorKind K = clang_getCursorKind(C);
    if (isClassKind(K))
        handleClass(C, *Ctx);
    if (isMethodKind(K))
        handleMethod(C, *Ctx);
    return isContainerKind(K) ? CXChildVisit_Recurse : CXChildVisit_Continue;
}

void buildSnapshot(Snapshot& S, const Options& O,
                   const std::string& ResourceArg) {
    CompilationDB DB(S.root, O.useCompilationDB);
    CXIndex Index = clang_createIndex(/*excludeDeclarationsFromPCH=*/0,
                                      /*displayDiagnostics=*/0);
    for (const std::string& Abs : S.sources) {
        const CompileArgs CA = buildArgs(Abs, S, DB, O, ResourceArg);
        // A compilation database stores flags relative to its own working
        // directory, so switch there while libclang resolves them.
        std::error_code EC;
        const fs::path Saved = fs::current_path(EC);
        if (!CA.directory.empty())
            fs::current_path(fs::path(CA.directory), EC);
        std::vector<const char*> Argv;
        Argv.reserve(CA.args.size());
        for (const std::string& A : CA.args)
            Argv.push_back(A.c_str());
        CXTranslationUnit TU = nullptr;
        const CXErrorCode Err = clang_parseTranslationUnit2(
            Index, Abs.c_str(), Argv.data(), static_cast<int>(Argv.size()),
            nullptr, 0, CXTranslationUnit_KeepGoing, &TU);
        if (!Saved.empty())
            fs::current_path(Saved, EC);
        if (Err != CXError_Success || !TU) {
            std::fprintf(stderr, "[WARN] cannot parse %s (CXErrorCode %d)\n",
                         Abs.c_str(), static_cast<int>(Err));
            ++S.parseErrors;
            continue;
        }
        unsigned Shown = 0;
        const unsigned NumDiags = clang_getNumDiagnostics(TU);
        for (unsigned I = 0; I < NumDiags; ++I) {
            CXDiagnostic D = clang_getDiagnostic(TU, I);
            if (clang_getDiagnosticSeverity(D) >= CXDiagnostic_Error) {
                ++S.parseErrors;
                if (Shown++ < 2) {
                    const std::string Text =
                        toStd(clang_formatDiagnostic(
                            D, clang_defaultDiagnosticDisplayOptions()));
                    std::fprintf(stderr, "[WARN] %s\n", Text.c_str());
                }
            }
            clang_disposeDiagnostic(D);
        }
        VisitCtx Ctx{&S, TU, &O.ignores};
        clang_visitChildren(clang_getTranslationUnitCursor(TU),
                            &translationUnitVisitor, &Ctx);
        clang_disposeTranslationUnit(TU);
    }
    clang_disposeIndex(Index);
}

}  // namespace classdiff
