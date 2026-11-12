//===----------------------------------------------------------------------===//
// class_extractor_bit_code.cpp — bitcode backend of the class-information tool.
//
// Extracts every class, its member functions, fields and inheritance
// relationships from a C/C++ LLVM bitcode module's DWARF debug info (compile
// with -g):
//   * a class is a DICompositeType tagged DW_TAG_class_type,
//     DW_TAG_structure_type or DW_TAG_union_type that has a definition;
//   * a base class is a DIDerivedType tagged DW_TAG_inheritance inside the
//     class type's elements (access + virtual flags travel on it);
//   * a member function is either a DISubprogram stored in the class type's
//     elements (the declaration) or a DISubprogram in the module whose
//     containing type is the class (the out-of-line definition).
//
// Classes defined outside the application's own source tree (std::, the
// libstdc++/glibc headers, other library code linked into the module) are
// hidden by default; --include-library keeps them.
//
// The CLI (class_extractor_cli.cpp) dispatches .bc/.ll inputs here and writes the
// resulting classinfo::ClassReport itself.
//===----------------------------------------------------------------------===//

#include "class_extractor_report.h"

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugProgramInstruction.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace classinfo;

namespace {

// ---------------------------------------------------------------------------
// Path helpers (same normalization rules as the call-graph reporter)
// ---------------------------------------------------------------------------

/// Lexically resolve ".", ".." and repeated separators in an absolute path.
/// Relative paths are returned unchanged.
std::string normalizePath(const std::string& path) {
    if (path.empty() || path[0] != '/')
        return path;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (start == i) break;
        std::string part = path.substr(start, i - start);
        if (part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(std::move(part));
    }
    std::string out;
    for (const std::string& part : parts) {
        out += '/';
        out += part;
    }
    return out.empty() ? "/" : out;
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (name.empty()) return {};
    if (name[0] == '/') return name;
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::string parentDir(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return {};
    return path.substr(0, slash);
}

std::string baseName(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// Normalized absolute path of a DIFile (empty when there is no file).
std::string filePath(const llvm::DIFile* file) {
    if (!file) return {};
    return normalizePath(joinPath(file->getDirectory().str(),
                                  file->getFilename().str()));
}

/// The analyzed application's source tree, derived from the compile units: each
/// unit contributes its own directory and its parent, so project headers kept
/// beside the sources remain application code.
std::vector<std::string> buildAppSourceDirs(const llvm::Module& M) {
    std::vector<std::string> dirs;
    for (const llvm::DICompileUnit* cu : M.debug_compile_units()) {
        const llvm::DIFile* file = cu ? cu->getFile() : nullptr;
        if (!file) continue;
        const std::string dir = parentDir(filePath(file));
        if (dir.empty() || dir == "/") continue;
        dirs.push_back(dir);
        const std::string parent = parentDir(dir);
        if (!parent.empty() && parent != "/")
            dirs.push_back(parent);
    }
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    return dirs;
}

bool inAppTree(const std::vector<std::string>& appDirs,
               const std::string& file) {
    for (const std::string& dir : appDirs) {
        if (file.compare(0, dir.size(), dir) != 0)
            continue;
        if (file.size() == dir.size() || file[dir.size()] == '/')
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Debug-info helpers
// ---------------------------------------------------------------------------

bool isClassTag(unsigned tag) {
    return tag == llvm::dwarf::DW_TAG_class_type ||
           tag == llvm::dwarf::DW_TAG_structure_type ||
           tag == llvm::dwarf::DW_TAG_union_type;
}

const char* kindName(unsigned tag) {
    switch (tag) {
    case llvm::dwarf::DW_TAG_class_type: return "class";
    case llvm::dwarf::DW_TAG_structure_type: return "struct";
    case llvm::dwarf::DW_TAG_union_type: return "union";
    default: return "type";
    }
}

/// Name used for a type node: the debug-info name, or an
/// "<anonymous@file:line>" placeholder for lambdas / unnamed types.
std::string displayName(const llvm::DIType* T) {
    if (!T) return "?";
    const std::string name = T->getName().str();
    if (!name.empty()) return name;
    const std::string file = T->getFilename().str();
    if (file.empty()) return "<anonymous>";
    return "<anonymous@" + baseName(file) + ":" + std::to_string(T->getLine()) +
           ">";
}

/// Fully qualified name built from the class/namespace scope chain. Local
/// classes keep their enclosing function: "main()::Local".
std::string qualifiedName(const llvm::DIType* T) {
    if (!T) return "?";
    std::string prefix;
    for (const llvm::DIScope* scope = T->getScope(); scope;) {
        if (const auto* outer =
                llvm::dyn_cast<llvm::DICompositeType>(scope)) {
            if (!outer->getName().empty())
                prefix = outer->getName().str() + "::" + prefix;
            scope = outer->getScope();
        } else if (const auto* ns = llvm::dyn_cast<llvm::DINamespace>(scope)) {
            if (!ns->getName().empty())
                prefix = ns->getName().str() + "::" + prefix;
            scope = ns->getScope();
        } else if (const auto* fn = llvm::dyn_cast<llvm::DISubprogram>(scope)) {
            prefix = fn->getName().str() + "()::" + prefix;
            scope = fn->getScope();
        } else {
            break;
        }
    }
    return prefix + displayName(T);
}

void subroutineParts(const llvm::DISubroutineType* FT, std::string& ret,
                     std::string& params, int depth);

/// The referenced type of a pointer/reference/typedef/composite node, or null.
const llvm::DIType* baseTypeOf(const llvm::DIType* T) {
    if (const auto* composite = llvm::dyn_cast_or_null<llvm::DICompositeType>(T))
        return composite->getBaseType();
    if (const auto* derived = llvm::dyn_cast_or_null<llvm::DIDerivedType>(T))
        return derived->getBaseType();
    return nullptr;
}

/// Rendering of a type reference, as used for fields, parameters and bases.
std::string typeString(const llvm::DIType* T, int depth = 0) {
    if (!T || depth > 16) return "?";
    switch (T->getTag()) {
    case llvm::dwarf::DW_TAG_base_type: {
        const std::string name = T->getName().str();
        return name.empty() ? "void" : name;
    }
    case llvm::dwarf::DW_TAG_typedef: {
        const std::string name = T->getName().str();
        return name.empty()
                   ? typeString(baseTypeOf(T), depth + 1)
                   : name;
    }
    case llvm::dwarf::DW_TAG_const_type:
        return "const " + typeString(baseTypeOf(T), depth + 1);
    case llvm::dwarf::DW_TAG_volatile_type:
        return "volatile " + typeString(baseTypeOf(T), depth + 1);
    case llvm::dwarf::DW_TAG_restrict_type:
        return "restrict " + typeString(baseTypeOf(T), depth + 1);
    case llvm::dwarf::DW_TAG_pointer_type: {
        if (const auto* FT = llvm::dyn_cast_or_null<llvm::DISubroutineType>(
                baseTypeOf(T))) {
            std::string ret, params;
            subroutineParts(FT, ret, params, depth + 1);
            return ret + " (*)(" + params + ")";
        }
        return typeString(baseTypeOf(T), depth + 1) + "*";
    }
    case llvm::dwarf::DW_TAG_reference_type:
        return typeString(baseTypeOf(T), depth + 1) + "&";
    case llvm::dwarf::DW_TAG_rvalue_reference_type:
        return typeString(baseTypeOf(T), depth + 1) + "&&";
    case llvm::dwarf::DW_TAG_ptr_to_member_type: {
        std::string owner;
        if (const auto* DT = llvm::dyn_cast<llvm::DIDerivedType>(T)) {
            if (auto* extra =
                    llvm::dyn_cast_or_null<llvm::DIType>(DT->getExtraData()))
                owner = typeString(extra, depth + 1);
        }
        if (owner.empty()) owner = "<class>";
        if (const auto* FT = llvm::dyn_cast_or_null<llvm::DISubroutineType>(
                baseTypeOf(T))) {
            std::string ret, params;
            subroutineParts(FT, ret, params, depth + 1);
            return ret + " (" + owner + "::*)(" + params + ")";
        }
        return typeString(baseTypeOf(T), depth + 1) + " " + owner + "::*";
    }
    case llvm::dwarf::DW_TAG_array_type:
        return typeString(baseTypeOf(T), depth + 1) + "[]";
    case llvm::dwarf::DW_TAG_subroutine_type: {
        std::string ret, params;
        subroutineParts(llvm::cast<llvm::DISubroutineType>(T), ret, params,
                        depth + 1);
        return ret + " (" + params + ")";
    }
    case llvm::dwarf::DW_TAG_enumeration_type: {
        const std::string name = T->getName().str();
        return name.empty() ? "enum" : "enum " + name;
    }
    case llvm::dwarf::DW_TAG_class_type:
    case llvm::dwarf::DW_TAG_structure_type:
    case llvm::dwarf::DW_TAG_union_type:
        return qualifiedName(T);
    default: {
        const std::string name = T->getName().str();
        return name.empty() ? "?" : name;
    }
    }
}

void subroutineParts(const llvm::DISubroutineType* FT, std::string& ret,
                     std::string& params, int depth) {
    ret = "void";
    params.clear();
    if (!FT) return;
    const llvm::DITypeRefArray args = FT->getTypeArray();
    if (args.size() > 0 && args[0])
        ret = typeString(args[0], depth + 1);
    bool first = true;
    for (unsigned i = 1; i < args.size(); ++i) {
        const llvm::DIType* param = args[i];
        // The implicit 'this' parameter is marked DW_AT_object_pointer.
        if (!param || param->isObjectPointer()) continue;
        if (!first) params += ", ";
        first = false;
        params += typeString(param, depth + 1);
    }
}

template <typename NodeT>
std::string accessString(const NodeT* node, bool defaultPublic) {
    if (node->isPrivate()) return "private";
    if (node->isProtected()) return "protected";
    if (node->isPublic()) return "public";
    return defaultPublic ? "public" : "private";
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

class ClassExtractor {
public:
    explicit ClassExtractor(const llvm::Module& M) : M_(M) {}

    void run() {
        llvm::DebugInfoFinder finder;
        finder.processModule(M_);

        std::unordered_map<const llvm::DISubprogram*, unsigned> endLines;
        for (const llvm::Function& function : M_) {
            const llvm::DISubprogram* subprogram = function.getSubprogram();
            if (!subprogram) continue;
            for (const llvm::BasicBlock& block : function) {
                for (const llvm::Instruction& instruction : block) {
                    const llvm::DebugLoc location = instruction.getDebugLoc();
                    if (!location ||
                        !location->getScope() ||
                        location->getScope()->getSubprogram() != subprogram)
                        continue;
                    const unsigned line = location.getLine();
                    if (line)
                        endLines[subprogram] =
                            std::max(endLines[subprogram], line);
                }
            }
        }

        std::vector<llvm::DIType*> work;
        const auto push = [&work](llvm::DIType* type) {
            if (type) work.push_back(type);
        };

        for (llvm::DIType* type : finder.types())
            push(type);
        for (llvm::DIGlobalVariableExpression* gv : finder.global_variables())
            if (gv && gv->getVariable())
                push(gv->getVariable()->getType());
        for (llvm::DISubprogram* sp : finder.subprograms()) {
            push(sp->getType());
            push(sp->getContainingType());
            for (llvm::DINode* node : sp->getRetainedNodes()) {
                if (auto* local = llvm::dyn_cast<llvm::DILocalVariable>(node))
                    push(local->getType());
                else if (auto* global =
                             llvm::dyn_cast<llvm::DIGlobalVariable>(node))
                    push(global->getType());
            }
        }
        // Local variables now live in debug records, not retained nodes.
        for (const llvm::Function& F : M_) {
            for (const llvm::BasicBlock& BB : F) {
                for (const llvm::Instruction& inst : BB) {
                    for (const llvm::DbgRecord& record :
                         inst.getDbgRecordRange()) {
                        if (const auto* dvr = llvm::dyn_cast<llvm::DbgVariableRecord>(
                                &record))
                            if (dvr->getVariable())
                                push(dvr->getVariable()->getType());
                    }
                }
            }
        }

        collectClasses(work, endLines);

        // Attach out-of-line definitions: a DISubprogram whose containing type
        // is a class is a member function of that class.
        for (llvm::DISubprogram* sp : finder.subprograms()) {
            auto* owner = llvm::dyn_cast_or_null<llvm::DICompositeType>(
                sp->getContainingType());
            if (!owner)
                owner = llvm::dyn_cast_or_null<llvm::DICompositeType>(
                    sp->getScope());
            if (!owner || !isClassTag(owner->getTag()))
                continue;
            const size_t index = classIndex(owner);
            if (index == kNoIndex) continue;
            addMethod(classes_[index], sp,
                      owner->getName().empty() ||
                          owner->getTag() != llvm::dwarf::DW_TAG_class_type,
                      endLines);
        }

        std::sort(classes_.begin(), classes_.end(),
                  [](const ClassInfo& a, const ClassInfo& b) {
                      return a.name < b.name;
                  });
        for (ClassInfo& cls : classes_) {
            std::sort(cls.methods.begin(), cls.methods.end(),
                      [](const MethodInfo& a, const MethodInfo& b) {
                          if (a.line != b.line) {
                              if (a.line == 0) return false;
                              if (b.line == 0) return true;
                              return a.line < b.line;
                          }
                          return a.signature < b.signature;
                      });
        }
    }

    std::vector<ClassInfo>& classes() { return classes_; }

private:
    static constexpr size_t kNoIndex = static_cast<size_t>(-1);

    void collectClasses(
        std::vector<llvm::DIType*>& work,
        const std::unordered_map<const llvm::DISubprogram*, unsigned>&
            endLines) {
        std::unordered_set<const llvm::DIType*> seen;
        while (!work.empty()) {
            llvm::DIType* type = work.back();
            work.pop_back();
            if (!type || !seen.insert(type).second)
                continue;

            if (auto* composite = llvm::dyn_cast<llvm::DICompositeType>(type)) {
                if (isClassTag(composite->getTag()) &&
                    !composite->isForwardDecl()) {
                    const size_t index = classIndex(composite);
                    if (index != kNoIndex)
                        fillClass(classes_[index], composite, endLines);
                }
                if (composite->getBaseType())
                    work.push_back(composite->getBaseType());
                for (llvm::DINode* element : composite->getElements()) {
                    if (auto* derived = llvm::dyn_cast<llvm::DIDerivedType>(
                            element)) {
                        if (derived->getBaseType())
                            work.push_back(derived->getBaseType());
                    } else if (auto* inner = llvm::dyn_cast<llvm::DICompositeType>(
                                   element)) {
                        work.push_back(inner);
                    }
                }
            } else if (auto* derived =
                           llvm::dyn_cast<llvm::DIDerivedType>(type)) {
                if (derived->getBaseType())
                    work.push_back(derived->getBaseType());
                if (derived->getTag() == llvm::dwarf::DW_TAG_ptr_to_member_type)
                    if (auto* extra = llvm::dyn_cast_or_null<llvm::DIType>(
                            derived->getExtraData()))
                        work.push_back(extra);
            }
        }
    }

    /// Index of the class entry for \p type, creating it when needed. A
    /// forward declaration is merged into an already collected definition of
    /// the same qualified name.
    size_t classIndex(llvm::DICompositeType* type) {
        if (!type) return kNoIndex;
        const auto it = byType_.find(type);
        if (it != byType_.end()) return it->second;

        const std::string qname = qualifiedName(type);
        auto nameIt = byName_.find(qname);
        if (nameIt != byName_.end()) {
            byType_[type] = nameIt->second;
            // Prefer the definition's file/line over a declaration's.
            ClassInfo& cls = classes_[nameIt->second];
            if (!cls.hasDefinition && !type->isForwardDecl()) {
                cls.hasDefinition = true;
                cls.file = type->getFilename().str();
                cls.path = filePath(type->getFile());
                cls.line = type->getLine();
            }
            return nameIt->second;
        }

        ClassInfo cls;
        cls.name = qname;
        cls.kind = kindName(type->getTag());
        cls.file = type->getFilename().str();
        cls.path = filePath(type->getFile());
        cls.line = type->getLine();
        cls.hasDefinition = !type->isForwardDecl();
        classes_.push_back(std::move(cls));
        const size_t index = classes_.size() - 1;
        byType_[type] = index;
        byName_[qname] = index;
        return index;
    }

    void fillClass(
        ClassInfo& cls, llvm::DICompositeType* type,
        const std::unordered_map<const llvm::DISubprogram*, unsigned>&
            endLines) {
        // Lambdas / anonymous types have no name and their synthesized members
        // are public; for named types the class default is private.
        const bool defaultPublic =
            type->getName().empty() ||
            type->getTag() != llvm::dwarf::DW_TAG_class_type;
        for (llvm::DINode* element : type->getElements()) {
            if (auto* derived = llvm::dyn_cast<llvm::DIDerivedType>(element)) {
                if (derived->getTag() == llvm::dwarf::DW_TAG_inheritance) {
                    BaseInfo base;
                    base.name = typeString(derived->getBaseType());
                    base.access = accessString(derived, defaultPublic);
                    base.isVirtual = derived->isVirtual();
                    base.offsetBits = derived->getOffsetInBits();
                    cls.bases.push_back(std::move(base));
                } else if (derived->getTag() == llvm::dwarf::DW_TAG_member) {
                    // Compiler-generated members (the vtable pointer, ...) are
                    // not part of the source-level class definition.
                    if (derived->isArtificial()) continue;
                    FieldInfo field;
                    field.name = derived->getName().str();
                    if (field.name.empty()) field.name = "<anonymous>";
                    field.type = typeString(derived->getBaseType());
                    field.access = accessString(derived, defaultPublic);
                    field.isStatic = derived->isStaticMember();
                    field.offsetBits = derived->getOffsetInBits();
                    cls.fields.push_back(std::move(field));
                }
            } else if (auto* sp = llvm::dyn_cast<llvm::DISubprogram>(element)) {
                addMethod(cls, sp, defaultPublic, endLines);
            }
        }
    }

    static bool sameMethod(const MethodInfo& a, const MethodInfo& b) {
        if (a.name != b.name || a.params != b.params) return false;
        // The D0/D1/D2 (constructor C1/C2) variants of a destructor share one
        // source-level function; their mangled names differ.
        const bool destructor = !a.name.empty() && a.name[0] == '~';
        if (!a.linkageName.empty() && !b.linkageName.empty() && !destructor)
            return a.linkageName == b.linkageName;
        return true;
    }

    static void mergeMethod(MethodInfo& dst, const MethodInfo& src) {
        if (dst.linkageName.empty()) dst.linkageName = src.linkageName;
        if (!dst.isDefinition && src.isDefinition) {
            dst.isDefinition = true;
            dst.file = src.file;
            dst.line = src.line;
            dst.startLine = src.startLine;
            dst.endLine = src.endLine;
            dst.signature = src.signature;
        }
        if (src.isVirtual) dst.isVirtual = true;
        if (src.isPureVirtual) dst.isPureVirtual = true;
        if (src.isStatic) dst.isStatic = true;
        if (dst.access.empty()) dst.access = src.access;
    }

    static void addMethod(ClassInfo& cls, llvm::DISubprogram* sp,
                          bool defaultPublic,
                          const std::unordered_map<const llvm::DISubprogram*,
                                                   unsigned>& endLines) {
        if (!sp || sp->getName().empty()) return;

        MethodInfo method;
        method.name = sp->getName().str();
        method.linkageName = sp->getLinkageName().str();
        // Implicitly declared special members carry no access attribute; C++
        // declares them public.
        if ((sp->getFlags() & llvm::DINode::FlagArtificial) != 0 &&
            !sp->isPrivate() && !sp->isProtected() && !sp->isPublic())
            defaultPublic = true;
        method.access = accessString(sp, defaultPublic);
        method.file = sp->getFilename().str();
        method.line = sp->getLine();
        method.startLine = sp->getLine();
        const auto endLine = endLines.find(sp);
        method.endLine = endLine == endLines.end() ? method.startLine
                                                   : endLine->second;
        method.isDefinition = sp->isDefinition();
        method.isStatic =
            (sp->getFlags() & llvm::DINode::FlagStaticMember) != 0;

        const unsigned virtuality = sp->getVirtuality();
        method.isVirtual = virtuality != 0;
        method.isPureVirtual =
            (virtuality &
             static_cast<unsigned>(llvm::DISubprogram::SPFlagPureVirtual)) != 0;

        std::string ret = "void";
        method.params.clear();
        if (llvm::DISubroutineType* ft = sp->getType())
            subroutineParts(ft, ret, method.params, 0);
        // Constructors and destructors have no return type in C++.
        const std::string& qualified = cls.name;
        const size_t scopeEnd = qualified.rfind("::");
        std::string simple =
            scopeEnd == std::string::npos ? qualified
                                          : qualified.substr(scopeEnd + 2);
        const size_t templateStart = simple.find('<');
        if (templateStart != std::string::npos)
            simple.erase(templateStart);
        const bool destructor = !method.name.empty() && method.name[0] == '~';
        // A destructor takes no source-level parameters; the complete-object
        // variant carries an ABI-only flag parameter in its debug signature.
        if (destructor) method.params.clear();
        const bool specialMember = destructor || method.name == simple;
        method.signature =
            (specialMember ? std::string() : ret + " ") + method.name + "(" +
            method.params + ")";

        for (MethodInfo& existing : cls.methods) {
            if (sameMethod(existing, method)) {
                mergeMethod(existing, method);
                return;
            }
        }
        cls.methods.push_back(std::move(method));
    }

    const llvm::Module& M_;
    std::vector<ClassInfo> classes_;
    std::unordered_map<const llvm::DICompositeType*, size_t> byType_;
    std::unordered_map<std::string, size_t> byName_;
};

} // namespace

namespace classinfo {

bool extractClassesFromBitcode(const ClassOptions& options, ClassReport& report,
                               std::string& error) {
    const std::string& input = options.inputs.front();
    llvm::SMDiagnostic diagnostic;
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module =
        llvm::parseIRFile(input, diagnostic, context);
    if (!module) {
        error = input + ": " + diagnostic.getMessage().str();
        return false;
    }

    // Without debug info there is nothing to extract, but that is a property of
    // the input rather than a tool failure: report it and emit an empty report.
    report.hasDebugInfo = !module->debug_compile_units().empty();

    ClassExtractor extractor(*module);
    extractor.run();
    std::vector<ClassInfo>& all = extractor.classes();

    const std::vector<std::string> appDirs = buildAppSourceDirs(*module);
    report.classes.reserve(all.size());
    for (ClassInfo& cls : all) {
        const bool isApp =
            options.includeLibrary || appDirs.empty() || cls.path.empty() ||
            inAppTree(appDirs, cls.path);
        if (!isApp) {
            ++report.hiddenClasses;
            continue;
        }
        report.classes.push_back(std::move(cls));
    }
    return true;
}

} // namespace classinfo
