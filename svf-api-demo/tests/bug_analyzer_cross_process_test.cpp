#include "bug_analyzer_plugin.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        llvm::errs() << "Usage: " << argv[0]
                     << " <cross-process-fixture.bc>\n";
        return 2;
    }

    const std::string input = argv[1];
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    const std::unique_ptr<llvm::Module> module =
        llvm::parseIRFile(input, diagnostic, context);
    if (!module) {
        diagnostic.print(argv[0], llvm::errs());
        return 2;
    }

    std::vector<buganalyzer::Finding> findings;
    buganalyzer::analyzeLocalCrossProcess(*module, input, findings);
    buganalyzer::analyzeRemoteCrossProcess(*module, input, findings);

    std::map<std::string, std::vector<const buganalyzer::Finding*>> byFunction;
    for (const buganalyzer::Finding& finding : findings)
        byFunction[finding.function].push_back(&finding);

    unsigned localCount = 0;
    unsigned remoteCount = 0;
    for (const buganalyzer::Finding& finding : findings) {
        if (finding.rule == "LOCAL_CROSS_PROCESS_ACCESS")
            ++localCount;
        else if (finding.rule == "REMOTE_CROSS_PROCESS_ACCESS")
            ++remoteCount;
    }

    llvm::outs() << "Cross-process calls by function:\n";
    for (const auto& entry : byFunction) {
        llvm::outs() << "  " << entry.first << "\n";
        std::set<std::string> printed;
        for (const buganalyzer::Finding* finding : entry.second) {
            const std::string line =
                "    " + finding->rule + " (" + finding->confidence +
                "): " + finding->message;
            if (printed.insert(line).second)
                llvm::outs() << line << "\n";
        }
    }
    llvm::outs() << "\nSummary: " << localCount << " local, " << remoteCount
                 << " remote findings across " << byFunction.size()
                 << " functions.\n";

    auto hasFinding = [&](const std::string& function,
                          const std::string& mechanism) {
        for (const buganalyzer::Finding& finding : findings) {
            if (contains(finding.function, function) &&
                contains(finding.message, mechanism))
                return true;
        }
        return false;
    };
    auto hasAnyFinding = [&](const std::string& function) {
        for (const buganalyzer::Finding& finding : findings) {
            if (contains(finding.function, function))
                return true;
        }
        return false;
    };

    std::vector<std::string> failures;
    auto expect = [&](bool condition, const std::string& description) {
        if (!condition)
            failures.push_back(description);
    };

    expect(hasFinding("open_local_control_socket", "socket()"),
           "missing local AF_UNIX socket finding");
    expect(hasFinding("start_local_supervisor", "socketpair()"),
           "missing socketpair finding");
    expect(hasFinding("start_local_supervisor", "fork()"),
           "missing fork finding");
    expect(hasFinding("launch_state_dumper", "execvp()"),
           "missing execvp finding");
    expect(hasFinding("start_local_supervisor",
                      "pthread_mutexattr_setpshared()"),
           "missing process-shared mutex finding");
    expect(hasFinding("start_local_supervisor", "sem_init()"),
           "missing process-shared semaphore finding");
    expect(hasFinding("map_shared_state", "shm_open()"),
           "missing shm_open finding");
    expect(hasFinding("map_shared_state", "mmap()"),
           "missing MAP_SHARED mmap finding");
    expect(hasFinding("lock_state_file", "fcntl()"),
           "missing cross-process file lock finding");
    expect(hasFinding("inspect_worker_memory", "process_vm_readv()"),
           "missing cross-process memory read finding");

    expect(hasFinding("open_tcp_socket", "socket()"),
           "missing TCP socket finding");
    expect(hasFinding("connect_to_collector", "connect()"),
           "missing TCP connect finding");
    expect(hasFinding("report_telemetry", "recv()"),
           "missing TCP recv finding");
    expect(hasFinding("report_telemetry", "shutdown()"),
           "missing TCP shutdown finding");
    expect(hasFinding("send_telemetry", "send()"),
           "missing telemetry send finding");
    expect(hasFinding("publish_udp_heartbeat", "sendto()"),
           "missing UDP sendto finding");
    expect(hasFinding("publish_udp_heartbeat", "recvfrom()"),
           "missing UDP recvfrom finding");
    expect(hasFinding("dispatch_payload", "send()"),
           "missing function-pointer dispatch finding");

    expect(!hasAnyFinding("disabled_legacy_collector"),
           "constant-false legacy path produced a finding");

    if (!failures.empty()) {
        llvm::errs() << "FAIL: " << failures.size() << " check(s) failed:\n";
        for (const std::string& failure : failures)
            llvm::errs() << "  - " << failure << "\n";
        return 1;
    }

    llvm::outs() << "PASS: all cross-process scenarios were reported.\n";
    return 0;
}
