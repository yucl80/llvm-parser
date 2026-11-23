//===----------------------------------------------------------------------===//
// class_diff_bitcode.cpp — the LLVM IR + DWARF backend for class_diff.
//
// Parses every .bc/.ll module with LLVM. Functions are demangled into
// "Namespace::Class::method(params)" and fingerprinted from their normalized
// instruction stream (SSA names/numbers and debug intrinsics are dropped).
// When the module carries DWARF (-g) it also supplies the class layout
// (bases/fields/aliases) and the access/virtual traits of methods, including
// pure-virtual ones that have no emitted function body at all.
//===----------------------------------------------------------------------===//

#include "class_diff.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/User.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <tuple>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace classdiff {

/// A single observation about a class/method, collected from either DWARF or
/// IR. Several of them for the same method (multiple modules, ABI variants,
/// declaration + definition) are merged by buildBitcodeSnapshot().
struct BCHit {
    std::string cls;       ///< qualified class name
    std::string key;       ///< method key; empty = class-layout-only hit
    std::string display;
    std::string file;
    unsigned line = 0;
    std::string declHash;  ///< DWARF interface / IR signature fingerprint
    std::string bodyHash;  ///< normalized IR fingerprint of one function
    std::string kind;      ///< class / struct / union
    std::vector<std::string> bases, fields, aliases;
};

bool isDIClassTag(unsigned Tag) {
    return Tag == llvm::dwarf::DW_TAG_class_type ||
           Tag == llvm::dwarf::DW_TAG_structure_type ||
           Tag == llvm::dwarf::DW_TAG_union_type;
}

const char* diKindName(unsigned Tag) {
    switch (Tag) {
    case llvm::dwarf::DW_TAG_structure_type: return "struct";
    case llvm::dwarf::DW_TAG_union_type: return "union";
    default: return "class";
    }
}

const llvm::DIType* diBaseType(const llvm::DIType* T) {
    if (const auto* C = llvm::dyn_cast_or_null<llvm::DICompositeType>(T))
        return C->getBaseType();
    if (const auto* D = llvm::dyn_cast_or_null<llvm::DIDerivedType>(T))
        return D->getBaseType();
    return nullptr;
}

/// "app::Outer::Widget" from the DWARF scope chain.
std::string diQualifiedName(const llvm::DIType* T) {
    if (!T)
        return "?";
    std::string Prefix;
    for (const llvm::DIScope* Scope = T->getScope(); Scope;) {
        if (const auto* Outer = llvm::dyn_cast<llvm::DICompositeType>(Scope)) {
            if (!Outer->getName().empty())
                Prefix = Outer->getName().str() + "::" + Prefix;
            Scope = Outer->getScope();
        } else if (const auto* NS = llvm::dyn_cast<llvm::DINamespace>(Scope)) {
            if (!NS->getName().empty())
                Prefix = NS->getName().str() + "::" + Prefix;
            Scope = NS->getScope();
        } else if (const auto* FN = llvm::dyn_cast<llvm::DISubprogram>(Scope)) {
            Prefix = FN->getName().str() + "()::" + Prefix;
            Scope = FN->getScope();
        } else {
            break;
        }
    }
    std::string Name = T->getName().str();
    if (Name.empty())
        Name = "<anonymous@" + T->getFilename().str() + ":" +
               std::to_string(T->getLine()) + ">";
    return Prefix + Name;
}

std::string diTypeString(const llvm::DIType* T, int Depth = 0) {
    if (!T || Depth > 12)
        return "?";
    switch (T->getTag()) {
    case llvm::dwarf::DW_TAG_base_type: {
        std::string N = T->getName().str();
        return N.empty() ? "void" : N;
    }
    case llvm::dwarf::DW_TAG_typedef: {
        std::string N = T->getName().str();
        return N.empty() ? diTypeString(diBaseType(T), Depth + 1) : N;
    }
    case llvm::dwarf::DW_TAG_const_type:
        return "const " + diTypeString(diBaseType(T), Depth + 1);
    case llvm::dwarf::DW_TAG_volatile_type:
        return "volatile " + diTypeString(diBaseType(T), Depth + 1);
    case llvm::dwarf::DW_TAG_pointer_type:
        return diTypeString(diBaseType(T), Depth + 1) + "*";
    case llvm::dwarf::DW_TAG_reference_type:
        return diTypeString(diBaseType(T), Depth + 1) + "&";
    case llvm::dwarf::DW_TAG_rvalue_reference_type:
        return diTypeString(diBaseType(T), Depth + 1) + "&&";
    case llvm::dwarf::DW_TAG_array_type:
        return diTypeString(diBaseType(T), Depth + 1) + "[]";
    case llvm::dwarf::DW_TAG_class_type:
    case llvm::dwarf::DW_TAG_structure_type:
    case llvm::dwarf::DW_TAG_union_type:
        return diQualifiedName(T);
    case llvm::dwarf::DW_TAG_subroutine_type: {
        const auto* FT = llvm::cast<llvm::DISubroutineType>(T);
        const llvm::DITypeRefArray Args = FT->getTypeArray();
        std::vector<std::string> P;
        for (unsigned I = 1; I < Args.size(); ++I) {
            const llvm::DIType* A = Args[I];
            if (A && A->isObjectPointer())
                continue;
            P.push_back(A ? diTypeString(A, Depth + 1) : "void");
        }
        return "(" + join(P, ", ") + ")";
    }
    default: {
        std::string N = T->getName().str();
        return N.empty() ? "?" : N;
    }
    }
}

/// LLVM 21 keeps isPrivate()/isProtected() on DIDerivedType and DISubprogram
/// rather than on the DINode base class. Clang emits DIFlagPublic explicitly
/// but leaves the flag off for private members, so the class default applies.
template <typename NodeT> std::string diAccess(const NodeT* N) {
    if (!N)
        return "none";
    if (N->isPrivate())
        return "private";
    if (N->isProtected())
        return "protected";
    if (N->isPublic())
        return "public";
    return "private";
}

std::string spParamList(const llvm::DISubprogram* SP) {
    if (!SP || !SP->getType())
        return std::string();
    const llvm::DITypeRefArray Args = SP->getType()->getTypeArray();
    std::vector<std::string> P;
    for (unsigned I = 1; I < Args.size(); ++I) {
        const llvm::DIType* A = Args[I];
        if (A && A->isObjectPointer())
            continue;
        P.push_back(A ? diTypeString(A) : "void");
    }
    return join(P, ", ");
}

/// Interface-only fingerprint: source-level name/params plus the DWARF
/// access, virtual, pure, static and artificial flags. Deliberately excludes
/// the definition flag and source locations, which vary between modules.
std::string spDeclFingerprint(const llvm::DISubprogram* SP) {
    if (!SP)
        return std::string();
    std::vector<std::string> Parts = {
        SP->getName().str(),
        spParamList(SP),
        diAccess(SP),
        "virtuality=" + std::to_string(SP->getVirtuality()),
        "pure=" + std::to_string(SP->isPure() ? 1 : 0),
        "deleted=" + std::to_string(SP->isDeleted() ? 1 : 0),
        "static=" + std::to_string(
                        (SP->getFlags() & llvm::DINode::FlagStaticMember) != 0),
        "artificial=" + std::to_string(
                            (SP->getFlags() & llvm::DINode::FlagArtificial) != 0)};
    return fingerprint(Parts);
}

std::string irTypeString(const llvm::Type* T) {
    if (!T)
        return "?";
    if (const auto* ST = llvm::dyn_cast<llvm::StructType>(T)) {
        // Anonymous/literal struct types carry a module-local slot number;
        // spelling that number would make identical code compare different.
        if (ST->hasName())
            return ST->getName().str();
        return "anon-struct";
    }
    std::string S;
    llvm::raw_string_ostream OS(S);
    T->print(OS);
    return OS.str();
}

/// Operand rendering that ignores SSA value names/numbers (they are replaced
/// by stable local ids) but keeps constants, globals, arguments and callees.
std::string irValueString(const llvm::Value* V,
                          std::map<const llvm::Value*, unsigned>& Ids) {
    if (!V)
        return "null";
    if (const auto* CI = llvm::dyn_cast<llvm::ConstantInt>(V)) {
        llvm::SmallString<64> Num;
        CI->getValue().toString(Num, 10, /*Signed=*/true);
        return "i:" + std::string(Num.begin(), Num.end());
    }
    if (const auto* CF = llvm::dyn_cast<llvm::ConstantFP>(V)) {
        char Buf[64];
        std::snprintf(Buf, sizeof(Buf), "%.17g",
                      CF->getValueAPF().convertToDouble());
        return std::string("f:") + Buf;
    }
    if (llvm::isa<llvm::ConstantPointerNull>(V))
        return "nullptr";
    if (llvm::isa<llvm::UndefValue>(V))
        return "undef";
    if (llvm::isa<llvm::PoisonValue>(V))
        return "poison";
    if (llvm::isa<llvm::ConstantAggregateZero>(V))
        return "zeroinitializer";
    if (const auto* GV = llvm::dyn_cast<llvm::GlobalValue>(V))
        return "g:" + GV->getName().str();
    if (const auto* CE = llvm::dyn_cast<llvm::ConstantExpr>(V))
        return "expr:" + std::to_string(CE->getOpcode());
    if (const auto* Arg = llvm::dyn_cast<llvm::Argument>(V))
        return "a" + std::to_string(Arg->getArgNo());
    if (llvm::isa<llvm::Constant>(V))
        return "const";
    if (llvm::isa<llvm::MetadataAsValue>(V))
        return "meta";
    auto It = Ids.find(V);
    if (It == Ids.end()) {
        const unsigned N = static_cast<unsigned>(Ids.size());
        Ids[V] = N;
        return "v" + std::to_string(N);
    }
    return "v" + std::to_string(It->second);
}

std::string stripDbgMetadata(const std::string& S) {
    static const std::string Key = "!dbg !";
    std::string R = S;
    size_t Pos = 0;
    while ((Pos = R.find(Key, Pos)) != std::string::npos) {
        size_t End = Pos + Key.size();
        while (End < R.size() && std::isdigit(static_cast<unsigned char>(R[End])))
            ++End;
        size_t Begin = Pos;
        while (Begin > 0 && (R[Begin - 1] == ' ' || R[Begin - 1] == '\t'))
            --Begin;
        if (Begin > 0 && R[Begin - 1] == ',')
            --Begin;
        R.erase(Begin, End - Begin);
        Pos = Begin;
    }
    return R;
}

std::string instructionText(const llvm::Instruction& I) {
    std::string S;
    llvm::raw_string_ostream OS(S);
    I.print(OS);
    OS.flush();
    return stripDbgMetadata(S);
}

void collectGlobalInitializers(const llvm::Value* V,
                               std::vector<std::string>& Out,
                               std::set<const llvm::GlobalVariable*>& Seen,
                               int Depth = 0);

void addGlobalVariable(const llvm::GlobalVariable* GV,
                       std::vector<std::string>& Out,
                       std::set<const llvm::GlobalVariable*>& Seen,
                       int Depth) {
    if (!GV || GV->isDeclaration() || !Seen.insert(GV).second || Depth > 8)
        return;
    std::string S;
    llvm::raw_string_ostream OS(S);
    OS << "global " << GV->getName().str() << " | " << *GV;
    OS.flush();
    Out.push_back("global|" + stripDbgMetadata(S));
    if (GV->hasInitializer())
        collectGlobalInitializers(GV->getInitializer(), Out, Seen, Depth + 1);
}

void collectGlobalInitializers(const llvm::Value* V,
                               std::vector<std::string>& Out,
                               std::set<const llvm::GlobalVariable*>& Seen,
                               int Depth) {
    if (!V || Depth > 8)
        return;
    if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
        addGlobalVariable(GV, Out, Seen, Depth);
        return;
    }
    const auto* User = llvm::dyn_cast<llvm::User>(V);
    if (!User)
        return;
    for (const llvm::Use& U : User->operands())
        if (llvm::isa<llvm::Constant>(U.get()))
            collectGlobalInitializers(U.get(), Out, Seen, Depth + 1);
}

std::vector<std::string> irBodyTokens(const llvm::Function& F) {
    std::vector<std::string> Out;
    std::map<const llvm::Value*, unsigned> Ids;
    std::set<const llvm::GlobalVariable*> SeenGlobals;
    for (const llvm::BasicBlock& BB : F) {
        Out.push_back("bb");
        for (const llvm::Instruction& I : BB) {
            if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
                if (const llvm::Function* Callee = CB->getCalledFunction()) {
                    const std::string N = Callee->getName().str();
                    if (startsWith(N, "llvm.dbg"))
                        continue;
                }
            }
            std::string Line = I.getOpcodeName();
            Line += "|" + irTypeString(I.getType());
            for (const llvm::Use& U : I.operands()) {
                Line += "|" + irValueString(U.get(), Ids);
                collectGlobalInitializers(U.get(), Out, SeenGlobals);
            }
            if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
                if (const llvm::Function* Callee = CB->getCalledFunction())
                    Line += "|callee:" + Callee->getName().str();
                else
                    Line += "|callee:indirect";
            }
            Line += "|text:" + instructionText(I);
            Out.push_back(std::move(Line));
        }
    }
    return Out;
}

/// End of an operator token such as "operator<", "operator<<" or
/// "operator<=>". These angle brackets are part of the function name, not
/// template delimiters, so they must not affect the nesting counter.
size_t operatorSymbolEnd(const std::string& S, size_t I) {
    static const char* const Keyword = "operator";
    static const size_t KeywordLen = 8;
    static const char* const Symbols[] = {
        "<=>", "<<=", ">>=", "<<", ">>", "<=", ">=", "->*", "->",
        "()", "[]", "++", "--", "+=", "-=", "*=", "/=", "%=",
        "&=", "|=", "^=", "new[]", "delete[]", "new", "delete",
        "+", "-", "*", "/", "%", "^", "&", "|", "~", "!", "=",
        "<", ">"};
    if (I > 0 && (std::isalnum(static_cast<unsigned char>(S[I - 1])) ||
                  S[I - 1] == '_'))
        return std::string::npos;
    if (I + KeywordLen > S.size() ||
        S.compare(I, KeywordLen, Keyword) != 0)
        return std::string::npos;
    size_t J = I + KeywordLen;
    while (J < S.size() &&
           (S[J] == ' ' || S[J] == '\t' || S[J] == '\n'))
        ++J;
    for (const char* Symbol : Symbols) {
        const size_t N = std::strlen(Symbol);
        if (J + N <= S.size() && S.compare(J, N, Symbol) == 0)
            return J + N;
    }
    return std::string::npos;
}

/// Turn "app::Widget::set(int)" into class="app::Widget" and keep the whole
/// demangled string as the method key (it already distinguishes overloads).
bool splitDemangled(const std::string& D, std::string& Class,
                    std::string& Method) {
    if (D.empty() || D.find("thunk to ") != std::string::npos)
        return false;
    size_t Paren = std::string::npos;
    int Angle = 0;
    for (size_t I = 0; I < D.size(); ++I) {
        const size_t OpEnd = operatorSymbolEnd(D, I);
        if (OpEnd != std::string::npos) {
            I = OpEnd - 1;
            continue;
        }
        const char C = D[I];
        if (C == '<') {
            ++Angle;
        } else if (C == '>') {
            if (Angle > 0)
                --Angle;
        } else if (C == '(' && Angle == 0) {
            Paren = I;
            break;
        }
    }
    if (Paren == std::string::npos)
        return false;
    const std::string Head = D.substr(0, Paren);
    size_t Sep = std::string::npos;
    int A2 = 0;
    for (size_t I = 0; I + 1 < Head.size(); ++I) {
        const size_t OpEnd = operatorSymbolEnd(Head, I);
        if (OpEnd != std::string::npos) {
            I = OpEnd - 1;
            continue;
        }
        if (Head[I] == '<') {
            ++A2;
        } else if (Head[I] == '>') {
            if (A2 > 0)
                --A2;
        } else if (A2 == 0 && Head[I] == ':' && Head[I + 1] == ':') {
            Sep = I;
            ++I;
        }
    }
    if (Sep == std::string::npos || Sep == 0)
        return false;
    Class = Head.substr(0, Sep);
    Method = D;
    return !Class.empty();
}

std::string demangleName(llvm::StringRef N) {
    if (N.empty())
        return std::string();
    const std::string Mangled = N.str();
    const std::string D = llvm::demangle(std::string_view(Mangled));
    return D.empty() ? Mangled : D;
}

std::string diFilePath(const llvm::DIFile* F) {
    if (!F)
        return std::string();
    const std::string Name = F->getFilename().str();
    if (Name.empty())
        return std::string();
    if (Name[0] == '/')
        return normPath(Name);
    const std::string Dir = F->getDirectory().str();
    return normPath(Dir.empty() ? Name : Dir + "/" + Name);
}

std::string relativeToRoot(const std::string& Root, const std::string& Abs) {
    if (Abs.empty())
        return std::string();
    const std::string P = normPath(Abs);
    if (!Root.empty() && startsWith(P, Root))
        return P.substr(Root.size());
    return fs::path(P).filename().generic_string();
}

/// Directories of the compile units' source files (plus their parents, so a
/// header next to a .cpp counts as application code). Used to keep system
/// header types such as __va_list_tag out of the report.
std::vector<std::string> appSourceDirs(const llvm::Module& M) {
    std::vector<std::string> Dirs;
    for (const llvm::DICompileUnit* CU : M.debug_compile_units()) {
        if (!CU || !CU->getFile())
            continue;
        const std::string P = diFilePath(CU->getFile());
        if (P.empty())
            continue;
        const fs::path Parent = fs::path(P).parent_path();
        if (Parent.empty())
            continue;
        Dirs.push_back(Parent.generic_string());
        if (Parent.has_parent_path())
            Dirs.push_back(Parent.parent_path().generic_string());
    }
    std::sort(Dirs.begin(), Dirs.end());
    Dirs.erase(std::unique(Dirs.begin(), Dirs.end()), Dirs.end());
    Dirs.erase(std::remove(Dirs.begin(), Dirs.end(), std::string()),
               Dirs.end());
    return Dirs;
}

bool inAppTree(const std::vector<std::string>& Dirs, const std::string& P) {
    if (Dirs.empty() || P.empty())
        return true;
    for (const std::string& D : Dirs)
        if (P == D || startsWith(P, D + "/"))
            return true;
    return false;
}

bool isLibraryClass(const std::string& C, const Options& O) {
    if (O.includeLibrary)
        return false;
    static const std::vector<std::string> Defaults = {
        "std::", "__", "llvm::", "clang::", "boost::", "absl::",
        "fmt::", "nlohmann::", "Eigen::", "google::", "testing::",
        "spdlog::", "asio::", "folly::", "boost_", "sqlite3", "SDL_",
        "Qt", "cv::", "torch::", "vtable for", "typeinfo",
        "non-virtual thunk", "virtual thunk"};
    for (const std::string& P : Defaults)
        if (startsWith(C, P))
            return true;
    for (const std::string& P : O.libPrefixes)
        if (startsWith(C, P))
            return true;
    return false;
}

void collectBitcodeHits(const llvm::Module& M, const std::string& Rel,
                        const std::string& Root, const Options& O,
                        std::vector<BCHit>& Hits, bool& HasDebugInfo) {
    if (!M.debug_compile_units().empty())
        HasDebugInfo = true;
    const std::vector<std::string> AppDirs = appSourceDirs(M);

    // (1) DWARF: class layout + methods declared inside the class (including
    //     pure virtual methods, which have no emitted Function at all).
    llvm::DebugInfoFinder Finder;
    Finder.processModule(M);
    for (const auto* T : Finder.types()) {
        const auto* CT = llvm::dyn_cast<llvm::DICompositeType>(T);
        if (!CT || !isDIClassTag(CT->getTag()) || CT->isForwardDecl())
            continue;
        const std::string Cls = diQualifiedName(CT);
        if (Cls.empty() || isLibraryClass(Cls, O))
            continue;
        const std::string ClassAbs = diFilePath(CT->getFile());
        const std::string ClassRel = relativeToRoot(Root, ClassAbs);
        if (isIgnored(ClassRel, O.ignores))
            continue;
        if (!inAppTree(AppDirs, ClassAbs))
            continue;  // system-header type retained through #include
        BCHit Layout;
        Layout.cls = Cls;
        Layout.kind = diKindName(CT->getTag());
        Layout.file = ClassRel.empty() ? CT->getFilename().str() : ClassRel;
        Layout.line = CT->getLine();
        for (const llvm::DINode* E : CT->getElements()) {
            if (const auto* D = llvm::dyn_cast<llvm::DIDerivedType>(E)) {
                if (D->getTag() == llvm::dwarf::DW_TAG_inheritance) {
                    Layout.bases.push_back(diAccess(D) + " " +
                                           diTypeString(D->getBaseType()));
                } else if (D->getTag() == llvm::dwarf::DW_TAG_member &&
                           !D->isArtificial()) {
                    Layout.fields.push_back(
                        diAccess(D) + " " + diTypeString(D->getBaseType()) +
                        " " + D->getName().str());
                } else if (D->getTag() == llvm::dwarf::DW_TAG_typedef) {
                    Layout.aliases.push_back(diAccess(D) + " " +
                                             D->getName().str());
                }
            } else if (const auto* SP =
                           llvm::dyn_cast<llvm::DISubprogram>(E)) {
                std::string Key, Cls2;
                if (!SP->getLinkageName().empty()) {
                    const std::string Dm =
                        demangleName(SP->getLinkageName());
                    if (Dm != SP->getLinkageName() &&
                        splitDemangled(Dm, Cls2, Key)) {
                        // Key and Cls2 come from the same demangled name as
                        // the Function path, so both hits merge cleanly.
                    } else {
                        Key.clear();
                    }
                }
                if (Key.empty()) {
                    Cls2 = Cls;
                    Key = Cls + "::" + SP->getName().str() + "(" +
                          spParamList(SP) + ")";
                }
                if (isLibraryClass(Cls2, O))
                    continue;
                const std::string MethodAbs = diFilePath(SP->getFile());
                const std::string MethodRel = relativeToRoot(Root, MethodAbs);
                if (isIgnored(MethodRel, O.ignores))
                    continue;
                BCHit H;
                H.cls = Cls2;
                H.key = Key;
                H.display = Key;
                H.file = MethodRel.empty() ? SP->getFilename().str() : MethodRel;
                H.line = SP->getLine();
                H.declHash = spDeclFingerprint(SP);
                Hits.push_back(std::move(H));
            }
        }
        Hits.push_back(std::move(Layout));
    }

    // (2) LLVM IR: every emitted member function, identified by its demangled
    //     signature and fingerprinted from its normalized instruction stream.
    for (const llvm::Function& F : M) {
        if (F.isDeclaration())
            continue;
        const std::string D = demangleName(F.getName());
        std::string Cls, Key;
        if (!splitDemangled(D, Cls, Key) || isLibraryClass(Cls, O))
            continue;
        const llvm::DISubprogram* SP = F.getSubprogram();
        std::string FuncRel = Rel;
        if (SP) {
            const std::string FuncAbs = diFilePath(SP->getFile());
            const std::string Candidate = relativeToRoot(Root, FuncAbs);
            if (!Candidate.empty())
                FuncRel = Candidate;
        }
        if (isIgnored(FuncRel, O.ignores))
            continue;
        BCHit H;
        H.cls = Cls;
        H.key = Key;
        H.display = Key;
        H.bodyHash = fingerprint(irBodyTokens(F));
        if (H.bodyHash.empty())
            H.bodyHash = "empty";
        H.declHash = fingerprint(
            {irTypeString(F.getFunctionType()),
             "linkage=" + std::to_string(static_cast<unsigned>(F.getLinkage())),
             "cc=" + std::to_string(F.getCallingConv()),
             spDeclFingerprint(SP), spParamList(SP)});
        H.file = FuncRel.empty() ? Rel : FuncRel;
        if (SP)
            H.line = SP->getLine();
        Hits.push_back(std::move(H));
    }
}

void buildBitcodeSnapshot(Snapshot& S, const Options& O) {
    std::vector<BCHit> Hits;
    for (const std::string& Abs : S.bitcodes) {
        llvm::SMDiagnostic Diag;
        llvm::LLVMContext Ctx;
        std::unique_ptr<llvm::Module> M = llvm::parseIRFile(Abs, Diag, Ctx);
        if (!M) {
            std::fprintf(stderr, "[WARN] cannot parse %s: %s\n", Abs.c_str(),
                         Diag.getMessage().str().c_str());
            ++S.parseErrors;
            continue;
        }
        const std::string Rel =
            startsWith(Abs, S.root)
                ? Abs.substr(S.root.size())
                : fs::path(Abs).filename().generic_string();
        collectBitcodeHits(*M, Rel, S.root, O, Hits, S.hasDebugInfo);
    }

    // A method may be emitted in several modules and as several ABI variants
    // (C1/C2 constructors, D0/D1/D2 destructors). Sorting makes the merge
    // independent of module order; all body fingerprints are accumulated.
    std::sort(Hits.begin(), Hits.end(), [](const BCHit& A, const BCHit& B) {
        return std::tie(A.cls, A.key, A.bodyHash, A.declHash, A.file) <
               std::tie(B.cls, B.key, B.bodyHash, B.declHash, B.file);
    });

    std::map<std::string, std::vector<std::string>> Bodies;
    for (const BCHit& H : Hits) {
        ClassRecord& C = S.classes[H.cls];
        if (C.name.empty()) {
            C.name = H.cls;
            C.file = H.file;
            C.line = H.line;
        }
        if (C.kind.empty() && !H.kind.empty())
            C.kind = H.kind;
        for (const std::string& X : H.bases)
            C.bases.insert(X);
        for (const std::string& X : H.fields)
            C.fields.insert(X);
        for (const std::string& X : H.aliases)
            C.aliases.insert(X);
        if (H.key.empty())
            continue;
        MethodRecord& R = C.methods[H.key];
        if (R.key.empty()) {
            R.key = H.key;
            R.display = H.display;
            R.declFile = H.file;
            R.declLine = H.line;
            R.hasDecl = true;
        }
        if (R.declHash.empty() && !H.declHash.empty())
            R.declHash = H.declHash;
        if (!H.bodyHash.empty()) {
            Bodies[H.cls + "\n" + H.key].push_back(H.bodyHash);
            if (R.defFile.empty()) {
                R.defFile = H.file;
                R.defLine = H.line;
            }
            R.hasDef = true;
        }
    }
    for (auto& KV : Bodies) {
        const size_t NL = KV.first.find('\n');
        std::vector<std::string> Sorted = KV.second;
        std::sort(Sorted.begin(), Sorted.end());
        Sorted.erase(std::unique(Sorted.begin(), Sorted.end()), Sorted.end());
        S.classes[KV.first.substr(0, NL)]
            .methods[KV.first.substr(NL + 1)]
            .bodyHash = fingerprint(Sorted);
    }
}

}  // namespace classdiff
