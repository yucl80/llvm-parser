//===----------------------------------------------------------------------===//
// class_extractor_source.cpp — source backend of the class-information tool.
//
// Uses libclang's C API to report, for every class/struct/union defined in the
// given source files:
//   - kind, qualified name, source location
//   - direct base classes (access specifier + virtual inheritance)
//   - member functions (access, signature, ctor/dtor/static/virtual/pure
//     virtual/deleted/defaulted/template)
//   - for each member function, the base-class functions it overrides
//
// The CLI (class_extractor_cli.cpp) dispatches C/C++ source inputs here and writes the
// resulting classinfo::ClassReport itself.
//===----------------------------------------------------------------------===//

#include "class_report.h"

#include <clang-c/Index.h>

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace classinfo;

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

/// Split libclang's "file:line" location into the shared report's fields.
void splitLocation(const std::string &Loc, std::string &File, unsigned &Line) {
  const size_t Colon = Loc.rfind(':');
  if (Colon == std::string::npos) {
    File = Loc;
    return;
  }
  File = Loc.substr(0, Colon);
  Line = static_cast<unsigned>(std::atoi(Loc.c_str() + Colon + 1));
}

/// libclang renders static/virtual and "= 0"/"= delete"/"= default" inside the
/// signature. The shared model keeps those in dedicated fields so both
/// backends produce the same signature shape ("void speak(int) const").
std::string stripDecorators(const std::string &Signature) {
  std::string Out = Signature;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const char *Prefix : {"static ", "virtual "}) {
      const size_t Len = std::strlen(Prefix);
      if (Out.compare(0, Len, Prefix) == 0) {
        Out.erase(0, Len);
        Changed = true;
      }
    }
  }
  for (const char *Suffix : {" = 0", " = delete", " = default"}) {
    const size_t Len = std::strlen(Suffix);
    if (Out.size() >= Len &&
        Out.compare(Out.size() - Len, Len, Suffix) == 0) {
      Out.erase(Out.size() - Len);
      break;
    }
  }
  return Out;
}

/// Parameter list of a signature, found by matching parentheses backwards so
/// that "operator()" and function-pointer parameters are handled correctly.
std::string parameterList(const std::string &Signature) {
  const size_t Close = Signature.rfind(')');
  if (Close == std::string::npos)
    return std::string();
  int Depth = 0;
  size_t Open = std::string::npos;
  for (size_t I = Close + 1; I-- > 0;) {
    if (Signature[I] == ')') {
      ++Depth;
    } else if (Signature[I] == '(' && --Depth == 0) {
      Open = I;
      break;
    }
    if (I == 0)
      break;
  }
  if (Open == std::string::npos || Close <= Open + 1)
    return std::string();
  return Signature.substr(Open + 1, Close - Open - 1);
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
/// Note: clang_CXXMethod_isVirtual()/isPureVirtual() and
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
    MI.name = toStd(clang_getCursorSpelling(M));
    MI.access = accessStr(clang_getCXXAccessSpecifier(M));
    splitLocation(locationStr(M), MI.file, MI.line);
    MI.signature = stripDecorators(IsTemplate
                                       ? templateMethodSignature(M)
                                       : methodSignature(M, MethodKind));
    MI.params = parameterList(MI.signature);
    MI.isStatic = clang_CXXMethod_isStatic(M) != 0;
    MI.isVirtual = clang_CXXMethod_isVirtual(M) != 0;
    MI.isPureVirtual = clang_CXXMethod_isPureVirtual(M) != 0;
    MI.isDefinition = clang_isCursorDefinition(M) != 0;

    auto addFlag = [&MI](const char *Flag) { MI.flags.push_back(Flag); };
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
    splitLocation(locationStr(C), Info.file, Info.line);
    Info.hasDefinition = clang_isCursorDefinition(C) != 0;

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
// Translation-unit parsing
//===----------------------------------------------------------------------===//

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

namespace classinfo {

bool extractClassesFromSource(const ClassOptions &options, ClassReport &report,
                              std::string &error) {
  Collector collector(options.allHeaders);
  std::vector<std::string> Failed;

  for (const std::string &Src : options.inputs) {
    std::string Ext = extensionOf(Src);
    bool IsC = Ext == ".c";
    bool IsHeader = isCFamilyHeader(Ext);
    // A .h file is treated as C only when the user says so with -x c.
    bool HeaderAsC = IsHeader && hasArg(options.clangArgs, "-x") &&
                     std::find(options.clangArgs.begin(),
                               options.clangArgs.end(), "c") !=
                         options.clangArgs.end();

    CXTranslationUnit TU = parse(Src, IsC || HeaderAsC, options.clangArgs);
    if (!TU) {
      Failed.push_back(Src);
      continue;
    }
    collector.collect(TU);
    clang_disposeTranslationUnit(TU);
  }

  report.classes = std::move(collector.Classes);
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

}  // namespace classinfo
