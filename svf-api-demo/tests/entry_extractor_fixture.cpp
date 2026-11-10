//===----------------------------------------------------------------------===//
// entry_extractor_fixture.cpp — C++ analysis fixture for entry_extractor.
//
// A small but *working* model of a network service: request handlers are
// attached to the framework in every common way (constant route tables,
// integral opcode tables, runtime registries, middleware chains, completion
// callbacks, static self-registration, metadata attributes and a cross-TU
// table). main() really dispatches through all of them, so compiling and
// running this file natively proves the fixture is not dead code.
//
// The entry_extractor_test.py driver compiles this file to bitcode (with and
// without -g) and checks the declarations placed next to each handler: an
// expectation of the entry's reason and trigger, or an expected absence.
//
// EXPECT_ENTRY entries are checked in `roots` mode (which contains every other
// classification); reason is the one the extractor should report after its
// priority merge. EXPECT_ABSENT checks a specific mode for a false positive.
//
// Native build / run:
//   clang++ -std=c++17 -O0 -pthread entry_extractor_fixture.cpp \
//           entry_extractor_fixture_aux.cpp -o fixture && ./fixture
//===----------------------------------------------------------------------===//

#include <cassert>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <pthread.h>

using Handler = void (*)(int);  // int = request id

static int gCalls = 0;
static void touch(int) { ++gCalls; }

//===----------------------------------------------------------------------===//
// 1. Compile-time dispatch tables.
//===----------------------------------------------------------------------===//

struct Route {
    const char* method;
    const char* path;
    Handler handler;
};
struct OpRoute {
    unsigned opcode;
    Handler handler;
};

// EXPECT_ENTRY name=http_get_user reason=route-handler trigger=/users
void http_get_user(int id) { touch(id); }
// EXPECT_ENTRY name=http_post_order reason=route-handler trigger=/orders
void http_post_order(int id) { touch(id); }

static const Route kRoutes[] = {
    {"GET", "/users/", http_get_user},
    {"POST", "/orders", http_post_order},
};

// An integral key (message id / opcode / enum method) must work as a label too.
// EXPECT_ENTRY name=op_ping reason=route-handler trigger=#1
void op_ping(int id) { touch(id); }
// EXPECT_ENTRY name=op_data reason=route-handler trigger=#2
void op_data(int id) { touch(id); }

static const OpRoute kOps[] = {{1, op_ping}, {2, op_data}};

//===----------------------------------------------------------------------===//
// 2. Runtime registries: the handler pointer appears in an instruction, not in
//    a global initializer.
//===----------------------------------------------------------------------===//

static std::map<std::string, Handler> gStringRouter;
static std::unordered_map<int, Handler> gOpcodeRouter;
static std::vector<Handler> gMiddleware;

// registerApi() is also called from global constructors, before the std::map
// above is constructed. Keep that path on a zero-initialised POD table so the
// fixture does not depend on static initialisation order.
struct StaticRoute {
    const char* path;
    Handler handler;
};
static StaticRoute gStaticRoutes[8];
static unsigned gStaticRouteCount = 0;

// EXPECT_ENTRY name=map_get_user reason=registered-handler
void map_get_user(int id) { touch(id); }
// EXPECT_ENTRY name=map_post_order reason=registered-handler
void map_post_order(int id) { touch(id); }
// EXPECT_ENTRY name=opcode_start reason=registered-handler
void opcode_start(int id) { touch(id); }
// EXPECT_ENTRY name=opcode_stop reason=registered-handler
void opcode_stop(int id) { touch(id); }
// EXPECT_ENTRY name=middleware_auth reason=registered-handler
void middleware_auth(int id) { touch(id); }
// EXPECT_ENTRY name=middleware_log reason=registered-handler
void middleware_log(int id) { touch(id); }

void registerApi(const char* path, Handler handler) {
    if (gStaticRouteCount < 8)
        gStaticRoutes[gStaticRouteCount++] = {path, handler};
}
void addMiddleware(Handler handler) { gMiddleware.push_back(handler); }

// EXPECT_ENTRY name=api_health reason=registered-handler trigger=/health
void api_health(int id) { touch(id); }

// Registered AND called directly: the old extractor lost this one in every
// mode because it had an external direct caller.
// EXPECT_ENTRY name=shared_handler reason=registered-handler trigger=/shared
void shared_handler(int id) { touch(id); }
static void callShared(int id) { shared_handler(id); }

// Type-erased callable: the target is handed to std::function's constructor.
// EXPECT_ENTRY name=std_function_target reason=registered-handler
void std_function_target(int id) { touch(id); }

//===----------------------------------------------------------------------===//
// 3. Framework callbacks (thread pool, signals, atexit).
//===----------------------------------------------------------------------===//

// EXPECT_ENTRY name=thread_worker reason=registered-handler trigger=pthread_create
void* thread_worker(void*) {
    ++gCalls;
    return nullptr;
}
// EXPECT_ENTRY name=signal_handler reason=registered-handler trigger=signal
void signal_handler(int) { ++gCalls; }
// EXPECT_ENTRY name=exit_handler reason=registered-handler trigger=atexit
void exit_handler() { ++gCalls; }

//===----------------------------------------------------------------------===//
// 4. Static self-registration: global constructors and the constructor
//    attribute both end up in @llvm.global_ctors.
//===----------------------------------------------------------------------===//

// EXPECT_ENTRY name=ctor_registered reason=registered-handler trigger=/ctor
void ctor_registered(int id) { touch(id); }

struct Registrar {
    Registrar() { registerApi("/ctor", ctor_registered); }
};
static Registrar gRegistrar;

// EXPECT_ENTRY name=constructor_registered reason=registered-handler trigger=ctor:
void constructor_registered(int id) { touch(id); }

__attribute__((constructor)) static void installHandlers() {
    registerApi("/constructor", constructor_registered);
}

//===----------------------------------------------------------------------===//
// 5. Metadata-marked entries.
//===----------------------------------------------------------------------===//

// EXPECT_ENTRY name=annotated_endpoint reason=annotated trigger=route:/annotated
__attribute__((annotate("route:/annotated")))
void annotated_endpoint(int id) { touch(id); }

// EXPECT_ENTRY name=used_endpoint reason=retained trigger=used
__attribute__((used))
void used_endpoint(int id) { touch(id); }

//===----------------------------------------------------------------------===//
// 6. A pure address-taken callback: no registration call, no store, no direct
//    caller. `volatile` stops clang from folding the global into a direct call,
//    so it stays a callback even in release bitcode without -g.
//===----------------------------------------------------------------------===//

// EXPECT_ENTRY name=loose_cb reason=callback
void loose_cb(int id) { touch(id); }
static Handler const volatile kLoose = loose_cb;

//===----------------------------------------------------------------------===//
// 7. Negative case: a virtual handler interface. Its vtable references RTTI
//    strings, but must never be mistaken for a labelled route table.
//===----------------------------------------------------------------------===//

struct ConnectionHandler {
    virtual ~ConnectionHandler() = default;
    virtual void onRead(int) = 0;
};
struct HttpConnection : ConnectionHandler {
    // EXPECT_ABSENT mode=routes name=HttpConnection::onRead
    void onRead(int id) override { touch(id); }
};

//===----------------------------------------------------------------------===//
// 8. Cross-TU table: kAuxHandlers is defined in
//    entry_extractor_fixture_aux.cpp. After llvm-link the label lives in this
//    TU's initializer while the handler pointers live in another global.
//===----------------------------------------------------------------------===//

extern "C" const Handler kAuxHandlers[];
struct AuxSlot {
    const char* label;
    const Handler* handlers;
};
static const AuxSlot kAuxTable[] = {{"/rpc", kAuxHandlers}};

//===----------------------------------------------------------------------===//
// Driver: exercise every registration path so nothing is dead code.
//===----------------------------------------------------------------------===//

int main() {
    for (const Route& route : kRoutes)
        route.handler(1);
    for (const OpRoute& route : kOps)
        route.handler(2);

    registerApi("/health", api_health);
    registerApi("/shared", shared_handler);
    gStringRouter["/users"] = map_get_user;
    gStringRouter["/orders"] = map_post_order;
    gOpcodeRouter[10] = opcode_start;
    gOpcodeRouter[20] = opcode_stop;
    addMiddleware(middleware_auth);
    addMiddleware(middleware_log);

    std::function<void(int)> wrapped = std_function_target;
    wrapped(3);
    for (const auto& item : gStringRouter)
        item.second(4);
    for (const auto& item : gOpcodeRouter)
        item.second(5);
    for (Handler handler : gMiddleware)
        handler(6);
    for (unsigned i = 0; i < gStaticRouteCount; ++i)
        gStaticRoutes[i].handler(15);

    callShared(7);
    if (kLoose)
        kLoose(8);

    HttpConnection connection;
    ConnectionHandler* base = &connection;
    base->onRead(9);

    if (kAuxTable[0].handlers)
        kAuxTable[0].handlers[0](10);

    pthread_t thread;
    if (pthread_create(&thread, nullptr, thread_worker, nullptr) == 0)
        pthread_join(thread, nullptr);
    std::signal(SIGUSR1, signal_handler);
    std::atexit(exit_handler);

    // Metadata- and ctor-registered handlers are only referenced from tables;
    // touch them so no toolchain drops them.
    annotated_endpoint(11);
    used_endpoint(12);
    ctor_registered(13);
    constructor_registered(14);

    assert(gCalls > 0);
    return 0;
}
