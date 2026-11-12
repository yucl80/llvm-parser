//===----------------------------------------------------------------------===//
// entry_extractor_advanced_fixture.cpp — regression fixture for the newer
// entry patterns: a type-erased lambda handler, a handler whose only direct
// caller is dead code, an exported alias and a hidden-visibility API.
//
// Compiled to bitcode by entry_extractor_test.py; never linked natively.
//===----------------------------------------------------------------------===//

#include <functional>

extern "C" void sink(int) {}

// A capturing lambda: its body never appears as a function pointer. The
// extractor recovers it through the std::function invoker chain.
extern "C" __attribute__((noinline)) void register_cb(std::function<void(int)>) {}
extern "C" void install_lambda() {
    const int captured = 7;
    register_cb([captured](int id) { sink(id + captured); });
}

// dead_callers() is never invoked: its direct call must not disqualify
// dead_handler from being an external entry.
extern "C" __attribute__((noinline)) void dead_handler(int id) { sink(id); }
extern "C" void dead_callers() { dead_handler(1); }

extern "C" __attribute__((noinline)) void alias_target(int id) { sink(id); }
extern "C" void alias_public(int) __attribute__((alias("alias_target")));

extern "C" __attribute__((visibility("hidden"), noinline))
void hidden_api(int id) { sink(id); }

extern "C" int main(int argc, char**) {
    install_lambda();
    hidden_api(argc);
    return 0;
}
