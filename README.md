# llvm-parser

SVF-based call graph analysis tool that performs Andersen-style pointer analysis on LLVM bitcode and outputs a topological call graph tree.

## Components

### Analyzer

- **`svf-api-demo/test.cpp`** / **`svf-api-demo/a.cpp`** — Loads LLVM bitcode, builds SVFIR (Program Assignment Graph), runs AndersenWaveDiff pointer analysis, and prints the resolved call graph as a tree topology with `├──`/`└──` branch connectors.
- **`svf-api-demo/test_svf`** — Compiled analyzer binary.

### Test Programs

| File | Description |
|------|-------------|
| `svf-api-demo/test_prog.c` | Simple C program with direct function calls (`foo`, `bar`, `zoo`) |
| `svf-api-demo/test_complex.cpp` | Comprehensive C++ test covering all indirect call patterns |

## Test Scenarios (test_complex.cpp)

The complex test covers the following call patterns that SVF's pointer analysis must resolve:

### Direct Calls
- `directCall()` → `printf` — trivial direct call

### Function Pointers
- **Basic function pointer**: `FuncPtr fp = calleeViaPtr; fp();`
- **Function pointer array**: `FuncPtr table[3] = {fa, fb, fc}; table[i]();`
- **Callback parameter**: `sortWithCallback(arr, n, cmp)` where `cmp` is a function pointer parameter, called with `compareAsc` / `compareDesc`
- **Pointer to function pointer** (double/triple indirection): `FuncPtr* fpp = &fp; FuncPtr** fppp = &fpp;`
- **Struct member function pointer**: `Handler` struct with `HandlerFunc onEvent` member, including runtime swap between instances
- **Function returning function pointer**: `chooseHandler(kind)` returns `fa` or `fb`; caller invokes the returned pointer
- **Array of pointers to function pointers**: `FuncPtr* fptrs[2] = {&fns[0], &fns[1]}; (*fptrs[i])();`
- **Nested callback**: `middleWare()` receives both a `Comparator` and a `void (*report)(int)` function pointer, demonstrating multi-layer indirect call chains

### Function Overloading
- `overloaded(int)` and `overloaded(double)` — resolved via LLVM mangling

### Virtual Dispatch
- Single virtual inheritance: `Base* b = new Derived(); b->virtualMethod();`
- Polymorphic array: `Animal* animals[3]` holding `Animal`, `Dog`, `Cat` instances, iterated in a loop

### Lambdas
- Stateless lambda: `[](){}`
- Capture-by-value lambda with return: `[x](int y) -> int`
- Capture-by-reference lambda calling another function

## Build & Usage

### Prerequisites

- SVF framework (with LLVM backend)
- LLVM 21 toolchain
- Z3 solver

### Compile Bitcode

```bash
# C test program
/root/src/SVF/llvm-21.1.0.obj/bin/clang -O0 -g -emit-llvm -c test_prog.c -o test_prog.bc

# C++ test program
/root/src/SVF/llvm-21.1.0.obj/bin/clang++ -std=c++17 -O0 -g -emit-llvm -c test_complex.cpp -o test_complex.bc
```

### Build Analyzer

```bash
g++ -std=c++17 -c test.cpp -o test.o \
  -I/root/src/SVF/svf/include \
  -I/root/src/SVF/svf-llvm/include \
  -I/root/src/SVF/Release-build/include \
  -I/root/src/SVF/llvm-21.1.0.obj/include

g++ -std=c++17 -c a.cpp -o a.o \
  -I/root/src/SVF/svf/include \
  -I/root/src/SVF/svf-llvm/include \
  -I/root/src/SVF/Release-build/include \
  -I/root/src/SVF/llvm-21.1.0.obj/include

g++ test.o a.o -o test_svf \
  -L/root/src/SVF/Release-build/lib -lSvfCore -lSvfLLVM \
  -L/root/src/SVF/llvm-21.1.0.obj/lib -lLLVM-21 \
  -lz3 -lpthread -ldl
```

### Run Analysis

```bash
LD_LIBRARY_PATH=/root/src/SVF/Release-build/lib:/root/src/SVF/llvm-21.1.0.obj/lib \
  ./test_svf test_complex.bc
```

### Run Test Program

```bash
./test_complex
```

## Example Output

### Call Graph Topology

```
===== Call Graph Topology =====
main
    ├── directCall()
    │   └── printf
    ├── testFuncPtr()
    │   └── [Indirect]
    │       └── calleeViaPtr()
    │           └── printf
    ├── testOverloaded()
    │   ├── overloaded(int)
    │   │   └── printf
    │   └── overloaded(double)
    │       └── printf
    ├── testVirtual()
    │   ├── operator new(unsigned long)
    │   ├── Derived::Derived()
    │   │   └── Base::Base()
    │   ├── [Indirect]
    │   │   └── Derived::virtualMethod()
    │   │       └── printf
    │   └── [Indirect]
    │       └── Derived::~Derived()
    │           ├── Derived::~Derived()
    │           │   └── Base::~Base()
    │           └── operator delete(void*, unsigned long)
    ├── testLambda()
    │   └── testLambda()::$_0::operator()() const
    │       └── printf
    ├── testCallbackParam()
    │   └── sortWithCallback(int*, int, int (*)(int, int))
    │       ├── [Indirect] → compareAsc(int, int)
    │       └── [Indirect] → compareDesc(int, int)
    ├── testPtrToFuncPtr()
    │   └── [Indirect] → targetViaDoublePtr() → printf
    ├── testStructFuncPtr()
    │   └── [Indirect] → handleEventA(int) → printf
    ├── testReturnFuncPtr()
    │   └── chooseHandler(int)
    │       └── [Indirect] → fa() → printf
    └── testNestedCallback()
        └── middleWare(int, int, int (*)(int, int), void (*)(int))
            ├── [Indirect] → compareAsc(int, int)
            ├── [Indirect] → compareDesc(int, int)
            └── [Indirect] → reportResult(int) → printf
```

Indirect calls (function pointers, virtual dispatch) are marked `[Indirect]`. The tree uses `├──`/`└──` connectors with `│` vertical lines for hierarchy. If a call graph cycle is detected, `[cycle]` is printed to prevent infinite recursion.
