# llvm-parser

SVF-based call graph analysis tool that performs Andersen-style pointer analysis on LLVM bitcode and outputs a topological call graph tree.

## Components

### Analyzer

- **`svf-api-demo/src/main.cpp`** — CLI driver: parses flags, loads LLVM bitcode, runs the pointer analysis, and streams per-entry call graphs as tab-separated lines.
- **`svf-api-demo/src/callgraph_analysis.cpp`** / **`svf-api-demo/include/callgraph_analysis.h`** — Analysis core (`analyzeCallGraph`) and `AnalysisConfig`.
- **`svf-api-demo/CMakeLists.txt`** — CMake build (out-of-source into `build/`): builds the analyzer, the native test program, and the test bitcode.
- **`svf-api-demo/build/clang_analyzer`** — Compiled analyzer binary (CMake build output).

### Test Programs

| File | Description |
|------|-------------|
| `svf-api-demo/tests/test_prog.c` | Simple C program with direct function calls (`foo`, `bar`, `zoo`) |
| `svf-api-demo/tests/test_complex.cpp` | Comprehensive C++ test covering all indirect call patterns |

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
- **Multiple inheritance**: `MultiDerived` inherits from both `MixinA` and `MixinB`, each with virtual methods; calls dispatch through each base pointer with this-pointer adjustment
- **Diamond virtual inheritance**: `DiamondTip` inherits from `MidLeft` and `MidRight`, both virtually derived from `DiamondBase`; virtual base pointer resolution
- **Abstract base class dispatch**: `AbstractBase` with pure virtual `doit()`/`report()`, dispatched through array of `ConcreteA`/`ConcreteB` pointers
- **Multi-level virtual chain**: `VBase → VMid → VDerived` three-level override hierarchy dispatched through base pointer array
- **Virtual calls in constructor/destructor**: `TraceDerived` calls virtual `log()` during construction and destruction (dynamic type changes during these contexts)
- **Pure virtual call in base destructor**: `AbstractBase::~AbstractBase()` dispatches via `llvm.trap` (undefined behavior guard)

### Lambdas
- Stateless lambda: `[](){}`
- Capture-by-value lambda with return: `[x](int y) -> int`
- Capture-by-reference lambda calling another function
- **Generic lambda (C++14)**: `[](auto a, auto b){}` — templated call operator instantiated with `int` and `double`
- **Capture by move (C++14)**: `[p = ptr](){}` — lambda captures a dynamically allocated pointer via move
- **Nested lambda**: Lambda `makeAdder()` returns a capturing lambda; closure-in-closure call chain
- **Lambda wrapped in std::function**: Stateless lambda assigned to `std::function<void()>`, invoked through the type-erased wrapper

### Modern C++ Callable Wrappers
- **std::function**: Wraps free functions (`stdFuncTarget1`, `stdFuncTarget2`), a lambda, and supports reassignment between targets
- **std::bind**: Binds arguments to `bindTarget(a,b,c)` using `std::placeholders`, creating callable objects with fewer parameters
- **std::function + bind**: A `std::function<void()>` wrapping a `std::bind` expression
- **std::invoke (C++17)**: Unified call syntax invoking a free function, a member function (`Calculator::add` via pointer-to-member), and a lambda

### Member Pointers & Functors
- **Pointer to member function**: `int (Calculator::*MathOp)(int,int)` called via `.*` and `->*` syntax on instances of `Calculator`, selecting `add`, `sub`, `mul` at runtime
- **Functor (function object)**: `Greeter` class with `operator()(const char*)` — different instances hold different state; calling `hello("World")` vs `goodbye("World")` dispatches to the same operator with different internal data

### Function Pointer Advanced Patterns
- **Ternary/conditional dispatch**: `FuncPtr f = flag ? condTrue : condFalse;` — runtime condition selects between two function pointers
- **State machine with function pointer table**: `StateHandler states[3] = {stateIdle, stateRunning, stateStopped};` dispatched in a loop
- **Function pointer reassignment in loop**: `FuncPtr fp` reassigned each iteration through a loop over `fns[]` array
- **Struct dispatch table**: `DispatchEntry` struct array with `name` string and `handler` function pointer; iterated and invoked
- **Function pointer on struct with operator()**: `CallableOps` struct wrapping a function pointer, called via `operator()()` overload

### Recursion
- **Direct recursion**: `factorial(n)` calls itself with `n-1`
- **Mutual recursion**: `mutualA()` and `mutualB()` call each other alternately with decreasing counter

### Template & Generic Patterns
- **Function templates**: `maxOf<int>(3,7)` and `maxOf<double>(3.14,2.72)` — different template instantiations produce distinct call targets
- **Variadic function template**: `varargSink(1,2,3,4,5)` — recursive template instantiation over parameter pack
- **CRTP (Curiously Recurring Template Pattern)**: `ShapeBase<Derived>::draw()` calls `static_cast<Derived*>(this)->drawImpl()`, resolved at compile time to `Circle::drawImpl()` or `Square::drawImpl()`

### Other C++ Features
- **Default arguments**: `defaultArgsFunc(1)`, `defaultArgsFunc(2,20)`, `defaultArgsFunc(3,30,"explicit")` — calls with different argument counts expand differently at IR level
- **Operator overloading**: Built-in `operator+` called on `int` operands to verify operator call edges

### C++17 Callable & Dispatch Patterns (New)
- **std::mem_fn**: `std::mem_fn(&Calculator::add)` wraps a member function pointer into a callable, invoked via `_Mem_fn::operator()` with `__invoke_memfun_ref` / `__invoke_memfun_deref` paths
- **std::bind with member function**: `std::bind(&Calculator::add, &calc, _1, _2)` — binds a member function pointer with an instance pointer and placeholders, creating a complex nested call chain through `_Bind::__call` → `__invoke_memfun_deref`
- **Delegate pattern**: `Button` class stores `std::function<void()>` / `std::function<void(int)>` as members. Callbacks (`delegateClickA/B`, `delegateKeyHandler`) are dynamically registered with `setOnClick`/`setOnKey` and dispatched through `operator()` — includes runtime reassignment
- **std::map dispatch table**: `std::map<int, FuncPtr>` storing `mapCmdStart`/`mapCmdStop`/`mapCmdStatus`, looked up via `find()` and invoked through `it->second()`
- **if constexpr dispatch (C++17)**: `algoDispatch<true>()` vs `algoDispatch<false>()` — compile-time branch elimination via `if constexpr`, only the selected path survives in IR
- **Fold expression call (C++17)**: `(foldTarget(args), ...)` expands a parameter pack over the comma operator into sequential calls; `(fns(), ...)` calls a pack of function pointers
- **std::apply (C++17)**: `std::apply(applyTarget, tuple)` unpacks a tuple into function arguments; also tested with a lambda
- **Overloaded lambda pattern (C++17)**: `Overloaded{[](int){}, [](const char*){}}` uses variadic `using Ts::operator()...` and CTAD deduction guide to create a multi-overload callable; dispatched through the synthesized `operator()`
- **std::variant + std::visit**: `VariantType = std::variant<int, double, const char*>` visited in a loop with an `Overloaded` visitor, dispatching to `variantIntHandler` / `variantDoubleHandler` / `variantStrHandler` per alternative
- **Recursive lambda via std::function**: Lambda that captures itself by reference (`[&fib]`) in a `std::function<int(int)>` and calls `fib(n-1) + fib(n-2)` recursively to compute Fibonacci, relying on type-erased self-reference

## Build & Usage

### Prerequisites

- SVF framework at `/home/test/src/SVF` (or `-DSVF_ROOT=<path>`)
- LLVM 21 toolchain at `/usr/lib/llvm-21` (or `-DLLVM_PREFIX=<path>`)
- Z3 solver

### Build

CMake, out-of-source into `svf-api-demo/build/`:

```bash
cmake -S svf-api-demo -B svf-api-demo/build
cmake --build svf-api-demo/build
```

This produces:

- `svf-api-demo/build/clang_analyzer` — analyzer binary
- `svf-api-demo/build/test_complex` — native test program executable
- `svf-api-demo/build/tests/test_complex.bc`, `test_prog.bc` — test bitcode
  (generated by the `bitcode` target with the LLVM-21 clang; the system
  clang 18 cannot produce/read bitcode compatible with the LLVM-21 SVF build)

`SVF_ROOT` (default `/home/test/src/SVF`) and `LLVM_PREFIX` (default
`/usr/lib/llvm-21`) can be overridden at configure time:

```bash
cmake -S svf-api-demo -B svf-api-demo/build -DSVF_ROOT=/path/to/SVF
```

### Run Analysis

The analyzer supports multiple analysis modes via CLI flags. The default
is tuned for the most **complete + precise** call graph: flow-sensitive
PTA + context-sensitive SVFG (heap-object model off). Enabling the heap
model resolves indirect calls through **local fn-ptr arrays** (e.g.
`void (*fns[2])() = {regA, regB}`) that the default misses, but it also
measurably under-resolves virtual dispatch through base-class arrays and
struct dispatch tables — a net completeness loss on the test suite — so it
is opt-in.

The output call graph **excludes library code** by default to keep the tree
focused on user code:
- C++ standard library / runtime functions by name: `std::`, `__gnu_cxx::`,
  `__cxa_*` exception ABI, `operator new/delete`;
- any function defined in a **system include header** (libstdc++/glibc
  templates under `/usr/include`, etc.) — judged by its debug-info file path,
  which catches libstdc++ functions whose demangled names carry a return-type
  prefix (`void std::...`, `decltype(...) std::...`) and would slip past a
  name-only filter;
- library/runtime functions with **no debug info** (`printf`, LLVM intrinsics,
  `__dynamic_cast`) — when the module does carry debug info elsewhere.

User callbacks invoked from inside library functions (comparators, lambdas,
functors, `std::function` targets) are still shown, grafted onto the caller
that reached them. Pass `--include-stdlib` to keep everything in the output.

| Flag | Description |
|------|-------------|
| *(default)* | FlowSensitive + context-sensitive SVFG, library code excluded — most complete & precise |
| `--fs` | FlowSensitive (FSSPARSE_WPA) — sparse flow-sensitive analysis [default on] |
| `--vfs` | VersionedFlowSensitive (VFS_WPA) — versioned flow-sensitive |
| `--ander` | Downgrade to flow-insensitive AndersenWaveDiff |
| `--cs` | Context-sensitive SVFG (SVF default; flag is descriptive) [default on] |
| `--heap-model` | Heap object model (`ModelConsts`, `ModelArrays`) — opt-in |
| `--include-stdlib` | Keep C++ std / system-header / no-debug-info library functions in the call graph [default: excluded] |

```bash
# Default: flow-sensitive + context-sensitive (most complete & precise)
LD_LIBRARY_PATH=/home/test/src/SVF/Release-build/lib:/usr/lib/llvm-21/lib \
  ./svf-api-demo/build/clang_analyzer svf-api-demo/build/tests/test_complex.bc

# Flow-insensitive Andersen (faster, less precise)
LD_LIBRARY_PATH=/home/test/src/SVF/Release-build/lib:/usr/lib/llvm-21/lib \
  ./svf-api-demo/build/clang_analyzer --ander svf-api-demo/build/tests/test_complex.bc

# Also resolve indirect calls through local fn-ptr arrays (may lose
# virtual/struct dispatch targets)
LD_LIBRARY_PATH=/home/test/src/SVF/Release-build/lib:/usr/lib/llvm-21/lib \
  ./svf-api-demo/build/clang_analyzer --heap-model svf-api-demo/build/tests/test_complex.bc

# Versioned flow-sensitive (object versioning)
LD_LIBRARY_PATH=/home/test/src/SVF/Release-build/lib:/usr/lib/llvm-21/lib \
  ./svf-api-demo/build/clang_analyzer --vfs svf-api-demo/build/tests/test_complex.bc
```

Convenience target — build and run the analyzer on the test-complex bitcode
with the default config. The bundled runtime libs in `build/lib/` are picked
up via the `$ORIGIN` rpath, so no `LD_LIBRARY_PATH` is needed:

```bash
cmake --build svf-api-demo/build --target analyze
```

### Deploy to Another Machine

`clang_analyzer` links against shared SVF/LLVM/z3 libraries and carries an
`$ORIGIN`-relative rpath, so the runtime libraries travel with the binary.
Build a self-contained deployment folder:

```bash
cmake --build svf-api-demo/build --target deploy
```

This assembles `svf-api-demo/build/deploy/`:

```
deploy/
├── clang_analyzer
└── lib/
    ├── libSvfCore.so.3
    ├── libSvfLLVM.so.3
    ├── libLLVM.so.21.1
    ├── libz3.so.4
    └── extapi.bc        # SVF's external-API model (located beside libSvfCore.so)
```

Copy that folder to any x86-64 Linux machine and run — no installs, no
`LD_LIBRARY_PATH`:

```bash
./deploy/clang_analyzer my_program.bc
```

The target machine only needs the usual base system libraries (glibc,
libstdc++, zlib, zstd, ICU …), which are present on essentially every Linux
distro. Fully static linking is not possible without rebuilding SVF itself
as a static library (it is built shared-only); the `$ORIGIN` bundle is the
pragmatic equivalent.

### Run Test Program

```bash
./svf-api-demo/build/test_complex
```

## Output Format

The report (default `<bitcode-stem>.callgraph.tsv`) is line-oriented so it can be
streamed both when writing and when parsing — nothing is materialized in
memory. `#`-prefixed lines are metadata/markers; every other line is one span,
tab-separated, in pre-order (a span's parent always appears before it):

```
#schema_version 1
#bitcode test_prog.bc
#analysis pta=FlowSensitive context_sensitive=1 heap_model=0 exclude_std=1
#entries main
#entry main
0		root	0	0	main	main	test_prog.c	3	6
1	0	direct	1	0	foo()	_Z3foov	test_prog.c	8	10	4
2	1	indirect	2	0	bar()	_Z3barv	test_prog.c	12	14	9
3	1	direct	2	1	foo()	_Z3foov	test_prog.c	8	10	13
#end main spans=4 reachable_funcs=3
```

Span-line columns: `span_id`, `parent_span_id`, `kind` (`root`/`direct`/`indirect`),
`depth`, `cycle` (`0`/`1`), `function` (demangled, params stripped), `mangled`,
`file`, `start_line`, `end_line`, `call_line`. Empty fields mean "absent" (the
root has no parent; functions without debug info have no line columns). A `cycle`
span is a recursion back-edge whose subtree is not expanded. `#end` carries the
entry's span count and the number of unique reachable functions.

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
