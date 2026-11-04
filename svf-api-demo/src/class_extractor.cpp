//===----------------------------------------------------------------------===//
// class_extractor — extract every class, its member functions and its
// inheritance relationships from a C/C++ LLVM bitcode module.
//
// The information comes from the module's DWARF debug info (compile with -g):
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
// Usage: class_extractor [options] <bitcode-file>
//===----------------------------------------------------------------------===//

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
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Report data model
// ---------------------------------------------------------------------------

struct BaseInfo {
    std::string name;       ///< display name of the base class (qualified)
    std::string access;     ///< public / protected / private
    bool isVirtual = false; ///< virtual inheritance
    uint64_t offsetBits = 0;
};

struct FieldInfo {
    std::string name;
    std::string type;
    std::string access;
    bool isStatic = false;
    uint64_t offsetBits = 0;
};

struct MethodInfo {
    std::string name;        ///< simple name ("speak", "operator()", "~Dog")
    std::string params;      ///< parameter list without parentheses
    std::string signature;   ///< "void speak(int) const"-style rendering
    std::string linkageName; ///< mangled symbol, empty for pure declarations
    std::string access;
    std::string file;
    unsigned line = 0;
    bool isVirtual = false;
    bool isPureVirtual = false;
    bool isStatic = false;
    bool isDefinition = false;
};

struct ClassInfo {
    std::string qualifiedName;
    std::string kind; ///< class / struct / union
    std::string file; ///< definition file as written in the debug info
    std::string path; ///< normalized absolute definition path (filtering)
    unsigned line = 0;
    bool hasDefinition = false;
    std::vector<BaseInfo> bases;
    std::vector<FieldInfo> fields;
    std::vector<MethodInfo> methods;
};

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

/// "<access> [virtual ]<name>" for an inheritance entry.
std::string baseClause(const BaseInfo& base) {
    std::string out = base.access;
    if (base.isVirtual) out += " virtual";
    out += " " + base.name;
    return out;
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

        collectClasses(work);

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
                          owner->getTag() != llvm::dwarf::DW_TAG_class_type);
        }

        std::sort(classes_.begin(), classes_.end(),
                  [](const ClassInfo& a, const ClassInfo& b) {
                      return a.qualifiedName < b.qualifiedName;
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

    void collectClasses(std::vector<llvm::DIType*>& work) {
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
                        fillClass(classes_[index], composite);
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
        cls.qualifiedName = qname;
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

    void fillClass(ClassInfo& cls, llvm::DICompositeType* type) {
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
                addMethod(cls, sp, defaultPublic);
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
            dst.signature = src.signature;
        }
        if (src.isVirtual) dst.isVirtual = true;
        if (src.isPureVirtual) dst.isPureVirtual = true;
        if (src.isStatic) dst.isStatic = true;
        if (dst.access.empty()) dst.access = src.access;
    }

    static void addMethod(ClassInfo& cls, llvm::DISubprogram* sp,
                          bool defaultPublic) {
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
        const std::string& qualified = cls.qualifiedName;
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

// ---------------------------------------------------------------------------
// Report writers
// ---------------------------------------------------------------------------

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

const char* boolText(bool value) { return value ? "true" : "false"; }

void writeTextReport(llvm::raw_ostream& os, const std::string& bcFile,
                     const std::vector<ClassInfo>& classes,
                     size_t hiddenClasses, bool hasDebugInfo) {
    size_t methodCount = 0;
    size_t baseCount = 0;
    for (const ClassInfo& cls : classes) {
        methodCount += cls.methods.size();
        baseCount += cls.bases.size();
    }

    os << "#bitcode " << bcFile << "\n";
    if (!hasDebugInfo)
        os << "#warning no_debug_info: the module carries no DWARF debug "
              "info; recompile with -g to extract classes\n";
    os << "#classes " << classes.size() << " methods " << methodCount
       << " inheritance_edges " << baseCount << "\n";
    if (hiddenClasses > 0)
        os << "#hidden_library_classes " << hiddenClasses
           << " (use --include-library to keep them)\n";
    os << "\n";

    for (const ClassInfo& cls : classes) {
        os << cls.kind << " " << cls.qualifiedName;
        if (!cls.bases.empty()) {
            os << " : ";
            for (size_t i = 0; i < cls.bases.size(); ++i) {
                if (i > 0) os << ", ";
                os << baseClause(cls.bases[i]);
            }
        }
        if (!cls.hasDefinition) os << " (declaration)";
        os << " {\n";
        os << "  # " << (cls.file.empty() ? "<no file>" : cls.file) << ":"
           << cls.line << "\n";

        for (const FieldInfo& field : cls.fields) {
            os << "  ";
            if (field.isStatic) os << "static ";
            os << field.type << " " << field.name << "  [" << field.access;
            if (!field.isStatic)
                os << ", offset " << field.offsetBits << " bits";
            os << "]\n";
        }
        if (!cls.fields.empty() && !cls.methods.empty()) os << "\n";

        for (const MethodInfo& method : cls.methods) {
            os << "  ";
            if (method.isStatic) os << "static ";
            if (method.isVirtual) os << "virtual ";
            os << method.signature;
            if (method.isPureVirtual) os << " = 0";
            os << "  [" << method.access;
            if (method.isDefinition)
                os << ", definition";
            else
                os << ", declaration";
            if (!method.linkageName.empty())
                os << ", " << method.linkageName;
            os << "]";
            if (!method.file.empty())
                os << "  " << method.file << ":" << method.line;
            os << "\n";
        }
        os << "}\n\n";
    }

    if (!classes.empty()) {
        os << "[inheritance]\n";
        for (const ClassInfo& cls : classes) {
            for (const BaseInfo& base : cls.bases) {
                os << cls.qualifiedName << " -> " << base.name << "  ["
                   << base.access;
                if (base.isVirtual) os << ", virtual";
                os << "]\n";
            }
        }
    }
}

void writeJsonReport(llvm::raw_ostream& os, const std::string& bcFile,
                     const std::vector<ClassInfo>& classes,
                     size_t hiddenClasses, bool hasDebugInfo) {
    os << "{\n";
    os << "  \"bitcode\": \"" << jsonEscape(bcFile) << "\",\n";
    os << "  \"debug_info\": " << boolText(hasDebugInfo) << ",\n";
    os << "  \"hidden_library_classes\": " << hiddenClasses << ",\n";
    os << "  \"classes\": [";
    for (size_t ci = 0; ci < classes.size(); ++ci) {
        const ClassInfo& cls = classes[ci];
        os << (ci == 0 ? "\n" : ",\n");
        os << "    {\n";
        os << "      \"name\": \"" << jsonEscape(cls.qualifiedName) << "\",\n";
        os << "      \"kind\": \"" << cls.kind << "\",\n";
        os << "      \"file\": \"" << jsonEscape(cls.file) << "\",\n";
        os << "      \"line\": " << cls.line << ",\n";
        os << "      \"definition\": " << boolText(cls.hasDefinition) << ",\n";

        os << "      \"bases\": [";
        for (size_t i = 0; i < cls.bases.size(); ++i) {
            const BaseInfo& base = cls.bases[i];
            os << (i == 0 ? "\n" : ",\n");
            os << "        {\"name\": \"" << jsonEscape(base.name)
               << "\", \"access\": \"" << base.access
               << "\", \"virtual\": " << boolText(base.isVirtual)
               << ", \"offset_bits\": " << base.offsetBits << "}";
        }
        if (!cls.bases.empty()) os << "\n      ";
        os << "],\n";

        os << "      \"fields\": [";
        for (size_t i = 0; i < cls.fields.size(); ++i) {
            const FieldInfo& field = cls.fields[i];
            os << (i == 0 ? "\n" : ",\n");
            os << "        {\"name\": \"" << jsonEscape(field.name)
               << "\", \"type\": \"" << jsonEscape(field.type)
               << "\", \"access\": \"" << field.access
               << "\", \"static\": " << boolText(field.isStatic)
               << ", \"offset_bits\": " << field.offsetBits << "}";
        }
        if (!cls.fields.empty()) os << "\n      ";
        os << "],\n";

        os << "      \"methods\": [";
        for (size_t i = 0; i < cls.methods.size(); ++i) {
            const MethodInfo& method = cls.methods[i];
            os << (i == 0 ? "\n" : ",\n");
            os << "        {\"name\": \"" << jsonEscape(method.name)
               << "\", \"signature\": \"" << jsonEscape(method.signature)
               << "\", \"linkage_name\": \"" << jsonEscape(method.linkageName)
               << "\", \"access\": \"" << method.access
               << "\", \"virtual\": " << boolText(method.isVirtual)
               << ", \"pure_virtual\": " << boolText(method.isPureVirtual)
               << ", \"static\": " << boolText(method.isStatic)
               << ", \"definition\": " << boolText(method.isDefinition)
               << ", \"file\": \"" << jsonEscape(method.file)
               << "\", \"line\": " << method.line << "}";
        }
        if (!cls.methods.empty()) os << "\n      ";
        os << "]\n";
        os << "    }";
    }
    if (!classes.empty()) os << "\n  ";
    os << "],\n";

    // Flattened inheritance graph: one edge per (derived, base) pair, so
    // consumers do not have to walk the per-class "bases" arrays.
    os << "  \"inheritance\": [";
    size_t edgeCount = 0;
    for (const ClassInfo& cls : classes) {
        for (const BaseInfo& base : cls.bases) {
            os << (edgeCount == 0 ? "\n" : ",\n");
            ++edgeCount;
            os << "    {\"derived\": \"" << jsonEscape(cls.qualifiedName)
               << "\", \"base\": \"" << jsonEscape(base.name)
               << "\", \"access\": \"" << base.access
               << "\", \"virtual\": " << boolText(base.isVirtual)
               << ", \"offset_bits\": " << base.offsetBits << "}";
        }
    }
    if (edgeCount > 0) os << "\n  ";
    os << "]\n}\n";
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct Config {
    std::string bitcode;
    std::string format = "json";
    std::string output;
    bool includeLibrary = false;
};

void printUsage(const char* prog) {
    llvm::errs()
        << "Usage: " << prog << " [options] <bitcode-file>\n"
        << "Extract every class, its member functions, fields and base classes\n"
        << "from a C/C++ LLVM bitcode file's DWARF debug info.\n\n"
        << "The bitcode must be compiled with -g; without debug info there is no\n"
        << "class information in the IR.\n\n"
        << "Options:\n"
        << "  --format <json|text>  Report layout [default: json]\n"
        << "  --output <file>       Write the report to <file> [default: stdout]\n"
        << "  --include-library     Keep classes defined outside the application's\n"
        << "                        source tree (std::, libstdc++ headers, other\n"
        << "                        linked library code) [default: hidden]\n"
        << "  -h, --help            Show this help\n";
}

bool parseArgs(int argc, char** argv, Config& config) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--format") == 0 && i + 1 < argc) {
            config.format = argv[++i];
            if (config.format != "text" && config.format != "json") {
                llvm::errs() << "[ERROR] unknown --format '" << config.format
                             << "' (expected text or json)\n";
                return false;
            }
        } else if (std::strcmp(arg, "--output") == 0 && i + 1 < argc) {
            config.output = argv[++i];
        } else if (std::strcmp(arg, "--include-library") == 0) {
            config.includeLibrary = true;
        } else if (std::strcmp(arg, "-h") == 0 ||
                   std::strcmp(arg, "--help") == 0) {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg[0] == '-' && arg[1] == '-') {
            llvm::errs() << "[ERROR] unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return false;
        } else if (config.bitcode.empty()) {
            config.bitcode = arg;
        } else {
            llvm::errs() << "[ERROR] more than one bitcode file given\n";
            return false;
        }
    }
    if (config.bitcode.empty()) {
        printUsage(argv[0]);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Config config;
    if (!parseArgs(argc, argv, config))
        return 1;

    llvm::SMDiagnostic error;
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module =
        llvm::parseIRFile(config.bitcode, error, context);
    if (!module) {
        error.print("class_extractor", llvm::errs());
        return 1;
    }

    // Without debug info there is nothing to extract, but that is a property of
    // the input rather than a tool failure: warn clearly, emit an empty report
    // and exit successfully.
    const bool hasDebugInfo = !module->debug_compile_units().empty();
    if (!hasDebugInfo)
        llvm::errs() << "[WARN] " << config.bitcode
                     << " carries no DWARF debug info; class information is\n"
                        "       only present when the sources are compiled "
                        "with -g.\n"
                        "       Nothing to extract -- writing an empty report.\n";

    ClassExtractor extractor(*module);
    extractor.run();
    std::vector<ClassInfo>& all = extractor.classes();

    const std::vector<std::string> appDirs = buildAppSourceDirs(*module);
    std::vector<ClassInfo> classes;
    size_t hiddenClasses = 0;
    classes.reserve(all.size());
    for (ClassInfo& cls : all) {
        const bool isApp =
            config.includeLibrary || appDirs.empty() || cls.path.empty() ||
            inAppTree(appDirs, cls.path);
        if (!isApp) {
            ++hiddenClasses;
            continue;
        }
        classes.push_back(std::move(cls));
    }

    std::string report;
    llvm::raw_string_ostream out(report);
    if (config.format == "json")
        writeJsonReport(out, config.bitcode, classes, hiddenClasses,
                        hasDebugInfo);
    else
        writeTextReport(out, config.bitcode, classes, hiddenClasses,
                        hasDebugInfo);
    out.flush();

    if (config.output.empty()) {
        llvm::outs() << report;
        llvm::outs().flush();
    } else {
        std::ofstream file(config.output, std::ios::binary);
        if (!file) {
            llvm::errs() << "[ERROR] cannot open output file: " << config.output
                         << "\n";
            return 1;
        }
        file << report;
        if (!file) {
            llvm::errs() << "[ERROR] failed to write " << config.output << "\n";
            return 1;
        }
    }

    llvm::errs() << "[class_extractor] classes=" << classes.size();
    size_t methodCount = 0;
    size_t baseCount = 0;
    for (const ClassInfo& cls : classes) {
        methodCount += cls.methods.size();
        baseCount += cls.bases.size();
    }
    llvm::errs() << " methods=" << methodCount << " inheritance_edges="
                 << baseCount;
    if (hiddenClasses > 0)
        llvm::errs() << " hidden_library_classes=" << hiddenClasses;
    if (!hasDebugInfo) llvm::errs() << " no_debug_info=1";
    llvm::errs() << (config.includeLibrary ? " (library classes included)\n"
                                           : "\n");
    return 0;
}
