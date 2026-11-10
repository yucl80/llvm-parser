# llvm-parser

SVF-based call graph analysis tool that performs Andersen-style pointer analysis on LLVM bitcode and outputs a topological call graph tree.

## Components

### Analyzer

- **`svf-api-demo/src/main.cpp`** — CLI driver: parses flags, loads LLVM bitcode, runs the pointer analysis, and renders per-entry call graphs (ASCII tree by default, or tab-separated spans).
- **`svf-api-demo/src/callgraph_analysis.cpp`** / **`svf-api-demo/include/callgraph_analysis.h`** — Analysis core (`analyzeCallGraph`) and `AnalysisConfig`.
- **`svf-api-demo/src/callgraph_output.cpp`** / **`svf-api-demo/include/callgraph_output.h`** — Report writer: call-graph traversal into spans, metadata header, per-entry sink dispatch.
- **`svf-api-demo/src/callgraph_tsv_sink.cpp`** / **`svf-api-demo/include/callgraph_tsv_sink.h`** — `TsvSink` (machine-readable tab-separated spans) plus the shared span-line/escaping writers.
- **`svf-api-demo/src/callgraph_tree_sink.cpp`** / **`svf-api-demo/include/callgraph_tree_sink.h`** — `TreeSink` (indented ASCII call tree per entry).
- **`svf-api-demo/include/callgraph_sink.h`** / **`svf-api-demo/include/callgraph_span.h`** — `SpanSink` traversal-output interface and the shared span data types.
- **`svf-api-demo/src/function_filter.cpp`** / **`svf-api-demo/include/function_filter.h`** — Library-code classification, user filter patterns, and debug-line ranges.
- **`svf-api-demo/CMakeLists.txt`** — CMake build (out-of-source into `build/`): builds the analyzer, the native test program, and the test bitcode.
- **`svf-api-demo/build/clang_analyzer`** — Compiled analyzer binary (CMake build output).

### Class Information

One CLI (`build/class_extractor`) extracts classes, member functions, fields,
bases and inheritance. Its input file type selects the backend:

| Input | Backend |
|-------|---------|
| `.bc`, `.ll` (LLVM bitcode / textual IR, compiled with `-g`) | `src/class_extractor_bit_code.cpp` — DWARF debug info via the LLVM API |
| `.c`, `.cpp`, `.h`, ... (C/C++ sources) | `src/class_extractor_source.cpp` — libclang C API |

`src/class_extractor_cli.cpp` is the single entry point (type detection + dispatch),
`include/class_report.h` / `src/class_extractor_report.cpp` hold the shared report model
and the JSON/text writers. Extension-less inputs are classified by content;
`--input-type bitcode|source` forces a backend.

```bash
./svf-api-demo/build/class_extractor --format json svf-api-demo/build/tests/test_complex.bc
./svf-api-demo/build/class_extractor --format text svf-api-demo/tests/test_complex.cpp
./svf-api-demo/build/class_extractor --all svf-api-demo/tests/test_complex.cpp -- -std=c++17
```

`--include-library` keeps non-application classes in bitcode reports;
`--all` includes classes from (non-system) headers in source reports.

### SQLite SQL Scripts

`svf-api-demo/build/sql_extractor` uses libclang to recover SQLite SQL
statements from C/C++ sources. It matches `sqlite3_exec`, `sqlite3_prepare*`,
`sqlite3_get_table` and `sqlite3_declare_vtab` calls and statically evaluates
the SQL argument: escaped/raw/adjacent literals, macro expansions, `const
char*` / `std::string` constants (`sql.c_str()`), parentheses, casts and
concatenations.

Polymorphic SQL builders are resolved as well: functions returning a constant,
virtual dispatch (one report per override, including chained overrides),
function pointers and dispatch tables, lambdas / `std::function` callables,
format templates (`%d` / `{}` placeholders) and static fragments of
conditional or concatenated expressions. Each statement carries an `origin`
(`call`, `function`, `virtual`, `callable`, `format`, `fragment`, `literal`)
and a `symbol` naming the producer; alternative variants and fragments are
marked incomplete. `--no-dispatch` switches back to strict extraction, where
anything not statically resolvable is reported as `dynamic` instead of being
guessed.

| Flag | Description |
|------|-------------|
| `--format <json\|text\|sql>` | Structured report (default), one line per statement, or a runnable `.sql` script |
| `--output <file>` | Write to a file instead of stdout |
| `--api <name[:argno]>` | Extra SQL-carrying callee with a 0-based argument index, e.g. `--api QSqlQuery::prepare:0` (repeatable) |
| `--no-builtin` | Disable the built-in `sqlite3_*` list |
| `--scan-literals` | Also collect SQL-looking literals not consumed by a resolved call |
| `--include-headers` | Scan non-system headers too |
| `--unique` | Drop statements with duplicate SQL text |
| `--no-dispatch` | Disable producer/fragment inference (strict static mode) |
| `-- <clang args>` | Pass `-I`, `-D`, `-std=c++20`, ... to clang |

```bash
./svf-api-demo/build/sql_extractor --format sql --output schema.sql \
    svf-api-demo/tests/test_sqlite_source.cpp
./svf-api-demo/build/sql_extractor --scan-literals --format json \
    src/db.cpp src/schema.cpp -- -Iinclude -DUSE_SQLITE
```

`svf-api-demo/tests/test_sqlite_source.cpp` is the fixture covering literals,
macros, constants, `std::string` wrappers, raw strings, concatenation, virtual
overrides, function-pointer tables, lambdas / `std::function`, format templates,
custom APIs and dynamic SQL. Its `runDynamicSql()` function adds six run-time
SQL-assembly shapes — `operator+` with a parameter, `+=` with `std::to_string`,
an `append()` chain, `std::ostringstream`, `snprintf` into a stack buffer and a
loop-accumulated predicate. Both back ends must report these as `dynamic` with
no fabricated SQL, as they are the classic SQL-injection patterns.
`svf-api-demo/tests/sql_extractor_test.py` is its CTest regression suite
(`sql_extractor_regression`).

### SQLite SQL Scripts from Bitcode

`svf-api-demo/build/sql_bitcode_extractor` extracts the same SQLite SQL reports
directly from LLVM bitcode / IR (`.bc` / `.ll`) with the LLVM IR reader. It
follows the SQL argument of `sqlite3_*` calls through:

* string literals and global constants (including GEPs into merged string
  pools / string tables);
* functions returning a constant — their `return` statements are traced
  interprocedurally and recorded as `symbol`;
* branch fragments — a `select` / `phi` over constant SQL is reported as
  alternatives rather than one fabricated statement;
* indirect calls — a SVF/Andersen pointer-analysis layer resolves virtual
  dispatch, function pointers, global function-pointer tables and
  `std::function` / lambda targets to concrete callees; each callee's returned
  SQL becomes a candidate (several targets are reported as alternatives, not
  guessed). The call graph (`getIndCallMap`, paired to LLVM calls via
  `getCallICFGNode`), the raw PAG points-to set of the call's function pointer,
  and — for `-O0` array loads — the field-object points-to sets that hold a
  dispatch table's initializer are combined;
* genuinely runtime values (a local load, a `std::string` conversion, a format
  template, a runtime parameter and SQL assembled at run time via `operator+`,
  `+=`, `append()`, streams, `snprintf` or loops) are reported as `dynamic`
  instead of guessed.

The flags, `origin` / `symbol` fields and JSON / text / SQL layouts are identical
to the source extractor, so both back ends feed the same report consumers.

```bash
./svf-api-demo/build/sql_bitcode_extractor --format sql --output schema.sql \
    build/tests/test_sqlite_source.bc
```

The `svf-api-demo/tests/test_sqlite_source.cpp` fixture above is compiled to
`test_sqlite_source.bc` with `-g` and shared by the source and bitcode
extractors. `svf-api-demo/tests/sql_bitcode_extractor_test.py` is the CTest
regression suite (`sql_bitcode_extractor_regression`).

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

The output call graph **excludes library code** by default, so only the
analyzed application's own call chains are reported. The decision is made from
**where each function is defined**, not from a list of library function names:

- the application's source tree is derived from the module's **compile units**
  (each unit's directory and its parent, so project headers next to the sources
  stay application code);
- a function whose definition file lies **outside that tree** is library code —
  this is what filters the C library (`strrchr`, `memcpy`, ...), libstdc++
  headers (`std::`, `__gthread_mutex_lock`, ...) and any other library source
  linked into the module, regardless of where the toolchain keeps its headers;
- a function with **no definition in the module** (a body-less declaration) is
  imported library code. This check runs on the *original* module, before SVF's
  ExtAPI materialises stub bodies for modelled C functions;
- symbols whose namespace identifies them as std/runtime — `std::`,
  `__gnu_cxx::`, `__cxa_*`, `__gxx_*`, `_Unwind_*`, sanitizer hooks,
  `operator new/delete` — are filtered by symbol, so they are also hidden on
  release builds **without `-g`**;
- library/runtime functions with **no debug info** when the module carries
  debug info elsewhere, and calls made from inside a system header.

On a build **without `-g`** there is no definition file to attribute by, so
body-bearing header inlines (e.g. `__gthread_mutex_lock`) cannot be
classified; declarations and std symbols are still filtered.

Three rules shape the reported call tree. They apply identically to library
code and to functions hidden by a filter file:

1. an edge from a user function into a **hidden** function (library code or a
   filter match) is **not** reported;
2. an edge from a hidden function into a **user** function **is** reported: the
   user function is grafted onto the nearest visible ancestor (a callback
   invoked from inside library machinery keeps its own subtree);
3. the hidden function's **own call tree is not traversed**, so calls between
   two hidden functions never appear and callbacks reachable only through
   library-internal chains are not reported.

Pass `--include-stdlib` to keep library code in the output; filter-file matches
are still hidden.

**Project-specific filtering** is done with `--filter-file <file>`: the file
holds one regular expression per line (ECMAScript syntax, blank lines and
`#`-comment lines are skipped). Every function whose demangled name,
parameter-stripped name, or mangled symbol matches an expression is hidden
exactly like library code — rules 1–3 above apply, so a noisy helper (`log_*`,
tracing wrappers, test scaffolding) disappears while the user functions it
calls stay visible. Matching is a substring search, so `log` matches everywhere
in the name and `^log_` anchors at the start. Invalid expressions are reported
on stderr and skipped, and the filter applies regardless of
`--include-stdlib`.

```
# hide logging helpers; the user functions they call are still shown
^log_
^trace_
_dbg$
```

| Flag | Description |
|------|-------------|
| *(default)* | FlowSensitive + context-sensitive SVFG, library code excluded — most complete & precise |
| `--fs` | FlowSensitive (FSSPARSE_WPA) — sparse flow-sensitive analysis [default on] |
| `--vfs` | VersionedFlowSensitive (VFS_WPA) — versioned flow-sensitive |
| `--ander` | Downgrade to flow-insensitive AndersenWaveDiff |
| `--dda` | Demand-driven value-flow analysis (ContextDDA): only the function-pointer / vtable-pointer variables feeding indirect callsites are solved on demand on the SVFG — much faster than a full points-to solve for call-graph construction |
| `--dda-flow` | Demand-driven with FlowDDA (flow-sensitive on-demand solve; implies `--dda`) |
| `--dda-cxt-len <n>` | DDA: maximum calling-context length (SVF `MaxContextLen`, default 3; larger = more precise, slower) |
| `--dda-opt-svfg` | DDA: build the optimised pointer-only SVFG (SVFGOPT — merges context-insensitive regions, drops formal-in/actual-out): smaller graph, faster on-demand traversal, some precision loss |
| `--cs` | Context-sensitive SVFG (SVF default; flag is descriptive) [default on] |
| `--heap-model` | Heap object model (`ModelConsts`, `ModelArrays`) — opt-in |
| `--include-stdlib` | Keep C++ std / C library / system-header / declaration-only library functions in the call graph [default: excluded] |
| `--entry <name>` / `--entry-file <f>` | Call-graph entry functions (repeatable; demangled or mangled) |
| `--filter-file <f>` | Hide functions matching the regexes in `<f>` (one per line), like library code: their direct user callees stay visible, their internal call tree does not |
| `--format <tsv\|tree>` | Report layout: an indented ASCII call tree per entry (default) or tab-separated spans [default: tree] |

```bash
# Default: flow-sensitive + context-sensitive (most complete & precise)
LD_LIBRARY_PATH=/home/test/src/SVF/Release-build/lib:/usr/lib/llvm-21/lib \
  ./svf-api-demo/build/clang_analyzer svf-api-demo/build/tests/test_complex.bc

# Flow-insensitive Andersen (faster, less precise)
LD_LIBRARY_PATH=/home/test/src/SVF/Release-build/lib:/usr/lib/llvm-21/lib \
  ./svf-api-demo/build/clang_analyzer --ander svf-api-demo/build/tests/test_complex.bc

# Demand-driven value-flow analysis (fastest precise mode for call graphs)
LD_LIBRARY_PATH=/home/test/src/SVF/Release-build/lib:/usr/lib/llvm-21/lib \
  ./svf-api-demo/build/clang_analyzer --dda svf-api-demo/build/tests/test_complex.bc

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

The report comes in two layouts carrying exactly the same spans, selected with
`--format` (default `tree`). The same metadata header and per-entry summary are
written in both; the hiding rules and `--filter-file` apply identically.

### Tree Format (default)

The default layout renders each entry's call chain as an indented ASCII tree:
one line per span, `├──`/`└──` connectors with `│` verticals, a call-mode
marker (`[root]`, `[direct]` or `[indirect]`, plus `[cycle]` on recursion
back-edges), and the call site in parentheses. The markers mirror the TSV
`kind` column, so every span states how it was reached. The default
output name is `<bitcode-stem>.callgraph.tree`:

```
#schema_version 2
#bitcode build/tests/test_prog.bc
#analysis pta=FlowSensitive context_sensitive=1 heap_model=0 exclude_std=1
#entries main
===== entry: main =====
main [root]
    ├── foo [direct]  (tests/test_prog.c:6)
    ├── bar [direct]  (tests/test_prog.c:7)
    └── zoo [direct]  (tests/test_prog.c:8)
===== end: main spans=4 reachable=4 =====
```

The definition file / line-range columns are not repeated on tree lines —
consult the TSV report for them. Unlike the fully streamed TSV layout, the tree
buffers one entry's spans in memory to place the connectors.

### TSV Format

`--format tsv` writes a line-oriented report (default
`<bitcode-stem>.callgraph.tsv`) so it can be streamed both when writing and
when parsing — nothing is materialized in memory. `#`-prefixed lines are
metadata/markers; every other line is one span, tab-separated, in pre-order (a
span's parent always appears before it):

```
#schema_version 2
#bitcode test_prog.bc
#analysis pta=FlowSensitive context_sensitive=1 heap_model=0 exclude_std=1
#filter_file filters.txt patterns=2
#entries main
#entry main
0		root	0	0	main	main	test_prog.c	3	6
1	0	direct	1	0	foo()	_Z3foov	test_prog.c	8	10	test_prog.c	4
2	1	indirect	2	0	bar()	_Z3barv	test_prog.c	12	14	test_prog.c	9
3	1	direct	2	1	foo()	_Z3foov	test_prog.c	8	10	test_prog.c	13
#end main spans=4 reachable_funcs=3
```

Span-line columns: `span_id`, `parent_span_id`, `kind` (`root`/`direct`/`indirect`),
`depth`, `cycle` (`0`/`1`), `function` (demangled, params stripped), `mangled`,
`file`, `start_line`, `end_line`, `call_file`, `call_line`. `call_file` and
`call_line` are the file and line of the call site backing the edge — the
location in the caller where the function is invoked (the representative site,
lowest line number, when an edge has several). Hidden functions (library code,
filter matches, and library callees reached from a call site inside a system
include header) emit no span of their own, and their internal call tree is not
traversed; their user-code callees are still shown, grafted onto the nearest
visible ancestor. A user function is never hidden merely because its call site
sits in a system header. Empty fields mean "absent" (the root has no parent;
functions without debug info have no line columns; a call without debug info
has no call-site columns). A `cycle` span is a recursion back-edge whose
subtree is not expanded. `#end` carries the entry's span count and the number
of unique reachable functions. `#filter_file` records the filter file applied and
its pattern count; it is omitted when no filter file was given.

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
