//===----------------------------------------------------------------------===//
// class_diff.h — shared model and declarations for the class/method change
//                detector. Two interchangeable backends (source and bitcode)
//                feed the very same snapshot/diff model; this header is the
//                contract between them, the CLI and the report writers.
//===----------------------------------------------------------------------===//
#pragma once

#include <cstddef>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace classdiff {

enum class Backend { Source, Bitcode };

struct MethodRecord {
    std::string key;       ///< stable identity across the two projects
    std::string display;   ///< human-readable signature
    std::string declFile;  ///< relative path of the in-class declaration
    unsigned declLine = 0;
    std::string defFile;   ///< relative path of the definition (may be empty)
    unsigned defLine = 0;
    std::string declHash;  ///< declaration fingerprint
    std::string bodyHash;  ///< definition fingerprint
    std::string traits;    ///< semantic flags not visible in the token stream
    bool hasDecl = false;
    bool hasDef = false;
};

struct ClassRecord {
    std::string name;
    std::string kind;
    std::string file;
    unsigned line = 0;
    std::set<std::string> bases;
    std::set<std::string> fields;
    std::set<std::string> aliases;
    std::map<std::string, MethodRecord> methods;
};

struct Snapshot {
    std::string root;                    ///< canonical project directory + '/'
    std::string label;                   ///< path as given on the command line
    std::vector<std::string> sources;    ///< absolute C/C++ source paths
    std::vector<std::string> bitcodes;   ///< absolute .bc/.ll module paths
    std::map<std::string, ClassRecord> classes;
    bool hasDebugInfo = false;  ///< bitcode backend: module(s) carry DWARF
    size_t parseErrors = 0;
};

struct Options {
    std::vector<std::string> roots;
    std::vector<std::string> ignores;
    std::vector<std::string> clangArgs;
    std::string format = "text";
    std::string output;
    std::string standard = "c++17";
    std::string inputType = "auto";  ///< auto | source | bitcode
    std::vector<std::string> libPrefixes;  ///< bitcode: extra library prefixes
    bool allMethods = false;
    bool exitCode = false;
    bool useCompilationDB = true;
    bool includeLibrary = false;  ///< bitcode: keep library classes
};

enum class Change { Added, Removed, Modified, Unchanged };

struct MethodDiff {
    Change change = Change::Unchanged;
    std::string display;
    std::vector<std::string> reasons;
    MethodRecord oldRec;
    MethodRecord newRec;
};

struct ClassDiff {
    Change change = Change::Unchanged;
    std::string name;
    std::vector<std::string> classReasons;
    std::vector<MethodDiff> methods;
};

//--- shared helpers (defined in class_diff_common.cpp) ----------------------
bool isSourceFile(const std::string& Path);
bool isBitcodeFile(const std::string& Path);
bool startsWith(const std::string& S, const std::string& Prefix);
std::string normPath(const std::string& P);
std::string lower(std::string S);
std::string join(const std::vector<std::string>& Parts, const char* Sep);
std::string fingerprint(const std::vector<std::string>& Tokens);
std::string jsonEscape(const std::string& S);
const char* backendName(Backend B);
size_t fileCount(const Snapshot& S, Backend B);
std::string setJoin(const std::set<std::string>& S);
std::string classState(const ClassRecord& C, const char* Part);
std::vector<std::string> methodReasons(const MethodRecord& A,
                                       const MethodRecord& B);
std::string locationOf(const MethodRecord& R);
const char* changeName(Change C);

//--- input discovery & backend selection (class_diff_common.cpp) ------------
bool isIgnored(const std::string& Rel, const std::vector<std::string>& Ignores);
bool initProject(const std::string& Arg, const Options& O, Snapshot& S,
                 std::string& Error);
bool detectBackend(const Options& O, const Snapshot& A, const Snapshot& B,
                   Backend& Out, std::string& Error);
std::string detectResourceDir();

//--- diff + report (class_diff_common.cpp) ----------------------------------
std::vector<ClassDiff> diffSnapshots(const Snapshot& Old, const Snapshot& New,
                                     bool AllMethods);
void writeText(FILE* Out, const Snapshot& Old, const Snapshot& New,
               const std::vector<ClassDiff>& Diffs, Backend B);
void writeJson(FILE* Out, const Snapshot& Old, const Snapshot& New,
               const std::vector<ClassDiff>& Diffs, Backend B);

//--- backends ---------------------------------------------------------------
bool hasArg(const std::vector<std::string>& Args, const char* Prefix);  // source
void buildSnapshot(Snapshot& S, const Options& O,
                   const std::string& ResourceArg);  // class_diff_source.cpp
void buildBitcodeSnapshot(Snapshot& S, const Options& O);  // class_diff_bitcode.cpp

//--- CLI (class_diff.cpp) ---------------------------------------------------
bool parseArgs(int argc, char** argv, Options& O, std::string& Error);
void printUsage(const char* Prog);

}  // namespace classdiff
