//===----------------------------------------------------------------------===//
// sql_extractor_source.cpp — libclang backend of the SQL-script extractor.
//
// Walks the AST of each C/C++ translation unit and looks for calls to SQLite
// APIs (sqlite3_exec, sqlite3_prepare_v2, ...) or user supplied --api names.
// The SQL argument is evaluated statically:
//
//   * string literals, including escaped/C++ raw/adjacent literals — libclang
//     already concatenates adjacent literals and normalises raw strings;
//   * macro expansions (#define SQL "SELECT ...") — the StringLiteral cursor
//     carries the expanded text, so no tokenisation is required;
//   * variables / constants initialised with a literal, resolved through the
//     referenced VarDecl (recursion is guarded by the declaration USR);
//   * wrapper objects ("std::string sql = ...; sql.c_str()") via the object
//     expression carried by the member-call callee;
//   * SQL producers: functions returning a constant, virtual dispatch (all
//     overrides in the chain), function pointers/tables, lambdas and
//     std::function callables;
//   * format templates and static fragments of dynamic expressions;
//   * parentheses, casts, wrappers and simple concatenations; unresolvable
//     parts are reported as "dynamic" instead of being guessed.
//
// --scan-literals additionally collects SQL-looking string literals that no
// resolvable API call consumed (SQL kept in constants, string objects,
// conditional branches, tables, resources, ...).
//===----------------------------------------------------------------------===//

#include "sql_extract.h"

#include <clang-c/Index.h>

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace sqlinfo;

namespace {

//===----------------------------------------------------------------------===//
// libclang helpers
//===----------------------------------------------------------------------===//

std::string toStd(CXString S) {
    const char *C = clang_getCString(S);
    std::string R = C ? C : "";
    clang_disposeString(S);
    return R;
}

struct Location {
    std::string file;
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
};

/// Macro-expanded cursors have a spelling location in the macro definition;
/// the expansion location is what a user wants to see in the report.
Location expansionLocation(CXCursor C) {
    CXFile File = nullptr;
    unsigned Line = 0, Column = 0, Offset = 0;
    clang_getExpansionLocation(clang_getCursorLocation(C), &File, &Line,
                               &Column, &Offset);
    Location L;
    if (File)
        L.file = toStd(clang_getFileName(File));
    L.line = Line;
    L.column = Column;
    L.offset = Offset;
    return L;
}

std::string locationKey(const Location &L) {
    return L.file + ":" + std::to_string(L.offset);
}

std::vector<CXCursor> children(CXCursor C) {
    std::vector<CXCursor> Out;
    clang_visitChildren(
        C,
        [](CXCursor Child, CXCursor, CXClientData Data) {
            static_cast<std::vector<CXCursor> *>(Data)->push_back(Child);
            return CXChildVisit_Continue;
        },
        &Out);
    return Out;
}

std::string cursorKey(CXCursor C) {
    std::string Key = toStd(clang_getCursorUSR(C));
    if (Key.empty()) {
        Location L = expansionLocation(C);
        Key = locationKey(L) + ":" + toStd(clang_getCursorSpelling(C));
    }
    return Key;
}

/// "ns::Class::" prefix of a declaration, used to match qualified --api names.
std::string qualifiedName(CXCursor C) {
    std::vector<std::string> Parts;
    CXCursor Parent = clang_getCursorSemanticParent(C);
    while (!clang_Cursor_isNull(Parent)) {
        CXCursorKind K = clang_getCursorKind(Parent);
        if (K == CXCursor_Namespace || K == CXCursor_ClassDecl ||
            K == CXCursor_StructDecl || K == CXCursor_UnionDecl ||
            K == CXCursor_ClassTemplate ||
            K == CXCursor_ClassTemplatePartialSpecialization) {
            std::string Name = toStd(clang_getCursorSpelling(Parent));
            if (!Name.empty())
                Parts.push_back(Name);
            Parent = clang_getCursorSemanticParent(Parent);
            continue;
        }
        break;
    }
    std::string Out;
    for (auto It = Parts.rbegin(); It != Parts.rend(); ++It)
        Out += *It + "::";
    return Out + toStd(clang_getCursorSpelling(C));
}

bool isFunctionKind(CXCursorKind K) {
    return K == CXCursor_FunctionDecl || K == CXCursor_CXXMethod ||
           K == CXCursor_Constructor || K == CXCursor_Destructor ||
           K == CXCursor_ConversionFunction || K == CXCursor_FunctionTemplate;
}

/// The callee expression is the first child of a CallExpr (a DeclRefExpr, a
/// MemberRefExpr, or an UnexposedExpr wrapping one).
CXCursor findFunctionRef(CXCursor C, int Depth) {
    if (Depth > 3 || clang_Cursor_isNull(C))
        return clang_getNullCursor();
    CXCursorKind K = clang_getCursorKind(C);
    if (K == CXCursor_DeclRefExpr || K == CXCursor_MemberRefExpr) {
        CXCursor Ref = clang_getCursorReferenced(C);
        if (!clang_Cursor_isNull(Ref) &&
            isFunctionKind(clang_getCursorKind(Ref)))
            return Ref;
    }
    for (CXCursor Child : children(C)) {
        CXCursor Ref = findFunctionRef(Child, Depth + 1);
        if (!clang_Cursor_isNull(Ref))
            return Ref;
    }
    return clang_getNullCursor();
}

//===----------------------------------------------------------------------===//
// String literal decoding
//===----------------------------------------------------------------------===//

void appendUtf8(std::string &Out, unsigned CodePoint) {
    if (CodePoint <= 0x7f) {
        Out += static_cast<char>(CodePoint);
    } else if (CodePoint <= 0x7ff) {
        Out += static_cast<char>(0xc0 | (CodePoint >> 6));
        Out += static_cast<char>(0x80 | (CodePoint & 0x3f));
    } else if (CodePoint <= 0xffff) {
        Out += static_cast<char>(0xe0 | (CodePoint >> 12));
        Out += static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3f));
        Out += static_cast<char>(0x80 | (CodePoint & 0x3f));
    } else {
        Out += static_cast<char>(0xf0 | (CodePoint >> 18));
        Out += static_cast<char>(0x80 | ((CodePoint >> 12) & 0x3f));
        Out += static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3f));
        Out += static_cast<char>(0x80 | (CodePoint & 0x3f));
    }
}

int hexDigit(char C) {
    if (C >= '0' && C <= '9')
        return C - '0';
    if (C >= 'a' && C <= 'f')
        return C - 'a' + 10;
    if (C >= 'A' && C <= 'F')
        return C - 'A' + 10;
    return -1;
}

/// libclang renders a StringLiteral spelling as a quoted, C-escaped string
/// (also for raw strings and adjacent literal concatenations). Decode it back
/// to the exact bytes that reach sqlite3 at runtime.
bool decodeLiteral(const std::string &Spelling, std::string &Out) {
    const size_t N = Spelling.size();
    if (N == 0)
        return false;

    // Some libclang builds keep the raw-string spelling: R"tag(...)tag".
    const size_t Raw = Spelling.find("R\"");
    if (Raw != std::string::npos && Raw <= 2) {
        const size_t Open = Spelling.find('(', Raw + 2);
        if (Open != std::string::npos) {
            const std::string Delim = Spelling.substr(Raw + 2, Open - Raw - 2);
            const std::string Close = ")" + Delim + "\"";
            const size_t End = Spelling.find(Close, Open + 1);
            if (End != std::string::npos) {
                Out = Spelling.substr(Open + 1, End - Open - 1);
                return true;
            }
        }
        return false;
    }

    size_t I = 0;
    while (I < N && (Spelling[I] == 'u' || Spelling[I] == 'U' ||
                     Spelling[I] == 'L' || Spelling[I] == '8'))
        ++I;
    if (I >= N || Spelling[I] != '"') {
        // Already the bare contents (older libclang versions).
        Out = Spelling;
        return true;
    }
    ++I;

    std::string Result;
    while (I < N) {
        const char C = Spelling[I++];
        if (C == '"') {
            Out = Result;
            return true;
        }
        if (C != '\\') {
            Result += C;
            continue;
        }
        if (I >= N)
            break;
        const char E = Spelling[I++];
        switch (E) {
        case 'n': Result += '\n'; break;
        case 't': Result += '\t'; break;
        case 'r': Result += '\r'; break;
        case 'a': Result += '\a'; break;
        case 'b': Result += '\b'; break;
        case 'f': Result += '\f'; break;
        case 'v': Result += '\v'; break;
        case '0': Result += '\0'; break;
        case '\\': Result += '\\'; break;
        case '\'': Result += '\''; break;
        case '"': Result += '"'; break;
        case '?': Result += '?'; break;
        case 'x': {
            unsigned Value = 0;
            int Digits = 0;
            while (I < N && Digits < 2 && hexDigit(Spelling[I]) >= 0) {
                Value = Value * 16 + static_cast<unsigned>(hexDigit(Spelling[I]));
                ++I;
                ++Digits;
            }
            Result += static_cast<char>(Value & 0xff);
            break;
        }
        case 'u':
        case 'U': {
            const int Want = E == 'u' ? 4 : 8;
            unsigned Value = 0;
            int Digits = 0;
            while (I < N && Digits < Want && hexDigit(Spelling[I]) >= 0) {
                Value = Value * 16 + static_cast<unsigned>(hexDigit(Spelling[I++]));
                ++Digits;
            }
            appendUtf8(Result, Value);
            break;
        }
        default:
            if (E >= '0' && E <= '7') {
                unsigned Value = static_cast<unsigned>(E - '0');
                int Digits = 1;
                while (I < N && Digits < 3 && Spelling[I] >= '0' &&
                       Spelling[I] <= '7') {
                    Value = Value * 8 + static_cast<unsigned>(Spelling[I++] - '0');
                    ++Digits;
                }
                Result += static_cast<char>(Value & 0xff);
            } else {
                Result += E; // unknown escape: keep the escaped character
            }
            break;
        }
    }
    Out = Result;
    return true;
}

std::string upperAscii(std::string S) {
    for (char &C : S)
        C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
    return S;
}

/// Heuristic used only by --scan-literals: the first word after optional SQL
/// comments must be a statement-starting keyword and something must follow it.
bool looksLikeSql(const std::string &Text) {
    static const char *Keywords[] = {
        "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP",
        "ALTER",  "PRAGMA", "WITH",    "REPLACE", "VACUUM", "ATTACH",
        "DETACH", "BEGIN",  "COMMIT",  "ROLLBACK", "EXPLAIN", "ANALYZE",
        "REINDEX"};
    size_t I = 0;
    const size_t N = Text.size();
    for (;;) {
        while (I < N && std::isspace(static_cast<unsigned char>(Text[I])))
            ++I;
        if (I + 1 < N && Text[I] == '-' && Text[I + 1] == '-') {
            I += 2;
            while (I < N && Text[I] != '\n')
                ++I;
            continue;
        }
        if (I + 1 < N && Text[I] == '/' && Text[I + 1] == '*') {
            const size_t End = Text.find("*/", I + 2);
            I = End == std::string::npos ? N : End + 2;
            continue;
        }
        break;
    }
    size_t Start = I;
    while (I < N && (std::isalnum(static_cast<unsigned char>(Text[I])) ||
                     Text[I] == '_'))
        ++I;
    if (I == Start)
        return false;
    const std::string Word = upperAscii(Text.substr(Start, I - Start));
    bool Known = false;
    for (const char *K : Keywords)
        if (Word == K) {
            Known = true;
            break;
        }
    if (!Known)
        return false;
    while (I < N && std::isspace(static_cast<unsigned char>(Text[I])))
        ++I;
    return I < N;
}

//===----------------------------------------------------------------------===//
// Static evaluation of the SQL argument
//===----------------------------------------------------------------------===//

struct Value {
    std::string text;
    bool complete = true;
};

void appendDynamicPart(Value &V) {
    static const std::string Marker = "<dynamic>";
    if (V.text.size() < Marker.size() ||
        V.text.compare(V.text.size() - Marker.size(), Marker.size(), Marker) !=
            0)
        V.text += Marker;
    V.complete = false;
}

bool hasSqlTemplate(const std::string &Text) {
    std::string Candidate = Text;
    size_t Pos = 0;
    while ((Pos = Candidate.find("<dynamic>", Pos)) != std::string::npos) {
        Candidate.replace(Pos, 9, "value");
        Pos += 5;
    }
    return looksLikeSql(Candidate);
}

bool isTransparent(CXCursorKind K) {
    switch (K) {
    case CXCursor_UnexposedExpr:
    case CXCursor_ParenExpr:
    case CXCursor_UnaryOperator:
    case CXCursor_BinaryOperator:
    case CXCursor_CompoundAssignOperator:
    case CXCursor_CStyleCastExpr:
    case CXCursor_CXXStaticCastExpr:
    case CXCursor_CXXDynamicCastExpr:
    case CXCursor_CXXReinterpretCastExpr:
    case CXCursor_CXXConstCastExpr:
    case CXCursor_CXXFunctionalCastExpr:
    case CXCursor_CompoundLiteralExpr:
    case CXCursor_InitListExpr:
    case CXCursor_StmtExpr:
        return true;
    default:
        return false;
    }
}

/// An unresolved child of this kind may still contribute SQL text at runtime,
/// so a value built around it is only partial.
bool mayContribute(CXCursorKind K) {
    switch (K) {
    case CXCursor_DeclRefExpr:
    case CXCursor_MemberRefExpr:
    case CXCursor_CallExpr:
    case CXCursor_UnexposedExpr:
    case CXCursor_UnaryOperator:
    case CXCursor_BinaryOperator:
    case CXCursor_CompoundAssignOperator:
    case CXCursor_ConditionalOperator:
    case CXCursor_ArraySubscriptExpr:
    case CXCursor_CXXThisExpr:
        return true;
    default:
        return false;
    }
}

/// Implicit default arguments and empty placeholder expressions carry no
/// SQL, so an unresolved one must not downgrade a value to "partial".
bool unresolvedContributes(CXCursor C) {
    const CXCursorKind K = clang_getCursorKind(C);
    if (K == CXCursor_UnexposedExpr && children(C).empty())
        return false;
    return mayContribute(K);
}

class Resolver {
public:
    /// Literals reached while resolving an API argument are also "consumed":
    /// --scan-literals must not report a constant twice (once at its
    /// definition, once at the sqlite3 call that uses it).
    explicit Resolver(std::unordered_set<std::string> *Consumed)
        : Consumed_(Consumed) {}

    bool eval(CXCursor C, Value &V, int Depth = 0,
              CXCursor Use = clang_getNullCursor()) {
        if (Depth > 32 || clang_Cursor_isNull(C))
            return false;
        const CXCursorKind K = clang_getCursorKind(C);

        // Two alternative SQL branches are chosen at runtime; concatenating
        // them would fabricate a statement that never reaches sqlite3.
        if (K == CXCursor_ConditionalOperator)
            return false;

        if (K == CXCursor_StringLiteral) {
            std::string Text;
            if (!decodeLiteral(toStd(clang_getCursorSpelling(C)), Text))
                return false;
            if (Consumed_)
                Consumed_->insert(locationKey(expansionLocation(C)));
            V.text = std::move(Text);
            V.complete = true;
            return true;
        }

        if (K == CXCursor_DeclRefExpr || K == CXCursor_MemberRefExpr) {
            CXCursor Ref = clang_getCursorReferenced(C);
            if (!clang_Cursor_isNull(Ref)) {
                const CXCursorKind RK = clang_getCursorKind(Ref);
                if (RK == CXCursor_VarDecl || RK == CXCursor_FieldDecl)
                    return evalDecl(Ref, V, Depth + 1, Use);
            }
            // A member-call callee ("sql.c_str()") carries the object
            // expression as its child, invisible in getNumArguments().
            if (K == CXCursor_MemberRefExpr)
                return evalChildren(C, V, Depth + 1, Use);
            return false;
        }

        if (K == CXCursor_CallExpr) {
            const int NumArgs = clang_Cursor_getNumArguments(C);
            Value Acc;
            bool Found = false;
            for (int I = 0; I < NumArgs; ++I) {
                CXCursor Arg = clang_Cursor_getArgument(C, static_cast<unsigned>(I));
                Value Part;
                if (eval(Arg, Part, Depth + 1, Use)) {
                    Acc.text += Part.text;
                    Acc.complete = Acc.complete && Part.complete;
                    Found = true;
                } else if (unresolvedContributes(Arg)) {
                    appendDynamicPart(Acc);
                }
            }
            if (!Found) {
                // Wrapper conversions (std::string::c_str(), data(), ...)
                // hold the SQL in the callee's object expression.
                const std::vector<CXCursor> Kids = children(C);
                if (!Kids.empty()) {
                    Value Object;
                    if (eval(Kids.front(), Object, Depth + 1, Use)) {
                        V = std::move(Object);
                        return true;
                    }
                }
                return false;
            }
            V = std::move(Acc);
            return true;
        }

        if (isTransparent(K))
            return evalChildren(C, V, Depth + 1, Use);
        return false;
    }

private:
    struct Mutation {
        unsigned endOffset;
        std::string text;
    };

    static unsigned cursorOffset(CXCursor C, bool End) {
        CXSourceRange Range = clang_getCursorExtent(C);
        CXSourceLocation Loc =
            End ? clang_getRangeEnd(Range) : clang_getRangeStart(Range);
        unsigned Offset = 0;
        clang_getExpansionLocation(Loc, nullptr, nullptr, nullptr, &Offset);
        return Offset;
    }

    static bool references(CXCursor C, CXCursor Decl, int Depth = 0) {
        if (Depth > 32 || clang_Cursor_isNull(C))
            return false;
        if (clang_equalCursors(clang_getCursorReferenced(C), Decl))
            return true;
        for (CXCursor Child : children(C))
            if (references(Child, Decl, Depth + 1))
                return true;
        return false;
    }

    static CXCursor enclosingFunction(CXCursor C) {
        for (CXCursor Parent = clang_getCursorSemanticParent(C);
             !clang_Cursor_isNull(Parent);
             Parent = clang_getCursorSemanticParent(Parent))
            if (isFunctionKind(clang_getCursorKind(Parent)))
                return Parent;
        return clang_getNullCursor();
    }

    void collectMutations(CXCursor C, CXCursor Decl, CXCursor Use,
                          std::vector<Mutation> &Out, int Depth) {
        if (Depth > 256 || clang_Cursor_isNull(C) ||
            cursorOffset(C, false) >= cursorOffset(Use, false))
            return;

        const CXCursorKind K = clang_getCursorKind(C);
        if (K == CXCursor_ForStmt || K == CXCursor_WhileStmt ||
            K == CXCursor_DoStmt) {
            std::vector<Mutation> Body;
            for (CXCursor Child : children(C))
                collectMutations(Child, Decl, Use, Body, Depth + 1);
            if (!Body.empty())
                Out.push_back({cursorOffset(C, true), "<dynamic>"});
            return;
        }
        if (K == CXCursor_CompoundAssignOperator) {
            const std::vector<CXCursor> Kids = children(C);
            if (Kids.size() >= 2 && references(Kids.front(), Decl)) {
                Value Part;
                if (!eval(Kids.back(), Part, 0, Use))
                    appendDynamicPart(Part);
                Out.push_back({cursorOffset(C, true), std::move(Part.text)});
                return;
            }
        } else if (K == CXCursor_CallExpr) {
            const std::string Name = toStd(clang_getCursorSpelling(C));
            const int NumArgs = clang_Cursor_getNumArguments(C);
            const std::vector<CXCursor> Kids = children(C);
            if ((Name == "operator+=" || Name == "+=") && NumArgs == 2 &&
                references(clang_Cursor_getArgument(C, 0), Decl)) {
                Value Part;
                if (!eval(clang_Cursor_getArgument(C, 1), Part, 0, Use))
                    appendDynamicPart(Part);
                Out.push_back({cursorOffset(C, true), std::move(Part.text)});
                return;
            }
            if (Name == "append" && !Kids.empty() && NumArgs > 0 &&
                references(Kids.front(), Decl)) {
                for (int I = 0; I < NumArgs; ++I) {
                    Value Part;
                    CXCursor Arg =
                        clang_Cursor_getArgument(C, static_cast<unsigned>(I));
                    if (!eval(Arg, Part, 0, Use))
                        appendDynamicPart(Part);
                    Out.push_back(
                        {cursorOffset(C, true), std::move(Part.text)});
                }
                return;
            }
            if ((Name == "operator<<" || Name == "<<") && NumArgs == 2 &&
                references(clang_Cursor_getArgument(C, 0), Decl)) {
                Value Part;
                if (!eval(clang_Cursor_getArgument(C, 1), Part, 0, Use))
                    appendDynamicPart(Part);
                Out.push_back({cursorOffset(C, true), std::move(Part.text)});
            }
            if (Name == "snprintf" && NumArgs >= 3 &&
                references(clang_Cursor_getArgument(C, 0), Decl)) {
                Value Format;
                if (eval(clang_Cursor_getArgument(C, 2), Format, 0, Use)) {
                    size_t Pos = 0;
                    while ((Pos = Format.text.find('%', Pos)) !=
                           std::string::npos) {
                        if (Pos + 1 < Format.text.size() &&
                            std::isalpha(static_cast<unsigned char>(
                                Format.text[Pos + 1]))) {
                            Format.text.replace(Pos, 2, "<dynamic>");
                            Pos += 9;
                            Format.complete = false;
                        } else {
                            ++Pos;
                        }
                    }
                    Out.push_back(
                        {cursorOffset(C, true), std::move(Format.text)});
                    return;
                }
            }
        }

        for (CXCursor Child : children(C))
            collectMutations(Child, Decl, Use, Out, Depth + 1);
    }

    bool evalChildren(CXCursor C, Value &V, int Depth, CXCursor Use) {
        Value Acc;
        bool Found = false;
        for (CXCursor Child : children(C)) {
            Value Part;
            if (eval(Child, Part, Depth + 1, Use)) {
                Acc.text += Part.text;
                Acc.complete = Acc.complete && Part.complete;
                Found = true;
            } else if (unresolvedContributes(Child)) {
                appendDynamicPart(Acc);
            }
        }
        if (!Found)
            return false;
        V = std::move(Acc);
        return true;
    }

    bool evalDecl(CXCursor Decl, Value &V, int Depth, CXCursor Use) {
        const std::string Key = cursorKey(Decl);
        if (!Active_.insert(Key).second)
            return false; // self-referential initialiser
        Value Initial;
        const bool HasInitial = evalChildren(Decl, Initial, Depth + 1, Use);
        std::vector<Mutation> Mutations;
        CXCursor Function = enclosingFunction(Decl);
        if (!clang_Cursor_isNull(Function))
            collectMutations(Function, Decl, Use, Mutations, 0);
        Active_.erase(Key);
        if (Mutations.empty()) {
            if (HasInitial)
                V = std::move(Initial);
            return HasInitial;
        }

        V = HasInitial ? std::move(Initial) : Value{};
        std::stable_sort(Mutations.begin(), Mutations.end(),
                         [](const Mutation &A, const Mutation &B) {
                             return A.endOffset < B.endOffset;
                         });
        for (const Mutation &Part : Mutations) {
            V.text += Part.text;
            V.complete = false;
        }
        return true;
    }

    std::unordered_set<std::string> *Consumed_ = nullptr;
    std::unordered_set<std::string> Active_;
};

//===----------------------------------------------------------------------===//
// SQL producers: functions, virtual overrides and callable variables
//===----------------------------------------------------------------------===//

struct LiteralInfo {
    std::string text; ///< decoded SQL text
    std::string key;  ///< source-location key, for --scan-literals de-dup
};

struct ProducerIndex {
    std::unordered_map<std::string, std::vector<LiteralInfo>> returns;
    std::unordered_map<std::string, std::string> names;
    std::unordered_map<std::string, std::vector<std::string>> overrides;
    std::unordered_map<std::string, std::vector<std::string>> varFuncs;
    std::unordered_map<std::string, std::vector<LiteralInfo>> varSql;
};

std::string usrOf(CXCursor C) { return toStd(clang_getCursorUSR(C)); }

bool inUserCode(CXCursor C) {
    return clang_Location_isInSystemHeader(clang_getCursorLocation(C)) == 0;
}

/// printf-style / brace-style placeholders mark a SQL template: its text is
/// static, but sqlite3 receives a substituted statement at runtime.
bool hasFormatPlaceholder(const std::string &S) {
    for (size_t I = 0; I + 1 < S.size(); ++I) {
        if (S[I] == '{') {
            const char Next = S[I + 1];
            if (Next == '}' || Next == ':' ||
                std::isdigit(static_cast<unsigned char>(Next)))
                if (S.find('}', I + 1) != std::string::npos)
                    return true;
        }
        if (S[I] == '%') {
            const char C = S[I + 1];
            if (C == 's' || C == 'd' || C == 'i' || C == 'u' || C == 'f' ||
                C == 'q' || C == 'Q' || C == 'z' || C == 'p' || C == 'x')
                return true;
        }
    }
    return false;
}

void collectSqlLiterals(CXCursor C, std::vector<LiteralInfo> &Out, int Depth) {
    if (Depth > 24 || clang_Cursor_isNull(C))
        return;
    if (clang_getCursorKind(C) == CXCursor_StringLiteral) {
        std::string Text;
        if (decodeLiteral(toStd(clang_getCursorSpelling(C)), Text) &&
            looksLikeSql(Text)) {
            const std::string Key = locationKey(expansionLocation(C));
            if (std::none_of(Out.begin(), Out.end(), [&](const LiteralInfo &L) {
                    return L.key == Key;
                }))
                Out.push_back({std::move(Text), Key});
        }
    }
    for (CXCursor Child : children(C))
        collectSqlLiterals(Child, Out, Depth + 1);
}

class IndexBuilder {
public:
    explicit IndexBuilder(ProducerIndex &Index) : Index_(Index) {}

    void build(CXTranslationUnit TU) {
        visit(clang_getTranslationUnitCursor(TU), 0);
        closeOverrides();
    }

private:
    void visit(CXCursor C, int Depth) {
        if (Depth > 256 || clang_Cursor_isNull(C))
            return;
        const CXCursorKind K = clang_getCursorKind(C);
        if (inUserCode(C)) {
            if (isFunctionKind(K))
                indexFunction(C);
            else if (K == CXCursor_VarDecl)
                indexVariable(C);
        }
        for (CXCursor Child : children(C))
            visit(Child, Depth + 1);
    }

    void indexFunction(CXCursor C) {
        const std::string Usr = usrOf(C);
        if (Usr.empty())
            return;
        Index_.names[Usr] = qualifiedName(C);
        std::vector<LiteralInfo> Sql;
        collectReturnLiterals(C, Sql, 0);
        if (!Sql.empty())
            Index_.returns[Usr] = std::move(Sql);
        if (clang_getCursorKind(C) == CXCursor_CXXMethod) {
            CXCursor *Overridden = nullptr;
            unsigned Num = 0;
            clang_getOverriddenCursors(C, &Overridden, &Num);
            for (unsigned I = 0; I < Num; ++I) {
                const std::string Base = usrOf(Overridden[I]);
                if (!Base.empty() && Base != Usr)
                    Index_.overrides[Base].push_back(Usr);
            }
            clang_disposeOverriddenCursors(Overridden);
        }
    }

    void collectReturnLiterals(CXCursor C, std::vector<LiteralInfo> &Out,
                               int Depth) {
        if (Depth > 64 || clang_Cursor_isNull(C))
            return;
        if (clang_getCursorKind(C) == CXCursor_ReturnStmt) {
            std::vector<LiteralInfo> Found;
            collectSqlLiterals(C, Found, 0);
            for (LiteralInfo &L : Found)
                if (std::none_of(Out.begin(), Out.end(),
                                 [&](const LiteralInfo &E) {
                                     return E.key == L.key;
                                 }))
                    Out.push_back(std::move(L));
            return;
        }
        for (CXCursor Child : children(C))
            collectReturnLiterals(Child, Out, Depth + 1);
    }

    void indexVariable(CXCursor C) {
        const std::string Usr = usrOf(C);
        if (Usr.empty())
            return;
        std::vector<LiteralInfo> Sql;
        std::vector<std::string> Funcs;
        collectVarInfo(C, Sql, Funcs, 0);
        if (!Sql.empty())
            Index_.varSql[Usr] = std::move(Sql);
        if (!Funcs.empty())
            Index_.varFuncs[Usr] = std::move(Funcs);
    }

    void collectVarInfo(CXCursor C, std::vector<LiteralInfo> &Sql,
                        std::vector<std::string> &Funcs, int Depth) {
        if (Depth > 24 || clang_Cursor_isNull(C))
            return;
        const CXCursorKind K = clang_getCursorKind(C);
        if (K == CXCursor_StringLiteral) {
            std::string Text;
            if (decodeLiteral(toStd(clang_getCursorSpelling(C)), Text) &&
                looksLikeSql(Text)) {
                const std::string Key = locationKey(expansionLocation(C));
                if (std::none_of(Sql.begin(), Sql.end(),
                                 [&](const LiteralInfo &L) {
                                     return L.key == Key;
                                 }))
                    Sql.push_back({std::move(Text), Key});
            }
        } else if (K == CXCursor_DeclRefExpr) {
            CXCursor Ref = clang_getCursorReferenced(C);
            if (!clang_Cursor_isNull(Ref) &&
                isFunctionKind(clang_getCursorKind(Ref))) {
                const std::string Usr = usrOf(Ref);
                if (!Usr.empty() &&
                    std::find(Funcs.begin(), Funcs.end(), Usr) == Funcs.end()) {
                    Funcs.push_back(Usr);
                    Index_.names[Usr] = qualifiedName(Ref);
                }
            }
        }
        for (CXCursor Child : children(C))
            collectVarInfo(Child, Sql, Funcs, Depth + 1);
    }

    /// Overrides may be chained (Derived2 -> Derived1 -> Base); a call through
    /// a Base pointer can reach every descendant in the chain.
    void closeOverrides() {
        bool Changed = true;
        while (Changed) {
            Changed = false;
            std::vector<std::pair<std::string, std::string>> Add;
            for (const auto &Entry : Index_.overrides)
                for (const std::string &Child : Entry.second) {
                    const auto It = Index_.overrides.find(Child);
                    if (It == Index_.overrides.end())
                        continue;
                    for (const std::string &Grand : It->second)
                        if (std::find(Entry.second.begin(), Entry.second.end(),
                                      Grand) == Entry.second.end())
                            Add.emplace_back(Entry.first, Grand);
                }
            for (const auto &P : Add) {
                std::vector<std::string> &Target = Index_.overrides[P.first];
                if (std::find(Target.begin(), Target.end(), P.second) ==
                    Target.end()) {
                    Target.push_back(P.second);
                    Changed = true;
                }
            }
        }
    }

    ProducerIndex &Index_;
};

//===----------------------------------------------------------------------===//
// AST collector
//===----------------------------------------------------------------------===//

class Collector {
public:
    Collector(CXTranslationUnit TU, const std::vector<ApiSpec> &Apis,
              bool ScanLiterals, bool IncludeHeaders,
              const ProducerIndex &Index, bool Dispatch)
        : TU_(TU), Apis_(Apis), ScanLiterals_(ScanLiterals),
          IncludeHeaders_(IncludeHeaders),
          MainFile_(toStd(clang_getTranslationUnitSpelling(TU))),
          Index_(Index), Dispatch_(Dispatch), Resolver_(&Consumed_) {}

    void collect() {
        visit(clang_getTranslationUnitCursor(TU_), 0);
        if (PendingLiterals_.empty())
            return;
        // A literal seen before the call that consumes it (e.g. a constant
        // defined at the top of the file) can only be filtered afterwards.
        std::vector<bool> Keep(Statements.size(), true);
        for (const PendingLiteral &P : PendingLiterals_)
            if (Consumed_.count(P.key))
                Keep[P.index] = false;
        std::vector<SqlStatement> Filtered;
        Filtered.reserve(Statements.size());
        for (size_t I = 0; I < Statements.size(); ++I)
            if (Keep[I])
                Filtered.push_back(std::move(Statements[I]));
        Statements = std::move(Filtered);
    }

    std::vector<SqlStatement> Statements;
    size_t Unresolved = 0;

private:
    bool inScope(CXCursor C) const {
        CXSourceLocation Loc = clang_getCursorLocation(C);
        if (clang_Location_isInSystemHeader(Loc))
            return false;
        if (IncludeHeaders_ || clang_Location_isFromMainFile(Loc))
            return true;
        // Cursor created inside a macro defined in a header: use the expansion
        // file, which is where the user actually invoked the API.
        Location L = expansionLocation(C);
        return !L.file.empty() && !MainFile_.empty() && L.file == MainFile_;
    }

    const ApiSpec *findApi(const std::string &Simple,
                           const std::string &Qualified) const {
        for (const ApiSpec &Api : Apis_) {
            const size_t Sep = Api.name.rfind("::");
            const std::string Last =
                Sep == std::string::npos ? Api.name : Api.name.substr(Sep + 2);
            if (Simple != Last)
                continue;
            if (Sep != std::string::npos && !Qualified.empty() &&
                Qualified != Api.name) {
                const std::string Suffix = "::" + Api.name;
                const bool IsSuffix =
                    Qualified.size() > Suffix.size() &&
                    Qualified.compare(Qualified.size() - Suffix.size(),
                                      Suffix.size(), Suffix) == 0;
                if (!IsSuffix)
                    continue;
            }
            return &Api;
        }
        return nullptr;
    }

    SqlStatement makeStatement(const ApiSpec &Api, const Location &L) const {
        SqlStatement St;
        St.api = Api.name;
        St.origin = "call";
        St.file = L.file;
        St.line = L.line;
        St.column = L.column;
        return St;
    }

    /// First CallExpr inside the SQL argument: the function/method/callable
    /// that produced the text handed to sqlite3.
    CXCursor findProducerCall(CXCursor C, int Depth) const {
        if (Depth > 16 || clang_Cursor_isNull(C))
            return clang_getNullCursor();
        if (clang_getCursorKind(C) == CXCursor_CallExpr)
            return C;
        for (CXCursor Child : children(C)) {
            CXCursor Found = findProducerCall(Child, Depth + 1);
            if (!clang_Cursor_isNull(Found))
                return Found;
        }
        return clang_getNullCursor();
    }

    static bool isProducerRef(CXCursor C) {
        if (clang_Cursor_isNull(C))
            return false;
        const CXCursorKind K = clang_getCursorKind(C);
        return isFunctionKind(K) || K == CXCursor_VarDecl;
    }

    /// The callee of `table[i]()` sits behind an ArraySubscriptExpr, so scan
    /// a few levels of the callee expression for a referenced function or
    /// variable (function pointer, lambda, std::function, dispatch table).
    CXCursor findProducerRef(CXCursor C, int Depth) const {
        if (Depth > 5 || clang_Cursor_isNull(C))
            return clang_getNullCursor();
        CXCursor Ref = clang_getCursorReferenced(C);
        if (isProducerRef(Ref))
            return Ref;
        for (CXCursor Child : children(C)) {
            CXCursor Found = findProducerRef(Child, Depth + 1);
            if (!clang_Cursor_isNull(Found))
                return Found;
        }
        return clang_getNullCursor();
    }

    CXCursor calleeDecl(CXCursor Call) const {
        const std::vector<CXCursor> Kids = children(Call);
        if (Kids.empty())
            return clang_getNullCursor();
        return findProducerRef(Kids.front(), 0);
    }

    std::string symbolOf(const std::string &Usr) const {
        const auto It = Index_.names.find(Usr);
        return It == Index_.names.end() ? std::string() : It->second;
    }

    void emitProducer(const ApiSpec &Api, const Location &L,
                       const LiteralInfo &Lit, const std::string &Origin,
                       const std::string &Symbol, bool Complete) {
        Consumed_.insert(Lit.key);
        SqlStatement St = makeStatement(Api, L);
        St.origin = Origin;
        St.symbol = Symbol;
        St.sql = Lit.text;
        St.complete = Complete;
        Statements.push_back(std::move(St));
    }

    /// Recover SQL from a producer call (function return, virtual overrides,
    /// function pointer, lambda variable), or from static fragments of the
    /// argument expression. Returns true when at least one statement was
    /// emitted; otherwise the caller reports a dynamic SQL argument.
    bool dispatch(CXCursor Arg, const ApiSpec &Api, const Location &L) {
        const CXCursor Producer = findProducerCall(Arg, 0);
        if (!clang_Cursor_isNull(Producer)) {
            const CXCursor Callee = calleeDecl(Producer);
            std::vector<std::string> Targets;
            std::string VarUsr, VarName;
            if (!clang_Cursor_isNull(Callee)) {
                const CXCursorKind CK = clang_getCursorKind(Callee);
                if (isFunctionKind(CK)) {
                    const std::string Usr = usrOf(Callee);
                    if (!Usr.empty())
                        Targets.push_back(Usr);
                } else if (CK == CXCursor_VarDecl) {
                    VarUsr = usrOf(Callee);
                    VarName = toStd(clang_getCursorSpelling(Callee));
                    const auto It = Index_.varFuncs.find(VarUsr);
                    if (It != Index_.varFuncs.end())
                        Targets = It->second;
                }
            }

            bool HasOverrides = false;
            std::vector<std::string> Candidates;
            for (const std::string &Target : Targets) {
                Candidates.push_back(Target);
                const auto It = Index_.overrides.find(Target);
                if (It != Index_.overrides.end()) {
                    HasOverrides = HasOverrides || !It->second.empty();
                    for (const std::string &Override : It->second)
                        if (std::find(Candidates.begin(), Candidates.end(),
                                      Override) == Candidates.end())
                            Candidates.push_back(Override);
                }
            }
            // Several possible producers (virtual dispatch or a function
            // pointer table) are alternatives, not one complete statement.
            const bool Multiple = HasOverrides || Candidates.size() > 1;
            const std::string ProducerOrigin =
                HasOverrides ? "virtual" : "function";

            size_t Emitted = 0;
            for (const std::string &Candidate : Candidates) {
                const auto It = Index_.returns.find(Candidate);
                if (It == Index_.returns.end())
                    continue;
                for (const LiteralInfo &Lit : It->second) {
                    emitProducer(Api, L, Lit, ProducerOrigin,
                                 symbolOf(Candidate), !Multiple);
                    ++Emitted;
                }
            }
            if (!VarUsr.empty()) {
                const auto It = Index_.varSql.find(VarUsr);
                if (It != Index_.varSql.end())
                    for (const LiteralInfo &Lit : It->second) {
                        emitProducer(Api, L, Lit, "callable", VarName, true);
                        ++Emitted;
                    }
            }
            if (Emitted)
                return true;
        }

        // Formatted / conditionally chosen / partially concatenated SQL:
        // report each static SQL-looking literal as a separate fragment,
        // never merge them into a statement that never runs.
        std::vector<LiteralInfo> Fragments;
        collectSqlLiterals(Arg, Fragments, 0);
        for (const LiteralInfo &Lit : Fragments) {
            Consumed_.insert(Lit.key);
            SqlStatement St = makeStatement(Api, L);
            St.origin = "fragment";
            St.sql = Lit.text;
            St.complete = false;
            Statements.push_back(std::move(St));
        }
        return !Fragments.empty();
    }

    void handleCall(CXCursor Call) {
        if (!inScope(Call))
            return;
        const std::string Simple = toStd(clang_getCursorSpelling(Call));
        if (Simple.empty())
            return;

        std::string Qualified;
        const std::vector<CXCursor> Kids = children(Call);
        if (!Kids.empty()) {
            CXCursor Decl = findFunctionRef(Kids.front(), 0);
            if (!clang_Cursor_isNull(Decl))
                Qualified = qualifiedName(Decl);
        }
        const ApiSpec *Api = findApi(Simple, Qualified);
        if (!Api)
            return;

        const int NumArgs = clang_Cursor_getNumArguments(Call);
        if (Api->sqlArg < 0 || Api->sqlArg >= NumArgs)
            return;
        CXCursor Arg = clang_Cursor_getArgument(Call,
                                                static_cast<unsigned>(Api->sqlArg));
        // Resolver marks every literal it consumed. A dynamic argument keeps
        // its literals visible to --scan-literals instead of hiding them.
        Location L = expansionLocation(Call);
        Value V;
        if (Resolver_.eval(Arg, V, 0, Call) &&
            (V.complete || (Dispatch_ && hasSqlTemplate(V.text)))) {
            SqlStatement St = makeStatement(*Api, L);
            St.sql = std::move(V.text);
            St.complete = V.complete;
            if (Dispatch_ && hasFormatPlaceholder(St.sql)) {
                St.origin = "format";
                St.complete = false;
            } else if (!St.complete) {
                St.origin = "fragment";
            }
            Statements.push_back(std::move(St));
            return;
        }
        if (Dispatch_ && dispatch(Arg, *Api, L))
            return;

        SqlStatement St = makeStatement(*Api, L);
        St.resolved = false;
        St.complete = false;
        ++Unresolved;
        Statements.push_back(std::move(St));
    }

    void handleLiteral(CXCursor Lit) {
        if (!inScope(Lit))
            return;
        Location L = expansionLocation(Lit);
        if (Consumed_.count(locationKey(L)))
            return;
        std::string Text;
        if (!decodeLiteral(toStd(clang_getCursorSpelling(Lit)), Text))
            return;
        if (!looksLikeSql(Text))
            return;

        SqlStatement St;
        St.origin = "literal";
        St.sql = std::move(Text);
        St.file = L.file;
        St.line = L.line;
        St.column = L.column;
        PendingLiterals_.push_back({locationKey(L), Statements.size()});
        Statements.push_back(std::move(St));
    }

    void visit(CXCursor C, int Depth) {
        if (Depth > 256 || clang_Cursor_isNull(C))
            return;
        const CXCursorKind K = clang_getCursorKind(C);
        if (K == CXCursor_CallExpr)
            handleCall(C);
        else if (K == CXCursor_StringLiteral && ScanLiterals_)
            handleLiteral(C);
        for (CXCursor Child : children(C))
            visit(Child, Depth + 1);
    }

    struct PendingLiteral {
        std::string key;
        size_t index;
    };

    CXTranslationUnit TU_;
    const std::vector<ApiSpec> &Apis_;
    bool ScanLiterals_;
    bool IncludeHeaders_;
    std::string MainFile_;
    const ProducerIndex &Index_;
    bool Dispatch_;
    std::unordered_set<std::string> Consumed_;
    Resolver Resolver_;
    std::vector<PendingLiteral> PendingLiterals_;
};

//===----------------------------------------------------------------------===//
// Translation-unit parsing
//===----------------------------------------------------------------------===//

bool hasArg(const std::vector<std::string> &Args, const char *Prefix) {
    for (const std::string &A : Args)
        if (A.rfind(Prefix, 0) == 0)
            return true;
    return false;
}

std::string extensionOf(const std::string &Path) {
    const size_t Slash = Path.find_last_of('/');
    const size_t Dot = Path.find_last_of('.');
    if (Dot == std::string::npos ||
        (Slash != std::string::npos && Dot < Slash))
        return std::string();
    return Path.substr(Dot);
}

/// libclang may come from an LLVM build tree whose compiled-in resource
/// directory (builtin headers such as stddef.h) does not exist. Locate it next
/// to the libclang shared object that is actually loaded.
std::string detectResourceDir() {
    void *Sym = dlsym(RTLD_DEFAULT, "clang_createIndex");
    Dl_info Info{};
    if (!Sym || dladdr(Sym, &Info) == 0 || !Info.dli_fname)
        return std::string();
    std::string Lib = Info.dli_fname;
    if (Lib.empty() || Lib[0] != '/')
        return std::string();
    const size_t Slash = Lib.find_last_of('/');
    const std::string ClangDir = Lib.substr(0, Slash) + "/clang";
    DIR *Dir = opendir(ClangDir.c_str());
    if (!Dir)
        return std::string();

    std::string Best;
    while (struct dirent *Entry = readdir(Dir)) {
        std::string Version = Entry->d_name;
        if (Version.empty() || Version[0] == '.')
            continue;
        const std::string Header =
            ClangDir + "/" + Version + "/include/stddef.h";
        if (access(Header.c_str(), R_OK) != 0)
            continue;
        if (Best.empty() || std::atoi(Version.c_str()) > std::atoi(Best.c_str()))
            Best = Version;
    }
    closedir(Dir);
    return Best.empty() ? std::string() : ClangDir + "/" + Best;
}

CXTranslationUnit parse(const std::string &Path, bool IsC,
                        const std::vector<std::string> &ExtraArgs) {
    static const std::string ResourceArg = [] {
        const std::string Dir = detectResourceDir();
        return Dir.empty() ? std::string() : "-resource-dir=" + Dir;
    }();

    std::vector<std::string> ArgStore;
    if (!hasArg(ExtraArgs, "-x")) {
        ArgStore.push_back("-x");
        ArgStore.push_back(IsC ? "c" : "c++");
    }
    if (!hasArg(ExtraArgs, "-std="))
        ArgStore.push_back(IsC ? "-std=c11" : "-std=c++17");
    if (!ResourceArg.empty() && !hasArg(ExtraArgs, "-resource-dir"))
        ArgStore.push_back(ResourceArg);
    ArgStore.insert(ArgStore.end(), ExtraArgs.begin(), ExtraArgs.end());

    std::vector<const char *> Args;
    Args.reserve(ArgStore.size());
    for (const std::string &A : ArgStore)
        Args.push_back(A.c_str());

    CXIndex Index = clang_createIndex(/*excludeDeclarationsFromPCH=*/0,
                                      /*displayDiagnostics=*/0);
    CXTranslationUnit TU = nullptr;
    const CXErrorCode Err = clang_parseTranslationUnit2(
        Index, Path.c_str(), Args.data(), static_cast<int>(Args.size()),
        /*unsaved_files=*/nullptr, /*num_unsaved_files=*/0,
        CXTranslationUnit_None, &TU);
    clang_disposeIndex(Index);

    if (Err != CXError_Success || !TU) {
        std::fprintf(stderr,
                     "error: cannot parse %s (CXErrorCode %d); add the missing "
                     "include paths after --\n",
                     Path.c_str(), static_cast<int>(Err));
        if (TU)
            clang_disposeTranslationUnit(TU);
        return nullptr;
    }

    // Incomplete compile flags are common when analysing a foreign source
    // tree: report diagnostics but still extract what libclang could parse.
    const unsigned NumDiags = clang_getNumDiagnostics(TU);
    unsigned Errors = 0;
    for (unsigned I = 0; I < NumDiags; ++I) {
        CXDiagnostic Diag = clang_getDiagnostic(TU, I);
        if (clang_getDiagnosticSeverity(Diag) >= CXDiagnostic_Error) {
            if (Errors < 10) {
                CXString Text = clang_formatDiagnostic(
                    Diag, clang_defaultDiagnosticDisplayOptions());
                std::fprintf(stderr, "%s\n", clang_getCString(Text));
                clang_disposeString(Text);
            }
            ++Errors;
        }
        clang_disposeDiagnostic(Diag);
    }
    if (Errors > 10)
        std::fprintf(stderr, "warning: %u more clang diagnostics suppressed\n",
                     Errors - 10);
    return TU;
}

std::string trim(const std::string &S) {
    size_t Begin = 0, End = S.size();
    while (Begin < End &&
           std::isspace(static_cast<unsigned char>(S[Begin])))
        ++Begin;
    while (End > Begin &&
           std::isspace(static_cast<unsigned char>(S[End - 1])))
        --End;
    return S.substr(Begin, End - Begin);
}

} // namespace

namespace sqlinfo {

bool extractSqlFromSource(const SqlOptions &options, SqlReport &report,
                          std::string &error) {
    // Explicit --api entries take precedence over the built-in table, so a
    // user can override an argument index (e.g. --api sqlite3_exec:0).
    std::vector<ApiSpec> Apis;
    for (const ApiSpec &Custom : options.customApis) {
        const auto It = std::find_if(
            Apis.begin(), Apis.end(),
            [&](const ApiSpec &A) { return A.name == Custom.name; });
        if (It == Apis.end())
            Apis.push_back(Custom);
        else
            *It = Custom;
    }
    if (options.useBuiltinApis)
        for (const ApiSpec &Builtin : builtinSqliteApis()) {
            const bool Exists =
                std::any_of(Apis.begin(), Apis.end(), [&](const ApiSpec &A) {
                    return A.name == Builtin.name;
                });
            if (!Exists)
                Apis.push_back(Builtin);
        }

    report.inputs = options.inputs;
    report.apis = Apis;

    std::vector<std::string> Failed;
    for (const std::string &Src : options.inputs) {
        const bool IsC = extensionOf(Src) == ".c";
        CXTranslationUnit TU = parse(Src, IsC, options.clangArgs);
        if (!TU) {
            Failed.push_back(Src);
            continue;
        }
        ProducerIndex Index;
        IndexBuilder(Index).build(TU);
        Collector Instance(TU, Apis, options.scanLiterals,
                            options.includeHeaders, Index, options.dispatch);
        Instance.collect();
        for (SqlStatement &St : Instance.Statements)
            report.statements.push_back(std::move(St));
        report.unresolved += Instance.Unresolved;
        clang_disposeTranslationUnit(TU);
    }

    if (options.unique) {
        std::unordered_set<std::string> Seen;
        std::vector<SqlStatement> Unique;
        Unique.reserve(report.statements.size());
        for (SqlStatement &St : report.statements)
            if (Seen.insert(trim(St.sql)).second)
                Unique.push_back(std::move(St));
        report.statements = std::move(Unique);
        report.unresolved = 0;
        for (const SqlStatement &St : report.statements)
            if (!St.resolved)
                ++report.unresolved;
    }

    report.parseErrors = Failed.size();
    if (!Failed.empty()) {
        error = "cannot parse source file(s): ";
        for (size_t I = 0; I < Failed.size(); ++I) {
            if (I > 0)
                error += ", ";
            error += Failed[I];
        }
        return false;
    }
    return true;
}

} // namespace sqlinfo
