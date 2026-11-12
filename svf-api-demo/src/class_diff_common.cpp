//===----------------------------------------------------------------------===//
// class_diff_common.cpp — shared helpers, input discovery, backend selection,
//              the diff model and the text/JSON report writers for class_diff.
//              This translation unit deliberately depends only on the C++
//              standard library and <filesystem>, never on clang or LLVM, so
//              it stays
//              independent of which analysis backend is in use.
//===----------------------------------------------------------------------===//

#include "class_diff.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace classdiff {

bool startsWith(const std::string& S, const std::string& Prefix) {
    return S.rfind(Prefix, 0) == 0;
}

std::string lower(std::string S) {
    for (char& C : S)
        C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
    return S;
}

/// Canonical path with forward slashes; also works for paths that do not exist.
std::string normPath(const std::string& P) {
    std::error_code EC;
    fs::path C = fs::weakly_canonical(fs::path(P), EC);
    if (EC)
        return fs::path(P).lexically_normal().generic_string();
    return C.generic_string();
}

std::string join(const std::vector<std::string>& Parts, const char* Sep) {
    std::string R;
    for (size_t I = 0; I < Parts.size(); ++I) {
        if (I)
            R += Sep;
        R += Parts[I];
    }
    return R;
}

/// FNV-1a over a normalized token stream. Empty input yields an empty hash,
/// which is how "no declaration" / "no definition" is represented.
std::string fingerprint(const std::vector<std::string>& Tokens) {
    if (Tokens.empty())
        return std::string();
    uint64_t H = 14695981039346656037ULL;
    auto mix = [&H](unsigned char C) {
        H ^= C;
        H *= 1099511628211ULL;
    };
    for (const std::string& T : Tokens) {
        for (unsigned char C : T)
            mix(C);
        mix('\n');
    }
    char Buf[17];
    std::snprintf(Buf, sizeof(Buf), "%016llx",
                  static_cast<unsigned long long>(H));
    return std::string(Buf);
}

std::string jsonEscape(const std::string& S) {
    std::string R;
    R.reserve(S.size() + 2);
    for (unsigned char C : S) {
        switch (C) {
        case '"': R += "\\\""; break;
        case '\\': R += "\\\\"; break;
        case '\n': R += "\\n"; break;
        case '\r': R += "\\r"; break;
        case '\t': R += "\\t"; break;
        default:
            if (C < 0x20) {
                char B[7];
                std::snprintf(B, sizeof(B), "\\u%04x", C);
                R += B;
            } else {
                R += static_cast<char>(C);
            }
        }
    }
    return R;
}

bool isSourceFile(const std::string& Path) {
    const size_t Slash = Path.find_last_of('/');
    const size_t Dot = Path.find_last_of('.');
    if (Dot == std::string::npos ||
        (Slash != std::string::npos && Dot < Slash))
        return false;
    static const std::set<std::string> Exts = {
        ".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh",
        ".hpp", ".hxx", ".h++", ".inc", ".inl", ".ipp", ".tcc"};
    return Exts.count(lower(Path.substr(Dot))) != 0;
}

bool isBitcodeFile(const std::string& Path) {
    const size_t Slash = Path.find_last_of('/');
    const size_t Dot = Path.find_last_of('.');
    if (Dot == std::string::npos ||
        (Slash != std::string::npos && Dot < Slash))
        return false;
    const std::string Ext = lower(Path.substr(Dot));
    return Ext == ".bc" || Ext == ".ll";
}

bool isIgnored(const std::string& Rel, const std::vector<std::string>& Ignores) {
    for (const std::string& F : Ignores)
        if (Rel.find(F) != std::string::npos)
            return true;
    return false;
}

/// Find the clang resource directory by locating the libclang shared object
/// actually loaded and picking the newest versioned sub-directory that ships
/// stddef.h. Builtin headers such as stddef.h are kept there.
std::string detectResourceDir() {
    void* Sym = dlsym(RTLD_DEFAULT, "clang_createIndex");
    Dl_info Info{};
    if (!Sym || dladdr(Sym, &Info) == 0 || !Info.dli_fname ||
        !Info.dli_fname[0])
        return std::string();
    std::string Lib = Info.dli_fname;
    const size_t Slash = Lib.find_last_of('/');
    if (Lib[0] != '/' || Slash == std::string::npos)
        return std::string();
    const std::string ClangDir = Lib.substr(0, Slash) + "/clang";
    DIR* Dir = opendir(ClangDir.c_str());
    if (!Dir)
        return std::string();
    std::string Best;
    while (struct dirent* E = readdir(Dir)) {
        std::string V = E->d_name;
        if (V.empty() || V[0] == '.')
            continue;
        if (access((ClangDir + "/" + V + "/include/stddef.h").c_str(), R_OK) != 0)
            continue;
        if (Best.empty() || std::atoi(V.c_str()) > std::atoi(Best.c_str()))
            Best = V;
    }
    closedir(Dir);
    return Best.empty() ? std::string() : ClangDir + "/" + Best;
}

std::string setJoin(const std::set<std::string>& S) {
    return join(std::vector<std::string>(S.begin(), S.end()), ";");
}

std::string classState(const ClassRecord& C, const char* Part) {
    const std::string Key(Part);
    if (Key == "kind")
        return C.kind;
    if (Key == "bases")
        return setJoin(C.bases);
    if (Key == "fields")
        return setJoin(C.fields);
    return setJoin(C.aliases);
}

bool initProject(const std::string& Arg, const Options& O, Snapshot& S,
                 std::string& Error) {
    std::error_code EC;
    const fs::path Given = fs::absolute(fs::path(Arg), EC);
    if (EC || !fs::exists(Given, EC)) {
        Error = "cannot access project path: " + Arg;
        return false;
    }
    S.label = Arg;
    const std::string File = normPath(Given.string());
    if (fs::is_regular_file(Given, EC)) {
        S.root = normPath(Given.parent_path().string());
        if (!S.root.empty() && S.root.back() != '/')
            S.root += '/';
        if (isBitcodeFile(File))
            S.bitcodes.push_back(File);
        else
            S.sources.push_back(File);
        return true;
    }
    if (!fs::is_directory(Given, EC)) {
        Error = "not a directory or source file: " + Arg;
        return false;
    }
    S.root = normPath(Given.string());
    if (!S.root.empty() && S.root.back() != '/')
        S.root += '/';

    fs::recursive_directory_iterator It(Given, EC), End;
    for (; !EC && It != End; It.increment(EC)) {
        const fs::directory_entry& E = *It;
        const std::string Abs = normPath(E.path().string());
        const std::string Rel =
            startsWith(Abs, S.root)
                ? Abs.substr(S.root.size())
                : E.path().filename().generic_string();
        bool Hidden = false;
        for (const fs::path& Part : fs::path(Rel))
            if (!Part.empty() && Part.generic_string()[0] == '.')
                Hidden = true;
        if (Hidden || isIgnored(Rel, O.ignores)) {
            if (E.is_directory(EC))
                It.disable_recursion_pending();
            continue;
        }
        if (E.is_directory(EC))
            continue;
        if (E.is_regular_file(EC)) {
            if (isBitcodeFile(Rel))
                S.bitcodes.push_back(Abs);
            else if (isSourceFile(Rel))
                S.sources.push_back(Abs);
        }
    }
    for (auto* List : {&S.sources, &S.bitcodes}) {
        std::sort(List->begin(), List->end());
        List->erase(std::unique(List->begin(), List->end()), List->end());
    }
    return true;
}

/// Explicit --input-type wins. In auto mode both inputs must unambiguously
/// consist of the same kind of files; a tree containing sources *and* build
/// artifacts needs an explicit choice.
bool detectBackend(const Options& O, const Snapshot& A, const Snapshot& B,
                   Backend& Out, std::string& Error) {
    if (O.inputType == "source") {
        if (A.sources.empty() || B.sources.empty()) {
            Error = "no C/C++ sources found for --input-type source";
            return false;
        }
        Out = Backend::Source;
        return true;
    }
    if (O.inputType == "bitcode") {
        if (A.bitcodes.empty() || B.bitcodes.empty()) {
            Error = "no .bc/.ll modules found for --input-type bitcode";
            return false;
        }
        Out = Backend::Bitcode;
        return true;
    }
    auto kindOf = [](const Snapshot& S) -> int {
        const bool BC = !S.bitcodes.empty(), Src = !S.sources.empty();
        if (BC && !Src)
            return 1;
        if (Src && !BC)
            return 0;
        return -1;
    };
    const int KA = kindOf(A), KB = kindOf(B);
    if (KA >= 0 && KA == KB) {
        Out = KA == 1 ? Backend::Bitcode : Backend::Source;
        return true;
    }
    Error = "cannot determine the input type (mixed or missing .cpp/.h and "
            ".bc/.ll files); use --input-type source|bitcode";
    return false;
}

const char* backendName(Backend B) {
    return B == Backend::Bitcode ? "bitcode" : "source";
}

size_t fileCount(const Snapshot& S, Backend B) {
    return B == Backend::Bitcode ? S.bitcodes.size() : S.sources.size();
}

const char* changeName(Change C) {
    switch (C) {
    case Change::Added: return "added";
    case Change::Removed: return "removed";
    case Change::Modified: return "modified";
    case Change::Unchanged: return "unchanged";
    }
    return "unchanged";
}

std::vector<std::string> methodReasons(const MethodRecord& A,
                                       const MethodRecord& B) {
    std::vector<std::string> R;
    if (A.traits != B.traits)
        R.push_back("interface");
    if (A.declHash != B.declHash)
        R.push_back("declaration");
    if (A.bodyHash != B.bodyHash)
        R.push_back("implementation");
    return R;
}

std::string locationOf(const MethodRecord& R) {
    std::string S = R.defFile.empty() ? R.declFile : R.defFile;
    const unsigned L = R.defFile.empty() ? R.declLine : R.defLine;
    if (!S.empty() && L)
        S += ":" + std::to_string(L);
    return S.empty() ? "-" : S;
}

static ClassDiff diffClass(const std::string& Name, const ClassRecord* Old,
                           const ClassRecord* New, bool AllMethods) {
    ClassDiff D;
    D.name = Name;
    if (!Old) {
        D.change = Change::Added;
        for (const auto& KV : New->methods)
            D.methods.push_back({Change::Added, KV.second.display, {"class added"},
                                 MethodRecord{}, KV.second});
        return D;
    }
    if (!New) {
        D.change = Change::Removed;
        for (const auto& KV : Old->methods)
            D.methods.push_back({Change::Removed, KV.second.display,
                                 {"class removed"}, KV.second, MethodRecord{}});
        return D;
    }

    for (const char* Part : {"kind", "bases", "fields", "aliases"})
        if (classState(*Old, Part) != classState(*New, Part))
            D.classReasons.push_back(Part);

    std::set<std::string> Keys;
    for (const auto& KV : Old->methods)
        Keys.insert(KV.first);
    for (const auto& KV : New->methods)
        Keys.insert(KV.first);
    for (const std::string& K : Keys) {
        const auto A = Old->methods.find(K);
        const auto B = New->methods.find(K);
        if (A == Old->methods.end()) {
            D.methods.push_back({Change::Added, B->second.display, {},
                                 MethodRecord{}, B->second});
            continue;
        }
        if (B == New->methods.end()) {
            D.methods.push_back({Change::Removed, A->second.display, {},
                                 A->second, MethodRecord{}});
            continue;
        }
        const std::vector<std::string> Reasons =
            methodReasons(A->second, B->second);
        if (!Reasons.empty()) {
            D.methods.push_back({Change::Modified, B->second.display, Reasons,
                                 A->second, B->second});
        } else if (AllMethods) {
            D.methods.push_back({Change::Unchanged, B->second.display, {},
                                 A->second, B->second});
        }
    }
    D.change = (!D.classReasons.empty() ||
                std::any_of(D.methods.begin(), D.methods.end(),
                            [](const MethodDiff& M) {
                                return M.change != Change::Unchanged;
                            }))
                   ? Change::Modified
                   : Change::Unchanged;
    return D;
}

std::vector<ClassDiff> diffSnapshots(const Snapshot& Old, const Snapshot& New,
                                     bool AllMethods) {
    std::set<std::string> Names;
    for (const auto& KV : Old.classes)
        Names.insert(KV.first);
    for (const auto& KV : New.classes)
        Names.insert(KV.first);
    std::vector<ClassDiff> Diffs;
    for (const std::string& N : Names) {
        const auto A = Old.classes.find(N);
        const auto B = New.classes.find(N);
        ClassDiff D = diffClass(N, A == Old.classes.end() ? nullptr : &A->second,
                                B == New.classes.end() ? nullptr : &B->second,
                                AllMethods);
        if (D.change != Change::Unchanged)
            Diffs.push_back(std::move(D));
    }
    return Diffs;
}

void writeText(FILE* Out, const Snapshot& Old, const Snapshot& New,
               const std::vector<ClassDiff>& Diffs, Backend B) {
    std::fprintf(Out, "# old: %s\n# new: %s\n# backend: %s\n",
                 Old.label.c_str(), New.label.c_str(), backendName(B));
    if (B == Backend::Bitcode)
        std::fprintf(Out, "# debug info: old=%s new=%s\n",
                     Old.hasDebugInfo ? "yes" : "no",
                     New.hasDebugInfo ? "yes" : "no");
    if (Diffs.empty()) {
        std::fputs("# no class or method changes\n", Out);
        return;
    }
    for (const ClassDiff& C : Diffs) {
        std::fprintf(Out, "[%s] %s", changeName(C.change), C.name.c_str());
        if (!C.classReasons.empty())
            std::fprintf(Out, "  (class %s changed)",
                         join(C.classReasons, ", ").c_str());
        std::fputc('\n', Out);
        if (C.methods.empty())
            std::fputs("    (no method changes)\n", Out);
        for (const MethodDiff& M : C.methods) {
            const char Mark = M.change == Change::Added      ? '+'
                              : M.change == Change::Removed ? '-'
                              : M.change == Change::Modified ? '~'
                                                            : '=';
            std::fprintf(Out, "  %c %s", Mark, M.display.c_str());
            if (!M.reasons.empty())
                std::fprintf(Out, "  [%s]", join(M.reasons, ", ").c_str());
            std::fputc('\n', Out);
            if (M.change == Change::Modified) {
                std::fprintf(Out, "      old: %s\n      new: %s\n",
                             locationOf(M.oldRec).c_str(),
                             locationOf(M.newRec).c_str());
            } else if (M.change == Change::Added) {
                std::fprintf(Out, "      new: %s\n",
                             locationOf(M.newRec).c_str());
            } else if (M.change == Change::Removed) {
                std::fprintf(Out, "      old: %s\n",
                             locationOf(M.oldRec).c_str());
            }
        }
    }
}

void writeJson(FILE* Out, const Snapshot& Old, const Snapshot& New,
               const std::vector<ClassDiff>& Diffs, Backend B) {
    size_t ChangedMethods = 0;
    for (const ClassDiff& C : Diffs)
        for (const MethodDiff& M : C.methods)
            if (M.change != Change::Unchanged)
                ++ChangedMethods;
    std::fprintf(Out,
                 "{\n"
                 "  \"old_root\": \"%s\",\n"
                 "  \"new_root\": \"%s\",\n"
                 "  \"input_type\": \"%s\",\n",
                 jsonEscape(Old.label).c_str(), jsonEscape(New.label).c_str(),
                 backendName(B));
    if (B == Backend::Bitcode)
        std::fprintf(Out, "  \"debug_info\": {\"old\": %s, \"new\": %s},\n",
                     Old.hasDebugInfo ? "true" : "false",
                     New.hasDebugInfo ? "true" : "false");
    std::fprintf(Out,
                 "  \"summary\": {\"old_files\": %zu, \"new_files\": %zu, "
                 "\"old_classes\": %zu, \"new_classes\": %zu, "
                 "\"changed_classes\": %zu, \"changed_methods\": %zu, "
                 "\"parse_errors\": %zu},\n",
                 fileCount(Old, B), fileCount(New, B), Old.classes.size(),
                 New.classes.size(), Diffs.size(), ChangedMethods,
                 Old.parseErrors + New.parseErrors);
    std::fputs("  \"classes\": [", Out);
    for (size_t I = 0; I < Diffs.size(); ++I) {
        const ClassDiff& C = Diffs[I];
        std::fprintf(Out,
                     "%s\n    {\"name\": \"%s\", \"change\": \"%s\", "
                     "\"class_changes\": [",
                     I ? "," : "", jsonEscape(C.name).c_str(),
                     changeName(C.change));
        for (size_t J = 0; J < C.classReasons.size(); ++J)
            std::fprintf(Out, "%s\"%s\"", J ? ", " : "",
                         jsonEscape(C.classReasons[J]).c_str());
        std::fputs("], \"methods\": [", Out);
        for (size_t J = 0; J < C.methods.size(); ++J) {
            const MethodDiff& M = C.methods[J];
            std::fprintf(Out,
                         "%s\n      {\"name\": \"%s\", \"change\": \"%s\", "
                         "\"reasons\": [",
                         J ? "," : "", jsonEscape(M.display).c_str(),
                         changeName(M.change));
            for (size_t K = 0; K < M.reasons.size(); ++K)
                std::fprintf(Out, "%s\"%s\"", K ? ", " : "",
                             jsonEscape(M.reasons[K]).c_str());
            std::fprintf(Out,
                         "], \"old\": \"%s\", \"new\": \"%s\"}",
                         jsonEscape(locationOf(M.oldRec)).c_str(),
                         jsonEscape(locationOf(M.newRec)).c_str());
        }
        std::fputs("]}", Out);
    }
    std::fputs(Diffs.empty() ? "]\n}\n" : "\n  ]\n}\n", Out);
}

}  // namespace classdiff
