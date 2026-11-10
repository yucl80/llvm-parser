//===----------------------------------------------------------------------===//
// entry_extractor_fixture_aux.cpp — second translation unit for the C++ entry
// fixture. Defines the handler-pointer array that the main TU's labelled table
// refers to through an extern declaration.
//
// Linking the two bitcode files with llvm-link produces the pattern where a
// dispatch table's label and its function pointers live in different globals:
// the extractor must follow the referenced global for functions, but must not
// treat the reference itself as a string label.
//===----------------------------------------------------------------------===//

using Handler = void (*)(int);

extern "C" void aux_ping_handler(int) {}
extern "C" void aux_pong_handler(int) {}

extern "C" const Handler kAuxHandlers[] = {aux_ping_handler,
                                           aux_pong_handler};
