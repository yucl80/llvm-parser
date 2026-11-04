/*
 * class_dump.cpp - dump C/C++ class structure with Clang (libclang C API).
 *
 * For every class/struct/union defined in the given source files it reports:
 *   - kind, qualified name, source location
 *   - direct base classes (access specifier + virtual inheritance)
 *   - member functions (access, signature, ctor/dtor/static/virtual/pure
 *     virtual/deleted/defaulted/template)
 *   - for each member function, whether it overrides base-class functions and
 *     the exact list of base functions it overrides
 *
 * Build with CMake (target defined in svf-api-demo/CMakeLists.txt):
 *
 *   cmake -S svf-api-demo -B build -DLLVM_PREFIX=/usr/lib/llvm-21
 *   cmake --build build --target class_dump
 *
 * Or compile directly (LLVM 21 with libclang-21-dev; -ldl is needed for the
 * builtin-header resource-dir lookup):
 *
 *   g++ -std=c++17 -O2 svf-api-demo/src/class_dump.cpp \
 *       -I/usr/lib/llvm-21/include \
 *       /usr/lib/llvm-21/lib/libclang.so \
 *       -o class_dump -Wl,-rpath,/usr/lib/llvm-21/lib -ldl
 *
 * Usage:
 *   ./class_dump <source> [more sources ...] [-- <extra clang args>]
 *   ./class_dump --all <source>     # also report classes from headers
 */

#include <clang-c/Index.h>

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

//===----------------------------------------------------------------------===//
// Result model
//===----------------------------------------------------------------------===//

struct BaseInfo {
  std::string name;
  std::string access;  // public / protected / private
  bool isVirtual = false;
};

struct MethodInfo {
  std::string signature;  // e.g. "virtual void speak() const"
  std::string access;
  std::string location;
  std::string flags;                    // ctor, dtor, template, pure virtual, ...
  std::vector<std::string> overridden;  // base functions this one overrides
};

struct ClassInfo {
  std::string name;
  std::string kind;  // class / struct / union / class template
  std::string location;
  std::vector<BaseInfo> bases;
  std::vector<MethodInfo> methods;
};

//===----------------------------------------------------------------------===//
// libclang helpers
//===----------------------------------------------------------------------===//

std::string toStd(CXString S) {
  const char *C = clang_getCString(S);
  std::string R = C ? C : "";
  clang_disposeString(S);
  return R;
}

std::string accessStr(CX_CXXAccessSpecifier A) {
  switch (A) {
  case CX_CXXPublic:
    return "public";
  case CX_CXXProtected:
    return "protected";
  case CX_CXXPrivate:
    return "private";
  case CX_CXXInvalidAccessSpecifier:
    break;
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

/// Qualified scope prefix of a declaration, e.g. "ns::Outer::" for a method.
std::string scopePrefix(CXCursor C) {
  std::vector<std::string> Parts;
  CXCursor Parent = clang_getCursorSemanticParent(C);
  while (!clang_Cursor_isNull(Parent)) {
    CXCursorKind K = clang_getCursorKind(Parent);
    if (K == CXCursor_Namespace || isClassKind(K) || K == CXCursor_EnumDecl) {
      std::string Name = toStd(clang_getCursorSpelling(Parent));
      if (!Name.empty())
        Parts.push_back(Name);
      Parent = clang_getCursorSemanticParent(Parent);
      continue;
    }
    break;
  }
  std::string Prefix;
  for (auto It = Parts.rbegin(); It != Parts.rend(); ++It)
    Prefix += *It + "::";
  return Prefix;
}

std::string locationStr(CXCursor C) {
  CXSourceLocation Loc = clang_getCursorLocation(C);
  CXFile File = nullptr;
  unsigned Line = 0, Column = 0, Offset = 0;
  clang_getExpansionLocation(Loc, &File, &Line, &Column, &Offset);
  if (!File)
    return std::string();
  std::string Name = toStd(clang_getFileName(File));
  return Name + ":" + std::to_string(Line);
}

/// "Base::foo(int) const" — enough to disambiguate overloads.
std::string methodLabel(CXCursor M) {
  std::string S = scopePrefix(M) + toStd(clang_getCursorSpelling(M)) + "(";
  int NumArgs = clang_Cursor_getNumArguments(M);
  for (int I = 0; I < NumArgs; ++I) {
    CXCursor Arg = clang_Cursor_getArgument(M, static_cast<unsigned>(I));
    if (I)
      S += ", ";
    S += toStd(clang_getTypeSpelling(clang_getCursorType(Arg)));
  }
  if (clang_Cursor_isVariadic(M))
    S += NumArgs > 0 ? ", ..." : "...";
  S += ")";
  if (clang_CXXMethod_isConst(M))
    S += " const";
  return S;
}

/// libclang exposes a member function template as a single FunctionTemplate
/// cursor without the templated CXXMethodDecl child, so the signature has to be
/// rebuilt from the function prototype type ("void (T) const").
///
/// Note: clang_CXXMethod_isVirtual()/isPurVirtual() and
/// clang_getOverriddenCursors() accept a CXXMethodDecl only, so the
/// virtual/override relation of a member function *template* is not reported.
std::string templateMethodSignature(CXCursor FT) {
  std::string Name = toStd(clang_getCursorSpelling(FT));
  std::string Ty = toStd(clang_getTypeSpelling(clang_getCursorType(FT)));

  size_t Open = Ty.find('(');
  if (Open == std::string::npos)
    return Name + "()";
  size_t Close = Ty.rfind(')');
  std::string Ret = Ty.substr(0, Open);
  std::string Params = Ty.substr(Open, Close == std::string::npos
                                          ? std::string::npos
                                          : Close - Open + 1);
  std::string Tail =
      Close == std::string::npos ? std::string() : Ty.substr(Close + 1);
  while (!Ret.empty() && Ret.back() == ' ')
    Ret.pop_back();

  // A constructor template has no return type but is spelled like one.
  if (Name == toStd(clang_getCursorSpelling(clang_getCursorSemanticParent(FT))))
    return Name + Params + Tail;
  return (Ret.empty() ? std::string() : Ret + " ") + Name + Params + Tail;
}

std::string methodSignature(CXCursor M, CXCursorKind Kind) {
  std::string S;
  if (clang_CXXMethod_isStatic(M))
    S += "static ";
  if (clang_CXXMethod_isVirtual(M))
    S += "virtual ";

  bool IsCtor = Kind == CXCursor_Constructor;
  bool IsDtor = Kind == CXCursor_Destructor;
  if (!IsCtor && !IsDtor) {
    CXType Ret = clang_getResultType(clang_getCursorType(M));
    if (Ret.kind != CXType_Invalid)
      S += toStd(clang_getTypeSpelling(Ret)) + " ";
  }

  S += toStd(clang_getCursorSpelling(M));
  S += "(";
  int NumArgs = clang_Cursor_getNumArguments(M);
  for (int I = 0; I < NumArgs; ++I) {
    CXCursor Arg = clang_Cursor_getArgument(M, static_cast<unsigned>(I));
    if (I)
      S += ", ";
    S += toStd(clang_getTypeSpelling(clang_getCursorType(Arg)));
    std::string ArgName = toStd(clang_getCursorSpelling(Arg));
    if (!ArgName.empty())
      S += " " + ArgName;
  }
  if (clang_Cursor_isVariadic(M))
    S += NumArgs > 0 ? ", ..." : "...";
  S += ")";
  if (clang_CXXMethod_isConst(M))
    S += " const";

  if (clang_CXXMethod_isPureVirtual(M))
    S += " = 0";
  else if (clang_CXXMethod_isDeleted(M))
    S += " = delete";
  else if (clang_CXXMethod_isDefaulted(M))
    S += " = default";
  return S;
}

//===----------------------------------------------------------------------===//
// Collector
//===----------------------------------------------------------------------===//

class Collector {
public:
  explicit Collector(bool AllHeaders) : AllHeaders(AllHeaders) { Seen.reserve(64); }

  void collect(CXTranslationUnit TU) {
    CXCursor Root = clang_getTranslationUnitCursor(TU);
    clang_visitChildren(Root, &Collector::topLevelVisitor, this);
  }

  std::vector<ClassInfo> Classes;

private:
  struct ClassCtx {
    Collector *Self;
    ClassInfo *Info;
    std::vector<CXCursor> Nested;
  };

  bool inScope(CXCursor C) const {
    CXSourceLocation Loc = clang_getCursorLocation(C);
    if (clang_Location_isInSystemHeader(Loc))
      return false;
    return AllHeaders || clang_Location_isFromMainFile(Loc);
  }

  static bool alreadySeen(Collector &Self, CXCursor C) {
    std::string Key = toStd(clang_getCursorUSR(C));
    if (Key.empty())
      Key = locationStr(C) + ":" + toStd(clang_getCursorSpelling(C));
    if (std::find(Self.Seen.begin(), Self.Seen.end(), Key) != Self.Seen.end())
      return true;
    Self.Seen.push_back(Key);
    return false;
  }

  static CXChildVisitResult topLevelVisitor(CXCursor C, CXCursor,
                                            CXClientData Data) {
    Collector *Self = static_cast<Collector *>(Data);
    CXCursorKind K = clang_getCursorKind(C);
    if (K == CXCursor_Namespace || K == CXCursor_LinkageSpec ||
        K == CXCursor_UnexposedDecl) {
      clang_visitChildren(C, &Collector::topLevelVisitor, Data);
      return CXChildVisit_Continue;
    }
    if (isClassKind(K))
      Self->handleClass(C);
    return CXChildVisit_Continue;
  }

  static CXChildVisitResult memberVisitor(CXCursor C, CXCursor,
                                          CXClientData Data) {
    ClassCtx *Ctx = static_cast<ClassCtx *>(Data);
    Collector *Self = Ctx->Self;
    CXCursorKind K = clang_getCursorKind(C);

    if (K == CXCursor_CXXBaseSpecifier) {
      if (!Self->inScope(C))
        return CXChildVisit_Continue;
      BaseInfo B;
      B.name = toStd(clang_getTypeSpelling(clang_getCursorType(C)));
      if (B.name.empty())
        B.name = toStd(clang_getCursorDisplayName(C));
      B.access = accessStr(clang_getCXXAccessSpecifier(C));
      B.isVirtual = clang_isVirtualBase(C) != 0;
      Ctx->Info->bases.push_back(std::move(B));
      return CXChildVisit_Continue;
    }

    // Nested classes are reported as classes of their own, after the
    // enclosing class.
    if (isClassKind(K)) {
      Ctx->Nested.push_back(C);
      return CXChildVisit_Continue;
    }

    if (!isMethodKind(K))
      return CXChildVisit_Continue;
    if (!Self->inScope(C))
      return CXChildVisit_Continue;

    bool IsTemplate = K == CXCursor_FunctionTemplate;
    CXCursor M = C;
    CXCursorKind MethodKind = K;

    MethodInfo MI;
    MI.access = accessStr(clang_getCXXAccessSpecifier(M));
    MI.location = locationStr(M);
    if (IsTemplate) {
      MI.signature = templateMethodSignature(M);
      MethodKind = CXCursor_FunctionTemplate;
    } else {
      MI.signature = methodSignature(M, MethodKind);
    }

    auto addFlag = [&MI](const char *Flag) {
      if (!MI.flags.empty())
        MI.flags += ", ";
      MI.flags += Flag;
    };
    if (IsTemplate)
      addFlag("template");
    if (MethodKind == CXCursor_Constructor)
      addFlag("ctor");
    else if (MethodKind == CXCursor_Destructor)
      addFlag("dtor");
    else if (MethodKind == CXCursor_ConversionFunction)
      addFlag("conversion");
    else if (IsTemplate &&
             toStd(clang_getCursorSpelling(M)) ==
                 toStd(clang_getCursorSpelling(
                     clang_getCursorSemanticParent(M))))
      addFlag("ctor");  // constructor template
    if (clang_CXXMethod_isPureVirtual(M))
      addFlag("pure virtual");
    if (clang_CXXMethod_isDeleted(M))
      addFlag("deleted");
    else if (clang_CXXMethod_isDefaulted(M))
      addFlag("defaulted");

    // The override relation resolved by Clang through the whole base chain,
    // including multiple and virtual inheritance.
    CXCursor *Overridden = nullptr;
    unsigned NumOverridden = 0;
    clang_getOverriddenCursors(M, &Overridden, &NumOverridden);
    for (unsigned I = 0; I < NumOverridden; ++I)
      MI.overridden.push_back(methodLabel(Overridden[I]));
    clang_disposeOverriddenCursors(Overridden);
    if (!MI.overridden.empty())
      addFlag("override");

    Ctx->Info->methods.push_back(std::move(MI));
    return CXChildVisit_Continue;
  }

  void handleClass(CXCursor C) {
    if (clang_Cursor_isNull(C) || !inScope(C))
      return;
    if (alreadySeen(*this, C))
      return;

    ClassInfo Info;
    Info.name = scopePrefix(C) + toStd(clang_getCursorDisplayName(C));
    if (Info.name.empty())
      Info.name = "(anonymous)";
    Info.location = locationStr(C);

    switch (clang_getCursorKind(C)) {
    case CXCursor_StructDecl:
      Info.kind = "struct";
      break;
    case CXCursor_UnionDecl:
      Info.kind = "union";
      break;
    case CXCursor_ClassTemplate:
      Info.kind = "class template";
      break;
    case CXCursor_ClassTemplatePartialSpecialization:
      Info.kind = "class template (partial specialization)";
      break;
    default:
      Info.kind = "class";
      break;
    }

    ClassCtx Ctx{this, &Info, {}};
    clang_visitChildren(C, &Collector::memberVisitor, &Ctx);
    std::vector<CXCursor> Nested = std::move(Ctx.Nested);

    Classes.push_back(std::move(Info));

    for (CXCursor N : Nested)
      handleClass(N);
  }

  bool AllHeaders;
  std::vector<std::string> Seen;
};

//===----------------------------------------------------------------------===//
// Reporting
//===----------------------------------------------------------------------===//

void printReport(const std::vector<ClassInfo> &Classes) {
  size_t MethodCount = 0, OverrideCount = 0, EdgeCount = 0;

  for (const ClassInfo &C : Classes) {
    std::printf("### %s %s\n", C.kind.c_str(), C.name.c_str());
    std::printf("    file      %s\n", C.location.c_str());

    if (C.bases.empty()) {
      std::printf("    bases     (none)\n");
    } else {
      std::printf("    bases\n");
      for (const BaseInfo &B : C.bases) {
        std::printf("      - %s%s%s\n", B.access.c_str(),
                    B.isVirtual ? " virtual " : " ", B.name.c_str());
        ++EdgeCount;
      }
    }

    if (C.methods.empty()) {
      std::printf("    methods   (none)\n");
      std::printf("\n");
      continue;
    }

    std::printf("    methods (%zu)\n", C.methods.size());
    for (const MethodInfo &M : C.methods) {
      ++MethodCount;
      std::printf("      - [%s] %s", M.access.c_str(), M.signature.c_str());
      if (!M.flags.empty())
        std::printf("  [%s]", M.flags.c_str());
      std::printf("\n");
      std::printf("          at %s\n", M.location.c_str());
      if (!M.overridden.empty()) {
        ++OverrideCount;
        std::printf("          overrides:");
        for (const std::string &O : M.overridden)
          std::printf(" %s;", O.c_str());
        std::printf("\n");
      }
    }
    std::printf("\n");
  }

  std::printf("=== summary ===\n");
  std::printf("classes: %zu  member functions: %zu  overriding functions: %zu  "
              "base edges: %zu\n",
              Classes.size(), MethodCount, OverrideCount, EdgeCount);
  for (const ClassInfo &C : Classes) {
    if (C.bases.empty())
      continue;
    std::printf("  %s", C.name.c_str());
    for (size_t I = 0; I < C.bases.size(); ++I) {
      const BaseInfo &B = C.bases[I];
      std::printf("%s %s%s%s", I == 0 ? " ->" : ",", B.access.c_str(),
                  B.isVirtual ? " virtual " : " ", B.name.c_str());
    }
    std::printf("\n");
  }
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

void printUsage(const char *Argv0) {
  std::fprintf(stderr,
               "usage: %s [--all] <source> [more sources ...] "
               "[-- <extra clang args>]\n"
               "\n"
               "  --all   also report classes defined in headers (system "
               "headers are always skipped)\n"
               "  --      everything after it is passed to clang verbatim\n"
               "\n"
               "Reports classes, member functions, base classes and the "
               "override relation.\n",
               Argv0);
}

bool hasArg(const std::vector<std::string> &Args, const char *Prefix) {
  for (const std::string &A : Args)
    if (A.rfind(Prefix, 0) == 0)
      return true;
  return false;
}

std::string extensionOf(const std::string &Path) {
  size_t Slash = Path.find_last_of('/');
  size_t Dot = Path.find_last_of('.');
  if (Dot == std::string::npos ||
      (Slash != std::string::npos && Dot < Slash))
    return std::string();
  return Path.substr(Dot);
}

bool isCFamilyHeader(const std::string &Ext) {
  return Ext == ".h" || Ext == ".hpp" || Ext == ".hh" || Ext == ".hxx" ||
         Ext == ".inc" || Ext == ".inl" || Ext.empty();
}

/// libclang may be loaded from an LLVM build tree whose compiled-in resource
/// directory (builtin headers such as stddef.h) does not exist. Locate the
/// resource directory next to the libclang shared object actually loaded.
std::string detectResourceDir() {
  void *Sym = dlsym(RTLD_DEFAULT, "clang_createIndex");
  Dl_info Info{};
  if (!Sym || dladdr(Sym, &Info) == 0 || !Info.dli_fname)
    return std::string();
  std::string Lib = Info.dli_fname;
  if (Lib.empty() || Lib[0] != '/')
    return std::string();
  size_t Slash = Lib.find_last_of('/');
  std::string ClangDir = Lib.substr(0, Slash) + "/clang";
  DIR *Dir = opendir(ClangDir.c_str());
  if (!Dir)
    return std::string();

  std::string Best;
  while (struct dirent *Entry = readdir(Dir)) {
    std::string Version = Entry->d_name;
    if (Version.empty() || Version[0] == '.')
      continue;
    std::string Header =
        ClangDir + "/" + Version + "/include/stddef.h";
    if (access(Header.c_str(), R_OK) != 0)
      continue;
    if (Best.empty() ||
        std::atoi(Version.c_str()) > std::atoi(Best.c_str()))
      Best = Version;
  }
  closedir(Dir);
  return Best.empty() ? std::string() : ClangDir + "/" + Best;
}

CXTranslationUnit parse(const std::string &Path, bool IsC,
                        const std::vector<std::string> &ExtraArgs) {
  static const std::string ResourceArg = [] {
    std::string Dir = detectResourceDir();
    return Dir.empty() ? std::string() : "-resource-dir=" + Dir;
  }();

  std::vector<std::string> ArgStore;
  ArgStore.push_back("-x");
  ArgStore.push_back(IsC ? "c" : "c++");
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
  CXErrorCode Err = clang_parseTranslationUnit2(
      Index, Path.c_str(), Args.data(), static_cast<int>(Args.size()),
      /*unsaved_files=*/nullptr, /*num_unsaved_files=*/0,
      CXTranslationUnit_None, &TU);
  clang_disposeIndex(Index);

  if (Err != CXError_Success || !TU) {
    std::fprintf(stderr, "error: cannot parse %s (CXErrorCode %d)\n",
                 Path.c_str(), static_cast<int>(Err));
    return nullptr;
  }

  unsigned NumDiags = clang_getNumDiagnostics(TU);
  for (unsigned I = 0; I < NumDiags; ++I) {
    CXDiagnostic Diag = clang_getDiagnostic(TU, I);
    if (clang_getDiagnosticSeverity(Diag) >= CXDiagnostic_Error) {
      CXString Text = clang_formatDiagnostic(
          Diag, clang_defaultDiagnosticDisplayOptions());
      std::fprintf(stderr, "%s\n", clang_getCString(Text));
      clang_disposeString(Text);
    }
    clang_disposeDiagnostic(Diag);
  }
  return TU;
}

}  // namespace

int main(int argc, char **argv) {
  std::vector<std::string> Sources;
  std::vector<std::string> ExtraArgs;
  bool AllHeaders = false;
  bool AfterDashDash = false;

  for (int I = 1; I < argc; ++I) {
    std::string A = argv[I];
    if (AfterDashDash) {
      ExtraArgs.push_back(A);
    } else if (A == "--") {
      AfterDashDash = true;
    } else if (A == "--all" || A == "-a") {
      AllHeaders = true;
    } else if (A == "-h" || A == "--help") {
      printUsage(argv[0]);
      return 0;
    } else {
      Sources.push_back(A);
    }
  }

  if (Sources.empty()) {
    printUsage(argv[0]);
    return 1;
  }

  Collector Col(AllHeaders);
  int Failures = 0;

  for (const std::string &Src : Sources) {
    std::string Ext = extensionOf(Src);
    bool IsC = Ext == ".c";
    bool IsHeader = isCFamilyHeader(Ext);
    // A .h file is treated as C only when the user says so with -x c.
    bool HeaderAsC = IsHeader && hasArg(ExtraArgs, "-x") &&
                     std::find(ExtraArgs.begin(), ExtraArgs.end(), "c") !=
                         ExtraArgs.end();

    CXTranslationUnit TU = parse(Src, IsC || HeaderAsC, ExtraArgs);
    if (!TU) {
      ++Failures;
      continue;
    }
    Col.collect(TU);
    clang_disposeTranslationUnit(TU);
  }

  printReport(Col.Classes);
  return Failures ? 2 : 0;
}
