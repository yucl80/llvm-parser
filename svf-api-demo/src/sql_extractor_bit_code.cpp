//===----------------------------------------------------------------------===//
// sql_extractor_bit_code.cpp — bitcode backend of the SQLite SQL-script
// extractor.
//
//   ./sql_bitcode_extractor [options] <module.bc> [more modules ...]
//
// Parses LLVM bitcode/IR with the LLVM IR reader and SVF's pointer analysis,
// recovering the SQL passed to sqlite3_exec / sqlite3_prepare_* /
// sqlite3_get_table / sqlite3_declare_vtab. The SQL argument is resolved
// statically:
//   * a string literal or a global constant behind a GEP into @.str;
//   * a function returning a constant (its return statements are followed
//     interprocedurally; the producing function is recorded as `symbol`);
//   * a branch (PHI / select) over constant SQL — reported as fragments;
//   * an indirect call (virtual dispatch, a function pointer, a dispatch
//     table, std::function) — SVF names its possible callees, and each
//     callee's returned SQL becomes a candidate (multi-candidate calls are
//     reported as `virtual`/`function` alternatives rather than guessed);
//   * anything still built at runtime is reported as `dynamic`.
//
// The CLI and report writers are shared with the source backend via
// sql_extractor_report.cpp; only the extraction strategy differs.
//===----------------------------------------------------------------------===//

#include "sql_extract.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>

// SVF pointer analysis: resolves indirect calls (virtual dispatch, function
// pointers, dispatch tables, std::function) to their possible callees.
#include "WPA/Andersen.h"
#include "Graphs/CallGraph.h"
#include "Graphs/ICFGNode.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVFIR/SVFIR.h"
#include "SVFIR/SVFVariables.h"
#include "MemoryModel/PointsTo.h"
#include "Util/Casting.h"
#include "Util/Options.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sqlinfo;

namespace {

/// A statically recovered SQL text and the function that produced it (empty
/// when the text came straight from the call argument).
struct ResolvedSql {
    std::string text;
    std::string symbol;
};

/// The first NUL-terminated, printable run inside a byte buffer, used for GEPs
/// into merged string pools. Non-printable bytes end the token.
std::string stringAt(const std::string &bytes, uint64_t offset) {
    if (offset >= bytes.size())
        return {};
    std::string out;
    for (uint64_t i = offset; i < bytes.size() && out.size() < 1024; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0)
            break;
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != ' ')
            return {};
        if (c > 0x7e)
            return {};
        out.push_back(static_cast<char>(c));
    }
    return out;
}

/// Bytes of a string constant. A GEP into a constant offset selects one
/// NUL-terminated run from a string pool; other forms resolve to the whole
/// initializer. Returns empty for anything that is not i8-typed data.
std::string stringConstantBytes(const llvm::Constant *C,
                               const llvm::DataLayout &DL) {
    if (!C)
        return {};
    if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(C)) {
        llvm::APInt offset(DL.getIndexSizeInBits(GEP->getPointerAddressSpace()),
                            0);
        if (!GEP->accumulateConstantOffset(DL, offset))
            return {};
        const auto *base =
            llvm::dyn_cast<llvm::Constant>(GEP->getPointerOperand());
        std::string bytes = stringConstantBytes(base, DL);
        return stringAt(bytes, offset.getZExtValue());
    }
    C = C->stripPointerCasts();
    if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(C)) {
        if (!GV->hasInitializer())
            return {};
        C = GV->getInitializer()->stripPointerCasts();
    }
    const auto *data = llvm::dyn_cast<llvm::ConstantDataArray>(C);
    if (!data || !data->getElementType()->isIntegerTy(8))
        return {};
    std::string bytes;
    bytes.reserve(data->getNumElements());
    for (unsigned i = 0; i < data->getNumElements(); ++i)
        bytes.push_back(static_cast<char>(data->getElementAsInteger(i) & 0xff));
    return bytes;
}

bool looksLikeSql(const std::string &raw) {
    const char *kw[] = {"SELECT", "INSERT", "UPDATE", "DELETE", "CREATE",
                        "DROP",   "ALTER",  "PRAGMA", "ATTACH", "DETACH",
                        "BEGIN",  "COMMIT", "ROLLBACK", "REPLACE", "TRUNCATE",
                        "GRANT",  "REVOKE", "WITH", "VACUUM", "ANALYZE",
                        "EXPLAIN"};
    size_t a = raw.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return false;
    size_t b = raw.find_last_not_of(" \t\r\n");
    std::string s = raw.substr(a, b - a + 1);
    size_t end = s.find_first_of(" \t\r\n(;");
    std::string tok = end == std::string::npos ? s : s.substr(0, end);
    if (tok.empty())
        return false;
    for (char &c : tok)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const char *k : kw)
        if (tok == k)
            return true;
    return false;
}

std::string functionSimpleName(llvm::StringRef mangled) {
    std::string dem = llvm::demangle(mangled.str());
    size_t lp = dem.find('(');
    std::string head = lp == std::string::npos ? dem : dem.substr(0, lp);
    size_t col = head.rfind("::");
    return col == std::string::npos ? head : head.substr(col + 2);
}

bool apiMatches(llvm::StringRef calleeName, const ApiSpec &api) {
    if (calleeName == api.name)
        return true; // C function (no mangling) or a mangled name the user gave
    if (functionSimpleName(calleeName) == api.name)
        return true;
    if (!api.name.empty() && api.name.find("::") != std::string::npos) {
        const std::string dem = llvm::demangle(calleeName.str());
        if (dem.find(api.name) != std::string::npos)
            return true;
    }
    return false;
}

/// Indirect-call resolution: maps an indirect `CallBase` (virtual dispatch, a
/// function pointer, a dispatch table entry, std::function) to the set of
/// possible callee functions that SVF's pointer analysis discovered.
using IndirectCallees =
    std::unordered_map<const llvm::CallBase *, std::vector<const llvm::Function *>>;

void resolveSql(const llvm::Value *V, const llvm::Module &M,
                const llvm::DataLayout &DL, unsigned depth,
                std::unordered_set<const llvm::Value *> &seen,
                std::vector<ResolvedSql> &out,
                const IndirectCallees &indirectCallees) {
    if (!V || depth > 10)
        return;
    if (!seen.insert(V).second)
        return; // guards recursive functions and diamond CFGs
    V = V->stripPointerCasts();
    if (const auto *C = llvm::dyn_cast<llvm::Constant>(V)) {
        std::string bytes = stringConstantBytes(C, DL);
        if (!bytes.empty()) {
            std::string text = stringAt(bytes, 0);
            if (looksLikeSql(text))
                out.push_back({std::move(text), std::string()});
        }
        return; // a constant carries no further SQL-bearing instructions
    }
    if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
        // A static "const char* g = "...";" used as the SQL argument loads a
        // global whose initializer still points at the string constant.
        const llvm::Value *ptr = LI->getPointerOperand()->stripPointerCasts();
        if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(ptr)) {
            if (GV->hasInitializer())
                resolveSql(GV->getInitializer(), M, DL, depth + 1, seen, out,
                           indirectCallees);
        }
        return;
    }
    if (const auto *PHI = llvm::dyn_cast<llvm::PHINode>(V)) {
        for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i)
            resolveSql(PHI->getIncomingValue(i), M, DL, depth + 1, seen, out,
                       indirectCallees);
        return;
    }
    if (const auto *Sel = llvm::dyn_cast<llvm::SelectInst>(V)) {
        resolveSql(Sel->getTrueValue(), M, DL, depth + 1, seen, out,
                   indirectCallees);
        resolveSql(Sel->getFalseValue(), M, DL, depth + 1, seen, out,
                   indirectCallees);
        return;
    }
    if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(V)) {
        // A direct call's return is followed interprocedurally; an indirect
        // call (virtual dispatch, a function pointer, a dispatch table,
        // std::function) is resolved through SVF's pointer analysis, which
        // records the possible callees in `indirectCallees`. C++ calls compile
        // to `invoke` under -O0, so match CallBase rather than only CallInst.
        std::vector<const llvm::Function *> callees;
        if (const llvm::Function *F = CB->getCalledFunction())
            callees.push_back(F);
        else {
            const auto It = indirectCallees.find(CB);
            if (It != indirectCallees.end())
                callees = It->second;
        }
        for (const llvm::Function *F : callees)
            for (const llvm::BasicBlock &BB : *F)
                for (const llvm::Instruction &I : BB)
                    if (const auto *RI = llvm::dyn_cast<llvm::ReturnInst>(&I))
                        resolveSql(RI->getReturnValue(), M, DL, depth + 1, seen,
                                   out, indirectCallees);
        return;
    }
    // LoadInst and every other runtime value: not statically known.
}

/// Build the indirect-callee map for \p M using SVF's Andersen analysis.
///
/// Three complementary layers, all from the same pointer analysis:
///  1. SVF's resolved call graph (`getIndCallMap`): virtual dispatch and
///     std::function targets SVF already connected to concrete callees. SVF
///     keys these by a `CallICFGNode`, and the very same node object is
///     reachable from the LLVM call via `LLVMModuleSet::getCallICFGNode`, so
///     the pairing is exact (no fragile enumeration-order assumption).
///  2. The raw PAG points-to set of the call's function pointer
///     (`pta->getPts(getFunPtr(node))`): every address-taken function object
///     in that set is a candidate. This recovers plain function pointers
///     (e.g. a global `const char *(*)()`).
///  3. For a call whose function-pointer points-to set is empty — a plain
///     `-O0` load from a global function-pointer table — the PAG stores the
///     initializer's function addresses in the array object's field objects
///     instead. Walk those field objects' points-to sets to recover the
///     dispatch-table candidates. Applied only as a fallback, so a cleanly
///     resolved virtual call is never polluted with unrelated vtable entries.
/// All candidates are unioned and de-duplicated per callsite, so a virtual
/// call, a function pointer, a dispatch table and a std::function target are
/// each reported as SQL candidates (alternatives) rather than guessed.
IndirectCallees buildIndirectCallees(const llvm::Module &M) {
    IndirectCallees map;
    SVF::Options::PStat.setValue(false);
    SVF::LLVMModuleSet::buildSVFModule(const_cast<llvm::Module &>(M));
    SVF::SVFIRBuilder builder;
    SVF::SVFIR *pag = builder.build();
    auto *pta =
        new SVF::AndersenWaveDiff(pag, SVF::PTATY::AndersenWaveDiff_WPA, false);
    pta->disablePrintStat();
    pta->analyze();
    SVF::CallGraph *cg = pta->getCallGraph();

    SVF::LLVMModuleSet *lms = SVF::LLVMModuleSet::getLLVMModuleSet();
    const auto &indMap = cg->getIndCallMap();

    auto appendCallee = [&](std::vector<const llvm::Function *> &callees,
                            const std::string &name) {
        const llvm::Function *lf = M.getFunction(name);
        if (lf && std::find(callees.begin(), callees.end(), lf) == callees.end())
            callees.push_back(lf);
    };

    // Collect every function object in an SVF points-to set.
    auto collectFromPts = [&](const SVF::PointsTo &pts,
                              std::vector<const llvm::Function *> &callees) {
        for (SVF::NodeID objId : pts) {
            const SVF::ObjVar *objVar = pag->getObjVar(objId);
            if (!objVar)
                continue;
            const SVF::BaseObjVar *obj = pag->getBaseObject(objVar->getId());
            if (obj && obj->isFunction()) {
                const SVF::FunObjVar *fun =
                    SVF::SVFUtil::cast<SVF::FunObjVar>(obj)->getFunction();
                if (fun)
                    appendCallee(callees, fun->getName());
            }
        }
    };

    // Layer 3 helper: for each object an address points to, scan all of the
    // object's field objects and collect function targets from their points-to
    // sets. SVF keeps a global array's constant initializer (function
    // pointers) in these field objects, so this recovers dispatch tables.
    auto collectFromFieldObjects =
        [&](const SVF::PointsTo &objs,
            std::vector<const llvm::Function *> &callees) {
            std::unordered_set<SVF::NodeID> visited;
            for (SVF::NodeID objId : objs) {
                const SVF::ObjVar *objVar = pag->getObjVar(objId);
                const SVF::BaseObjVar *base =
                    objVar ? pag->getBaseObject(objVar->getId()) : nullptr;
                if (!base)
                    continue;
                for (SVF::NodeID fieldId :
                     pag->getAllFieldsObjVars(base)) {
                    if (visited.insert(fieldId).second)
                        collectFromPts(pta->getPts(fieldId), callees);
                }
            }
        };

    for (const llvm::Function &F : M)
        if (!F.isDeclaration() && !F.isIntrinsic())
            for (const llvm::BasicBlock &BB : F)
                for (const llvm::Instruction &I : BB) {
                    const auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
                    // Direct calls are followed interprocedurally via
                    // getCalledFunction(); only indirect calls need SVF.
                    if (!CB || CB->getCalledFunction())
                        continue;
                    // Skip any call SVF did not model as an ICFG node
                    // (e.g. intrinsic debug calls) to avoid assertions.
                    if (!lms->hasICFGNode(CB))
                        continue;
                    const SVF::CallICFGNode *node = lms->getCallICFGNode(CB);

                    std::vector<const llvm::Function *> callees;

                    // Layer 1: SVF's resolved indirect-call edges.
                    const auto it = indMap.find(node);
                    if (it != indMap.end())
                        for (const SVF::FunObjVar *fn : it->second)
                            appendCallee(callees, fn->getName());

                    // Layer 2: the raw points-to set of the function pointer.
                    if (pag->isIndirectCallSites(node))
                        collectFromPts(pta->getPts(pag->getFunPtr(node)),
                                       callees);
                    const llvm::Value *fp =
                        CB->getCalledOperand()->stripPointerCasts();
                    if (lms->hasValueNode(fp))
                        collectFromPts(pta->getPts(lms->getValueNode(fp)),
                                       callees);

                    // Layer 3 (fallback): a function-pointer table loaded from
                    // a global array. At -O0 `dispatchTable[0]()` is a plain
                    // load whose function-pointer points-to set is empty;
                    // SVF keeps the initializer's function addresses in the
                    // array object's field objects. Follow the loaded pointer
                    // back through GEPs/casts and scan those field objects.
                    if (callees.empty()) {
                        if (const auto *LI =
                                llvm::dyn_cast<llvm::LoadInst>(fp)) {
                            const llvm::Value *ptr =
                                LI->getPointerOperand()->stripPointerCasts();
                            for (const llvm::Value *root : {ptr, fp}) {
                                if (!lms->hasValueNode(root))
                                    continue;
                                collectFromFieldObjects(
                                    pta->getPts(lms->getValueNode(root)),
                                    callees);
                                if (!callees.empty())
                                    break;
                            }
                        }
                    }

                    if (!callees.empty())
                        map[CB] = std::move(callees);
                }

    delete pta;
    SVF::SVFIR::releaseSVFIR();
    SVF::LLVMModuleSet::releaseLLVMModuleSet();
    return map;
}

void fillLocation(SqlStatement &St, const llvm::DebugLoc &DL) {
    if (!DL)
        return;
    if (const llvm::DILocation *Loc = DL.get()) {
        St.file = Loc->getFilename().str();
        St.line = Loc->getLine();
        St.column = Loc->getColumn();
    }
}

std::vector<ApiSpec> buildApis(const SqlOptions &O) {
    std::vector<ApiSpec> apis;
    for (const ApiSpec &custom : O.customApis) {
        bool exists = false;
        for (ApiSpec &a : apis)
            if (a.name == custom.name) {
                a = custom;
                exists = true;
                break;
            }
        if (!exists)
            apis.push_back(custom);
    }
    if (O.useBuiltinApis)
        for (const ApiSpec &builtin : builtinSqliteApis()) {
            const bool exists = std::any_of(
                apis.begin(), apis.end(),
                [&](const ApiSpec &a) { return a.name == builtin.name; });
            if (!exists)
                apis.push_back(builtin);
        }
    return apis;
}

} // namespace

namespace sqlinfo {

bool extractSqlFromBitcode(const SqlOptions &O, SqlReport &Report,
                          std::string &Error) {
    Report.apis = buildApis(O);
    std::unordered_set<std::string> consumed; // SQL text already reported

    for (const std::string &input : O.inputs) {
        Report.inputs.push_back(input);
        llvm::SMDiagnostic diagnostic;
        llvm::LLVMContext context;
        std::unique_ptr<llvm::Module> module =
            llvm::parseIRFile(input, diagnostic, context);
        if (!module) {
            ++Report.parseErrors;
            Error = input + ": " + diagnostic.getMessage().str();
            continue;
        }
        const llvm::DataLayout &DL = module->getDataLayout();
        auto indirect = buildIndirectCallees(*module);

        for (const llvm::Function &F : *module) {
            if (F.isDeclaration() || F.isIntrinsic())
                continue;
            for (const llvm::BasicBlock &BB : F)
                for (const llvm::Instruction &I : BB) {
                    const auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
                    if (!CB)
                        continue;
                    const llvm::Function *callee = CB->getCalledFunction();
                    if (!callee)
                        continue; // indirect call: the API name is unknown
                    const ApiSpec *api = nullptr;
                    for (const ApiSpec &a : Report.apis)
                        if (apiMatches(callee->getName(), a)) {
                            api = &a;
                            break;
                        }
                    if (!api)
                        continue;
                    if (api->sqlArg < 0 ||
                        api->sqlArg >= static_cast<int>(CB->arg_size()))
                        continue;
                    const llvm::Value *arg =
                        CB->getArgOperand(static_cast<unsigned>(api->sqlArg));

                    std::vector<ResolvedSql> sqls;
                    std::unordered_set<const llvm::Value *> seen;
                    resolveSql(arg, *module, DL, 0, seen, sqls, indirect);

                    SqlStatement base;
                    fillLocation(base, I.getDebugLoc());
                    base.api = api->name;

                    if (sqls.empty()) {
                        base.resolved = false;
                        base.complete = false;
                        base.origin = "call";
                        ++Report.unresolved;
                        Report.statements.push_back(std::move(base));
                        continue;
                    }

                    // The SQL may come from a single producer function or from
                    // several branches; the latter are alternatives, not one.
                    std::string producer;
                    if (const auto *CB = llvm::dyn_cast<llvm::CallBase>(arg)) {
                        if (const llvm::Function *PF = CB->getCalledFunction())
                            producer = functionSimpleName(PF->getName());
                        else {
                            // Indirect call (virtual dispatch, function pointer,
                            // dispatch table, std::function) resolved by SVF:
                            // name the producer from its (single) candidate.
                            const auto it = indirect.find(CB);
                            if (it != indirect.end() && it->second.size() == 1)
                                producer =
                                    functionSimpleName(it->second[0]->getName());
                        }
                    }
                    const bool multi = sqls.size() > 1;
                    std::unordered_set<std::string> added;
                    for (const ResolvedSql &r : sqls) {
                        if (!added.insert(r.text).second)
                            continue;
                        SqlStatement st = base;
                        st.sql = r.text;
                        st.resolved = true;
                        st.complete = !multi;
                        if (multi)
                            st.origin = "fragment";
                        else if (!producer.empty()) {
                            st.origin = "function";
                            st.symbol = producer;
                        } else {
                            st.origin = "call";
                        }
                        consumed.insert(r.text);
                        Report.statements.push_back(std::move(st));
                    }
                }
        }

        if (O.scanLiterals)
            for (const llvm::GlobalVariable &GV : module->globals()) {
                if (!GV.hasInitializer())
                    continue;
                std::string bytes =
                    stringConstantBytes(GV.getInitializer(), DL);
                std::string text = stringAt(bytes, 0);
                if (text.empty() || !looksLikeSql(text))
                    continue;
                if (!consumed.insert(text).second)
                    continue; // already reported as a call argument
                SqlStatement st;
                st.api.clear();
                st.origin = "literal";
                st.sql = text;
                st.resolved = true;
                st.complete = true;
                Report.statements.push_back(std::move(st));
            }
    }

    if (O.unique) {
        std::vector<SqlStatement> filtered;
        std::unordered_set<std::string> seenText;
        for (SqlStatement &st : Report.statements)
            if (seenText.insert(st.sql).second)
                filtered.push_back(std::move(st));
        Report.statements = std::move(filtered);
    }
    return true;
}

} // namespace sqlinfo

int main(int argc, char **argv) {
    SqlOptions Options;
    std::string Error;
    if (!parseSqlArgs(argc, argv, Options, Error)) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        printSqlUsage(argv[0]);
        return 1;
    }

    SqlReport Report;
    if (!extractSqlFromBitcode(Options, Report, Error)) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        return 1;
    }
    if (!writeSqlReport(Report, Options, Error)) {
        std::fprintf(stderr, "[ERROR] %s\n", Error.c_str());
        return 1;
    }

    size_t Resolved = 0;
    for (const SqlStatement &St : Report.statements)
        if (St.resolved)
            ++Resolved;
    std::fprintf(stderr,
                 "[sql_bitcode_extractor] files=%zu statements=%zu resolved=%zu "
                 "dynamic=%zu\n",
                 Report.inputs.size(), Report.statements.size(), Resolved,
                 Report.unresolved);
    return 0;
}
