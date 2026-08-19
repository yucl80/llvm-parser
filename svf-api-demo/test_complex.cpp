// Complex test case for call graph analysis
// Covers: function pointers, virtual inheritance, lambdas, modern C++ dispatch patterns,
//         complex struct pointer passing, and C++ syntactic sugar expansion

#include <cstdio>
#include <functional>
#include <map>
#include <variant>
#include <tuple>
#include <utility>
#include <vector>

// ===== Basic functions =====
void directCall() {
    printf("direct call\n");
}

// ===== Function pointers =====
typedef void (*FuncPtr)();

void calleeViaPtr() {
    printf("called via function pointer\n");
}

void testFuncPtr() {
    FuncPtr fp = calleeViaPtr;
    fp();  // indirect call: function pointer
}

// ===== Function overloading =====
void overloaded(int x) {
    printf("overloaded int: %d\n", x);
}

void overloaded(double x) {
    printf("overloaded double: %f\n", x);
}

void testOverloaded() {
    overloaded(42);
    overloaded(3.14);
}

// ===== Virtual inheritance (single) =====
class Base {
public:
    virtual void virtualMethod() {
        printf("Base::virtualMethod\n");
    }
    virtual ~Base() {}
};

class Derived : public Base {
public:
    void virtualMethod() override {
        printf("Derived::virtualMethod\n");
    }
};

void testVirtual() {
    Base* b = new Derived();
    b->virtualMethod();  // indirect call: virtual dispatch
    delete b;
}

// ===== Multiple virtual calls =====
class Animal {
public:
    virtual void speak() { printf("Animal\n"); }
    virtual ~Animal() {}
};

class Dog : public Animal {
public:
    void speak() override { printf("Dog\n"); }
};

class Cat : public Animal {
public:
    void speak() override { printf("Cat\n"); }
};

void testVirtualDispatch() {
    Animal* animals[3];
    animals[0] = new Animal();
    animals[1] = new Dog();
    animals[2] = new Cat();

    for (int i = 0; i < 3; i++) {
        animals[i]->speak();  // indirect call: virtual dispatch
    }

    for (int i = 0; i < 3; i++) {
        delete animals[i];
    }
}

// ===== Lambdas =====
void testLambda() {
    auto lambda1 = []() {
        printf("lambda1\n");
    };
    lambda1();

    int x = 42;
    auto lambda2 = [x](int y) -> int {
        printf("lambda2: %d\n", x + y);
        return x + y;
    };
    int result = lambda2(10);
    (void)result;

    // lambda that captures by reference and calls another function
    auto lambda3 = [&x]() {
        directCall();
        x = 100;
    };
    lambda3();
}

// ===== Nested calls through function pointer array =====
void fa() { printf("a\n"); }
void fb() { printf("b\n"); }
void fc() { printf("c\n"); }

void testFuncPtrArray() {
    FuncPtr table[3] = {fa, fb, fc};
    for (int i = 0; i < 3; i++) {
        table[i]();  // indirect calls via function pointer array
    }
}

// ===== Function pointer as parameter (callback pattern) =====
typedef int (*Comparator)(int, int);

int compareAsc(int a, int b) {
    return a - b;
}

int compareDesc(int a, int b) {
    return b - a;
}

void sortWithCallback(int* arr, int n, Comparator cmp) {
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (cmp(arr[i], arr[j]) > 0) {
                int tmp = arr[i];
                arr[i] = arr[j];
                arr[j] = tmp;
            }
        }
    }
}

void testCallbackParam() {
    int arr1[] = {3, 1, 4, 1, 5};
    sortWithCallback(arr1, 5, compareAsc);
    sortWithCallback(arr1, 5, compareDesc);
}

// ===== Pointer to function pointer (double indirection) =====
void targetViaDoublePtr() {
    printf("called via pointer-to-function-pointer\n");
}

void testPtrToFuncPtr() {
    FuncPtr fp = targetViaDoublePtr;
    FuncPtr* fpp = &fp;       // pointer to function pointer
    FuncPtr** fppp = &fpp;    // pointer to pointer to function pointer

    // call through single indirection
    fp();

    // call through double indirection
    FuncPtr fp2 = *fpp;
    fp2();

    // call through triple indirection
    FuncPtr fp3 = **fppp;
    fp3();
}

// ===== Function pointer as struct member =====
typedef void (*HandlerFunc)(int);

struct Handler {
    int id;
    HandlerFunc onEvent;
};

void handleEventA(int val) {
    printf("Handler A: %d\n", val);
}

void handleEventB(int val) {
    printf("Handler B: %d\n", val);
}

void testStructFuncPtr() {
    Handler h1 = {1, handleEventA};
    Handler h2 = {2, handleEventB};

    h1.onEvent(h1.id);
    h2.onEvent(h2.id);

    // swap function pointers between structs
    HandlerFunc tmp = h1.onEvent;
    h1.onEvent = h2.onEvent;
    h2.onEvent = tmp;

    h1.onEvent(99);
    h2.onEvent(99);
}

// ===== Function returning a function pointer =====
FuncPtr chooseHandler(int kind) {
    if (kind == 0)
        return fa;
    else
        return fb;
}

void testReturnFuncPtr() {
    FuncPtr f = chooseHandler(0);
    f();

    f = chooseHandler(1);
    f();

    // chain: call through returned function pointer without intermediate variable
    chooseHandler(0)();
}

// ===== Array of pointers to function pointers =====
void testArrayOfPtrToFuncPtr() {
    FuncPtr fns[2] = {fa, fb};
    FuncPtr* fptrs[2] = {&fns[0], &fns[1]};

    // call via pointer-to-function-pointer from array
    (*fptrs[0])();
    (*fptrs[1])();
}

// ===== Nested callback: function pointer passed through multiple layers =====
void middleWare(int x, int y, Comparator cmp, void (*report)(int)) {
    int result = cmp(x, y);
    report(result);
}

void reportResult(int val) {
    printf("result: %d\n", val);
}

void testNestedCallback() {
    // Comparator passed through middleware
    middleWare(10, 20, compareAsc, reportResult);
    middleWare(10, 20, compareDesc, reportResult);
}

// ================================================================
// Additional Complex Scenarios for SVF Call Graph Analysis
// ================================================================

// ===== 14. Multiple Inheritance =====
struct MixinA {
    virtual void featureA() { printf("MixinA::featureA\n"); }
    virtual ~MixinA() {}
};

struct MixinB {
    virtual void featureB() { printf("MixinB::featureB\n"); }
    virtual ~MixinB() {}
};

class MultiDerived : public MixinA, public MixinB {
public:
    void featureA() override { printf("MultiDerived::featureA\n"); }
    void featureB() override { printf("MultiDerived::featureB\n"); }
};

void testMultipleInheritance() {
    MultiDerived obj;
    MixinA* a = &obj;
    MixinB* b = &obj;
    a->featureA();  // virtual dispatch through first base
    b->featureB();  // virtual dispatch through second base (this pointer adjustment)
}

// ===== 15. Diamond Virtual Inheritance =====
class DiamondBase {
public:
    virtual void baseMethod() { printf("DiamondBase::baseMethod\n"); }
    virtual ~DiamondBase() {}
};

class MidLeft : virtual public DiamondBase {
public:
    void baseMethod() override { printf("MidLeft::baseMethod\n"); }
};

class MidRight : virtual public DiamondBase {
public:
    void baseMethod() override { printf("MidRight::baseMethod\n"); }
};

class DiamondTip : public MidLeft, public MidRight {
public:
    void baseMethod() override { printf("DiamondTip::baseMethod\n"); }
};

void testDiamondVirtualInheritance() {
    DiamondTip tip;
    DiamondBase* base = &tip;
    base->baseMethod();  // virtual dispatch through virtual base

    MidLeft* ml = &tip;
    ml->baseMethod();    // virtual dispatch through MidLeft
}

// ===== 16. std::function wrapping =====
void stdFuncTarget1() { printf("std::function target 1\n"); }
void stdFuncTarget2() { printf("std::function target 2\n"); }

void testStdFunction() {
    std::function<void()> f1 = stdFuncTarget1;
    f1();

    std::function<void()> f2 = stdFuncTarget2;
    f2();

    // std::function wrapping a lambda
    std::function<void()> f3 = []() {
        printf("std::function wrapping lambda\n");
    };
    f3();

    // reassign and call
    f1 = stdFuncTarget2;
    f1();
}

// ===== 17. Member Function Pointers =====
class Calculator {
public:
    int add(int a, int b) { printf("add: %d\n", a + b); return a + b; }
    int sub(int a, int b) { printf("sub: %d\n", a - b); return a - b; }
    int mul(int a, int b) { printf("mul: %d\n", a * b); return a * b; }
};

void testMemberFuncPtr() {
    Calculator calc;
    Calculator* pcalc = &calc;

    typedef int (Calculator::*MathOp)(int, int);
    MathOp op = &Calculator::add;
    (calc.*op)(10, 5);    // call via .*

    op = &Calculator::sub;
    (pcalc->*op)(10, 5);  // call via ->*

    op = &Calculator::mul;
    (calc.*op)(10, 5);
}

// ===== 18. Functor (Function Object) =====
class Greeter {
    const char* prefix;

public:
    Greeter(const char* p) : prefix(p) {}
    void operator()(const char* name) const {
        printf("%s, %s!\n", prefix, name);
    }
};

void testFunctor() {
    Greeter hello("Hello");
    Greeter goodbye("Goodbye");

    hello("World");
    goodbye("World");
}

// ===== 19. Recursion =====
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);  // direct recursion
}

void mutualB(int n);

void mutualA(int n) {
    if (n > 0) {
        printf("mutualA: %d\n", n);
        mutualB(n - 1);
    }
}

void mutualB(int n) {
    if (n > 0) {
        printf("mutualB: %d\n", n);
        mutualA(n - 1);
    }
}

void testRecursion() {
    factorial(5);
    mutualA(3);
}

// ===== 20. Function Templates =====
template <typename T>
T maxOf(T a, T b) {
    T result = (a > b) ? a : b;
    printf("maxOf\n");
    return result;
}

template <typename T>
void printType(T val) {
    (void)val;
    printf("printType called\n");
}

void testFunctionTemplates() {
    maxOf(3, 7);        // int instantiation
    maxOf(3.14, 2.72);  // double instantiation

    printType(42);       // int
    printType(3.14f);    // float
    printType("hello");  // const char*
}

// ===== 21. Generic Lambdas (C++14) =====
void testGenericLambda() {
    auto generic = [](auto a, auto b) {
        printf("generic lambda\n");
    };
    generic(1, 2);
    generic(3.14, 2.72);

    // lambda with capture by move (C++14)
    auto ptr = new int(42);
    auto moveCapture = [p = ptr]() {
        printf("move capture: %d\n", *p);
        delete p;
    };
    moveCapture();
}

// ===== 22. std::bind =====
void bindTarget(int a, int b, int c) {
    printf("bind: %d %d %d\n", a, b, c);
}

void testBind() {
    using namespace std::placeholders;

    auto bound1 = std::bind(bindTarget, 1, 2, 3);
    bound1();

    auto bound2 = std::bind(bindTarget, _1, _2, 10);
    bound2(5, 6);

    // std::function wrapping a bind
    std::function<void()> fb = std::bind(bindTarget, 7, 8, 9);
    fb();
}

// ===== 23. Virtual Calls in Constructor/Destructor =====
class TraceBase {
public:
    TraceBase() { printf("TraceBase ctor\n"); }
    virtual void log() { printf("TraceBase::log\n"); }
    virtual ~TraceBase() { log(); }
};

class TraceDerived : public TraceBase {
public:
    TraceDerived() { log(); }  // virtual call during construction
    void log() override { printf("TraceDerived::log\n"); }
    ~TraceDerived() override { log(); }  // virtual call during destruction
};

void testVirtualCtorDtor() {
    TraceDerived d;
}

// ===== 24. Abstract Base Class with Pure Virtual Dispatch =====
class AbstractBase {
public:
    virtual void doit() = 0;
    virtual void report() = 0;
    virtual ~AbstractBase() {}
};

class ConcreteA : public AbstractBase {
public:
    void doit() override { printf("ConcreteA::doit\n"); }
    void report() override { printf("ConcreteA::report\n"); }
};

class ConcreteB : public AbstractBase {
public:
    void doit() override { printf("ConcreteB::doit\n"); }
    void report() override { printf("ConcreteB::report\n"); }
};

void testAbstractDispatch() {
    AbstractBase* objs[2];
    objs[0] = new ConcreteA();
    objs[1] = new ConcreteB();

    for (int i = 0; i < 2; i++) {
        objs[i]->doit();
        objs[i]->report();
        delete objs[i];
    }
}

// ===== 25. CRTP (Curiously Recurring Template Pattern) =====
template <typename Derived>
class ShapeBase {
public:
    void draw() { static_cast<Derived*>(this)->drawImpl(); }
    const char* name() const { return static_cast<const Derived*>(this)->nameImpl(); }
};

class Circle : public ShapeBase<Circle> {
public:
    void drawImpl() { printf("Circle::draw\n"); }
    const char* nameImpl() const { return "Circle"; }
};

class Square : public ShapeBase<Square> {
public:
    void drawImpl() { printf("Square::draw\n"); }
    const char* nameImpl() const { return "Square"; }
};

void testCRTP() {
    Circle c;
    Square s;
    c.draw();
    s.draw();
}

// ===== 26. Ternary/Conditional Function Pointer Dispatch =====
void condTrue() { printf("condTrue\n"); }
void condFalse() { printf("condFalse\n"); }

void testTernaryDispatch() {
    int flag = 1;
    FuncPtr f = (flag ? condTrue : condFalse);
    f();

    flag = 0;
    f = (flag == 0 ? condFalse : condTrue);
    f();
}

// ===== 27. State Machine with Function Pointer Table =====
typedef void (*StateHandler)(int);

void stateIdle(int evt) { printf("Idle state handling event %d\n", evt); }
void stateRunning(int evt) { printf("Running state handling event %d\n", evt); }
void stateStopped(int evt) { printf("Stopped state handling event %d\n", evt); }

void testStateMachine() {
    StateHandler states[3] = {stateIdle, stateRunning, stateStopped};

    for (int state = 0; state < 3; state++) {
        states[state](42 + state);
    }
}

// ===== 28. Default Arguments (expand at call site) =====
void defaultArgsFunc(int a, int b = 10, const char* label = "default") {
    printf("%s: %d %d\n", label, a, b);
}

void testDefaultArgs() {
    defaultArgsFunc(1);         // uses both defaults
    defaultArgsFunc(2, 20);     // uses label default
    defaultArgsFunc(3, 30, "explicit");
}

// ===== 29. Variadic Function Template =====
void varargSink() { printf("base case\n"); }

template <typename T, typename... Args>
void varargSink(T val, Args... rest) {
    printf("vararg: %d\n", (int)val);
    varargSink(rest...);
}

void testVariadicTemplate() {
    varargSink(1, 2, 3, 4, 5);
}

// ===== 30. Nested Lambda (Lambda returning Lambda) =====
void testNestedLambda() {
    auto makeAdder = [](int base) {
        return [base](int x) {
            printf("nested lambda: %d\n", base + x);
        };
    };

    auto add5 = makeAdder(5);
    auto add10 = makeAdder(10);
    add5(3);
    add10(3);
}

// ===== 31. Function pointer reassignment in loop =====
void loopFnA() { printf("loopFnA\n"); }
void loopFnB() { printf("loopFnB\n"); }
void loopFnC() { printf("loopFnC\n"); }

void testLoopFuncPtrReassign() {
    FuncPtr fns[3] = {loopFnA, loopFnB, loopFnC};
    FuncPtr fp = nullptr;
    for (int i = 0; i < 3; i++) {
        fp = fns[i];
        fp();
    }
}

// ===== 32. Operator overloading that forwards to functions =====
void opFuncA() { printf("operator A\n"); }
void opFuncB() { printf("operator B\n"); }

void testOperatorOverloadCall() {
    // Test that overloaded operators ultimately call functions
    int a = 10, b = 20;
    int c = a + b;  // built-in operator, just generates a call to the operator
    printf("op result: %d\n", c);
    (void)c;
}

// ===== 33. Ternary with function pointer inside struct =====
struct DispatchEntry {
    const char* name;
    FuncPtr handler;
};

void dispatchAlpha() { printf("alpha\n"); }
void dispatchBeta() { printf("beta\n"); }
void dispatchGamma() { printf("gamma\n"); }

void testStructDispatchTable() {
    DispatchEntry table[3] = {
        {"alpha", dispatchAlpha},
        {"beta", dispatchBeta},
        {"gamma", dispatchGamma},
    };

    for (int i = 0; i < 3; i++) {
        table[i].handler();
    }
}

// ===== 34. std::invoke (C++17) =====
void invokeTarget(int v) {
    printf("invoke: %d\n", v);
}

void testStdInvoke() {
    std::invoke(invokeTarget, 42);

    Calculator calc;
    std::invoke(&Calculator::add, calc, 100, 200);

    auto lam = [](const char* s) { printf("invoke lambda: %s\n", s); };
    std::invoke(lam, "hello");
}

// ===== 35. Multi-level virtual inheritance chain =====
class VBase {
public:
    virtual void action() { printf("VBase::action\n"); }
    virtual ~VBase() {}
};

class VMid : public VBase {
public:
    void action() override { printf("VMid::action\n"); }
};

class VDerived : public VMid {
public:
    void action() override { printf("VDerived::action\n"); }
};

void testMultiLevelVirtual() {
    VBase* bases[3];
    bases[0] = new VBase();
    bases[1] = new VMid();
    bases[2] = new VDerived();

    for (int i = 0; i < 3; i++) {
        bases[i]->action();
        delete bases[i];
    }
}

// ===== 36. std::mem_fn =====
void testMemFn() {
    Calculator calc;

    auto addFn = std::mem_fn(&Calculator::add);
    addFn(calc, 20, 10);

    auto subFn = std::mem_fn(&Calculator::sub);
    subFn(&calc, 20, 10);
}

// ===== 37. std::bind with member function =====
void testBindMemberFunc() {
    Calculator calc;
    using namespace std::placeholders;

    // bind member function with instance pointer
    auto boundAdd = std::bind(&Calculator::add, &calc, _1, _2);
    boundAdd(30, 15);

    auto boundSub = std::bind(&Calculator::sub, &calc, _1, _2);
    boundSub(30, 15);
}

// ===== 38. Delegate pattern (std::function as class member) =====
class Button {
    std::function<void()> onClick_;
    std::function<void(int)> onKey_;

public:
    void setOnClick(std::function<void()> cb) { onClick_ = std::move(cb); }
    void setOnKey(std::function<void(int)> cb) { onKey_ = std::move(cb); }

    void click() {
        if (onClick_) onClick_();
    }

    void keyPress(int k) {
        if (onKey_) onKey_(k);
    }
};

void delegateClickA() { printf("Button A clicked\n"); }
void delegateClickB() { printf("Button B clicked\n"); }
void delegateKeyHandler(int code) { printf("Key pressed: %d\n", code); }

void testDelegatePattern() {
    Button btn1, btn2;

    btn1.setOnClick(delegateClickA);
    btn2.setOnClick(delegateClickB);
    btn1.setOnKey(delegateKeyHandler);
    btn2.setOnKey(delegateKeyHandler);

    btn1.click();
    btn2.click();
    btn1.keyPress(42);
    btn2.keyPress(99);

    // reassign delegate dynamically
    btn1.setOnClick(delegateClickB);
    btn1.click();
}

// ===== 39. Dispatch table using std::map =====
void mapCmdStart() { printf("cmd: start\n"); }
void mapCmdStop() { printf("cmd: stop\n"); }
void mapCmdStatus() { printf("cmd: status\n"); }

void testMapDispatch() {
    std::map<int, FuncPtr> dispatch;
    dispatch[0] = mapCmdStart;
    dispatch[1] = mapCmdStop;
    dispatch[2] = mapCmdStatus;

    for (int i = 0; i < 3; i++) {
        auto it = dispatch.find(i);
        if (it != dispatch.end()) {
            it->second();
        }
    }
}

// ===== 40. if constexpr dispatch (C++17) =====
template <bool UseFast>
void algoDispatch() {
    if constexpr (UseFast) {
        printf("fast path\n");
    } else {
        printf("slow path\n");
    }
}

template <typename T>
void processIfConstexpr(T val) {
    if constexpr (std::is_integral_v<T>) {
        printf("integral: %d\n", (int)val);
    } else {
        printf("non-integral\n");
    }
}

void testIfConstexpr() {
    algoDispatch<true>();
    algoDispatch<false>();

    processIfConstexpr(42);
    processIfConstexpr(3.14);
}

// ===== 41. Fold expression calling functions (C++17) =====
void foldTarget(int v) { printf("fold target: %d\n", v); }

template <typename... Args>
void multiCallFold(Args... args) {
    (foldTarget(args), ...);  // fold over comma operator
}

template <typename... Fns>
void callAll(Fns... fns) {
    (fns(), ...);  // expand function pointer pack with comma fold
}

void foldFnA() { printf("fold fn A\n"); }
void foldFnB() { printf("fold fn B\n"); }
void foldFnC() { printf("fold fn C\n"); }

void testFoldExpression() {
    multiCallFold(10, 20, 30);
    callAll(foldFnA, foldFnB, foldFnC);
}

// ===== 42. std::apply with function and tuple =====
int applyTarget(int a, double b, const char* c) {
    printf("apply: %d %.1f %s\n", a, b, c);
    return a;
}

void testStdApply() {
    auto tup = std::make_tuple(42, 3.14, "hello");
    std::apply(applyTarget, tup);

    // apply with lambda
    std::apply([](int x, int y) { printf("apply lambda: %d\n", x + y); },
               std::make_tuple(10, 20));
}

// ===== 43. Overloaded lambda pattern (C++17) =====
template <typename... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

void overloadIntFn(int v) { printf("overloaded int: %d\n", v); }
void overloadStrFn(const char* s) { printf("overloaded str: %s\n", s); }

void testOverloadedLambda() {
    auto visitor = Overloaded{
        [](int v) { overloadIntFn(v); },
        [](const char* s) { overloadStrFn(s); },
    };

    visitor(42);
    visitor("hello world");
}

// ===== 44. std::variant + std::visit =====
void variantIntHandler(int v) { printf("variant int: %d\n", v); }
void variantDoubleHandler(double v) { printf("variant double: %f\n", v); }
void variantStrHandler(const char* v) { printf("variant str: %s\n", v); }

using VariantType = std::variant<int, double, const char*>;

void testVariantVisit() {
    VariantType vars[3] = {42, 3.14, "variant"};

    for (auto& v : vars) {
        std::visit(Overloaded{
            [](int x) { variantIntHandler(x); },
            [](double x) { variantDoubleHandler(x); },
            [](const char* x) { variantStrHandler(x); },
        }, v);
    }
}

// ===== 45. Recursive lambda via std::function =====
void testRecursiveLambda() {
    std::function<int(int)> fib = [&fib](int n) -> int {
        if (n <= 1) return n;
        printf("fib(%d)\n", n);
        return fib(n - 1) + fib(n - 2);
    };

    int r = fib(4);
    printf("fib result: %d\n", r);
    (void)r;
}

// ================================================================
// 46+: Complex Struct Pointer Passing & C++ Syntactic Sugar
// ================================================================

// ===== 46. Nested struct holding a function pointer, passed by pointer =====
typedef void (*OpFunc)(int);

struct InnerOps {
    int base;
    OpFunc apply;    // function pointer inside nested struct
};

struct Wrapper {
    int id;
    InnerOps ops;    // nested struct
    OpFunc fallback; // function pointer in the outer struct
};

void opDouble(int v) { printf("double: %d\n", v * 2); }
void opTriple(int v) { printf("triple: %d\n", v * 3); }
void opFallback(int v) { printf("fallback: %d\n", v); }

// struct pointer passed through a function: dispatch on nested member
void processInner(Wrapper* w, int v) {
    w->ops.apply(v + w->ops.base);
}

// struct pointer passed through TWO function layers
void routeToInner(Wrapper* w) {
    processInner(w, w->id);
    w->fallback(w->id);
}

void testNestedStructPtr() {
    Wrapper w1 = {1, {2, opDouble}, opFallback};
    Wrapper w2 = {3, {4, opTriple}, opFallback};
    routeToInner(&w1);
    routeToInner(&w2);
}

// ===== 47. Linked list of structs, each node dispatches its own handler =====
struct TaskNode {
    int priority;
    TaskNode* next;
    void (*run)(TaskNode*);   // callback receives the struct pointer itself
};

void runLow(TaskNode* t) { printf("low prio %d\n", t->priority); }
void runHigh(TaskNode* t) { printf("high prio %d\n", t->priority); }

void runQueue(TaskNode* head) {
    // loop-carried points-to through n = n->next
    for (TaskNode* n = head; n != nullptr; n = n->next) {
        n->run(n);
    }
}

void testLinkedStructList() {
    TaskNode t1 = {1, nullptr, runLow};
    TaskNode t2 = {2, nullptr, runHigh};
    t1.next = &t2;
    t2.next = nullptr;
    runQueue(&t1);
}

// ===== 48. Binary tree recursion with per-node dispatch =====
struct TreeNode {
    int key;
    TreeNode* left;
    TreeNode* right;
    int (*visit)(TreeNode*);
};

int visitLeaf(TreeNode* n) { printf("leaf %d\n", n->key); return n->key; }
int visitInternal(TreeNode* n) { printf("internal %d\n", n->key); return n->key; }

void walkTree(TreeNode* n) {
    if (!n) return;
    n->visit(n);
    walkTree(n->left);   // recursion over struct pointers
    walkTree(n->right);
}

void testTreeStructPtr() {
    TreeNode leaf1 = {1, nullptr, nullptr, visitLeaf};
    TreeNode leaf2 = {2, nullptr, nullptr, visitLeaf};
    TreeNode root  = {0, &leaf1, &leaf2, visitInternal};
    walkTree(&root);
}

// ===== 49. Function pointer that takes a struct pointer (callback) =====
typedef int (*Transform)(struct Point2D*);
struct Point2D { int x, y; };

int scalePoint(Point2D* p) { printf("scale\n"); p->x *= 2; return p->x; }
int negatePoint(Point2D* p) { printf("negate\n"); p->y = -p->y; return p->y; }

void applyTransform(Point2D* p, Transform t) {
    t(p);   // struct pointer handed to an indirect callee
}

void testStructPtrCallback() {
    Point2D pt = {3, 4};
    applyTransform(&pt, scalePoint);
    applyTransform(&pt, negatePoint);
}

// ===== 50. Pointer-to-pointer-to-struct with target mutation =====
struct Gadget {
    void (*fire)(void);
};

void gadgetA() { printf("gadget A\n"); }
void gadgetB() { printf("gadget B\n"); }

void testDoublePtrStruct() {
    Gadget g1 = {gadgetA};
    Gadget g2 = {gadgetB};
    Gadget* pg = &g1;
    Gadget** ppg = &pg;

    (*ppg)->fire();    // dispatch through double pointer
    *ppg = &g2;        // retarget the pointer
    (*ppg)->fire();
}

// ===== 51. Array of structs passed by pointer; self-pointer callbacks =====
struct Slot {
    const char* name;
    void (*work)(Slot*);
};

void workA(Slot* s) { printf("slot A: %s\n", s->name); }
void workB(Slot* s) { printf("slot B: %s\n", s->name); }

void runSlots(Slot* slots, int count) {
    for (int i = 0; i < count; i++) {
        slots[i].work(&slots[i]);   // each element points to itself
    }
}

void testStructArrayPtr() {
    Slot slots[3] = {
        {"a", workA},
        {"b", workB},
        {"c", workA},
    };
    runSlots(slots, 3);
}

// ===== 52. Struct field pointing to a function pointer (double indirection) =====
// NOTE: the fn-pointers arrive via memcpy from a *global constant* array,
// so this resolves only with --heap-model (ModelConsts), not baseline Andersen.
struct Registry {
    void (**slot)();
};

void regA() { printf("reg A\n"); }
void regB() { printf("reg B\n"); }

void testStructPtrToFuncPtr() {
    void (*fns[2])() = {regA, regB};
    Registry r = {&fns[0]};
    (*(r.slot))();    // load fn-ptr-ptr, load fn-ptr, indirect call
    r.slot = &fns[1];
    (*(r.slot))();
}

// ===== 53. Setter function writing a function pointer into a struct =====
struct Service {
    void (*handler)(int);
};

void setHandler(Service* s, void (*h)(int)) {
    s->handler = h;   // interprocedural store into struct field
}

void svcA(int v) { printf("svc A %d\n", v); }
void svcB(int v) { printf("svc B %d\n", v); }

void testStructPtrSetter() {
    Service s;
    setHandler(&s, svcA);
    s.handler(1);
    setHandler(&s, svcB);
    s.handler(2);
}

// ===== 54. Range-based for over a vector of std::function (sugar) =====
void rfA() { printf("rf A\n"); }
void rfB() { printf("rf B\n"); }

void testRangeFor() {
    std::vector<std::function<void()>> tasks;
    tasks.push_back(rfA);
    tasks.push_back(rfB);
    tasks.push_back([]() { printf("rf lambda\n"); });

    // desugars to begin()/end()/operator!=/operator++/operator* + call
    for (auto& f : tasks) {
        f();
    }
}

// ===== 55. Virtual dispatch through a reference parameter =====
void speakViaRef(Animal& a) {
    a.speak();   // references are pointer aliases in IR
}

void testVirtualRefParam() {
    Dog dog;
    Cat cat;
    speakViaRef(dog);
    speakViaRef(cat);
}

// ===== 56. dynamic_cast downcast then virtual call (RTTI sugar) =====
void testDynamicCast() {
    Animal* a = new Dog();
    Dog* d = dynamic_cast<Dog*>(a);   // emits __dynamic_cast runtime helper
    if (d) {
        d->speak();
    }
    delete a;
}

// ===== 57. new[]/delete[] array allocation sugar =====
void testArrayNewDelete() {
    Animal** zoo = new Animal*[2];
    zoo[0] = new Dog();
    zoo[1] = new Cat();
    for (int i = 0; i < 2; i++) zoo[i]->speak();
    for (int i = 0; i < 2; i++) delete zoo[i];
    delete[] zoo;
}

// ===== 58. Move / copy semantics (std::move sugar) =====
struct Mover {
    int payload;
    Mover() : payload(0) { printf("Mover ctor\n"); }
    Mover(const Mover& o) : payload(o.payload) { printf("Mover copy\n"); }
    Mover(Mover&& o) noexcept : payload(o.payload) {
        printf("Mover move\n");
        o.payload = 0;
    }
};

Mover makeMover() {
    Mover m;
    return m;   // move (copy elision disabled at -O0)
}

void testMoveSemantics() {
    Mover a;
    Mover b = std::move(a);   // move ctor
    Mover c = a;              // copy ctor
    Mover d = makeMover();    // ctor + move from return
    (void)b; (void)c; (void)d;
}

// ===== 59. Ternary function pointer passed directly as an argument =====
void onTrue() { printf("ternary true\n"); }
void onFalse() { printf("ternary false\n"); }

void callOnce(FuncPtr f) { f(); }

void testTernaryAsArg() {
    int flag = 1;
    callOnce(flag ? onTrue : onFalse);   // no intermediate variable
    flag = 0;
    callOnce(flag ? onTrue : onFalse);
}

// ===== 60. Operator overloading sugar (operator[] / operator->) =====
class Vector2 {
    double data[2];
public:
    Vector2(double x, double y) { data[0] = x; data[1] = y; }
    double& operator[](int i) { return data[i]; }
};

void testSubscriptOperator() {
    Vector2 v(1.0, 2.0);
    v[0] = v[0] * 2.0;              // reads + writes via operator[]
    double sum = v[0] + v[1];
    printf("sum: %f\n", sum);
}

class Counted {
    int v;
public:
    Counted() : v(7) {}
    int get() const { return v; }
};

class SmartPtr {
    Counted* ptr;
public:
    SmartPtr(Counted* c) : ptr(c) {}
    Counted* operator->() const { return ptr; }
};

void testArrowOperator() {
    Counted c;
    SmartPtr sp(&c);
    int x = sp->get();   // desugars to (sp.operator->())->get()
    printf("smart: %d\n", x);
}

// ===== 61. Magic static: guarded static local initialization =====
int initCounter() {
    printf("initCounter\n");
    return 100;
}

int getCounter() {
    static int counter = initCounter();   // __cxa_guard_acquire/release
    return counter;
}

void testMagicStatic() {
    int a = getCounter();
    int b = getCounter();
    printf("counter: %d %d\n", a, b);
}

// ===== 62. Structured bindings (C++17 sugar) =====
void testStructuredBinding() {
    auto pair = std::make_pair(1, 2.5);
    auto [first, second] = pair;   // expands to std::get<0>/std::get<1>
    printf("sb: %d %.1f\n", first, second);
}

// ===== 63. Delegating constructor =====
class Deleg {
public:
    Deleg() : Deleg(0, 0) { printf("Deleg default\n"); }
    Deleg(int a, int b) { printf("Deleg(%d,%d)\n", a, b); }
};

void testDelegatingCtor() {
    Deleg d;   // Deleg() -> Deleg(int,int)
}

// ===== 64. Static member functions + pointer to static member =====
struct Utility {
    static void boot() { printf("Utility::boot\n"); }
    static void halt() { printf("Utility::halt\n"); }
};

void testStaticMemberFuncPtr() {
    Utility::boot();                 // direct static call
    void (*sf)() = &Utility::halt;   // pointer to static member
    sf();

    typedef void (*StaticFn)();
    StaticFn table[2] = {Utility::boot, Utility::halt};
    table[0]();
    table[1]();
}

// ===== 65. Abstract factory returning a base pointer (return-value pts) =====
Animal* makeAnimal(int kind) {
    if (kind == 0) return new Dog();
    return new Cat();
}

void testFactoryPattern() {
    Animal* a = makeAnimal(0);
    Animal* b = makeAnimal(1);
    a->speak();   // virtual call resolved from factory return value
    b->speak();
    delete a;
    delete b;
}

// ===== 66. extern "C" linkage and anonymous namespace =====
extern "C" void cLinkageFunc() {
    printf("extern C\n");
}

namespace {
void hiddenHelper() {
    printf("anon ns helper\n");
}
void hiddenEntry() {
    hiddenHelper();
}
}

void testLinkageVariants() {
    cLinkageFunc();
    hiddenEntry();
}

// ===== 67. Pointer to virtual member function (a->*sf) =====
// NOTE: the target address is computed from the vtable index, so SVF
// generally cannot resolve this indirect call (exercises the hard case).
void testVirtualMemberPtr() {
    Animal* a = new Dog();
    typedef void (Animal::*SpeakFn)();
    SpeakFn sf = &Animal::speak;
    (a->*sf)();   // vtable-index call via pointer-to-member
    delete a;
}

// ===== 68. constexpr function invoked in runtime context =====
constexpr int square(int x) { return x * x; }

void testConstexprRuntime(int runtimeInput) {
    int n = square(runtimeInput);   // non-constant argument => real call
    printf("square: %d\n", n);
}

// ===== Main =====
int main() {
    directCall();
    testFuncPtr();
    testOverloaded();
    testVirtual();
    testVirtualDispatch();
    testLambda();
    testFuncPtrArray();
    testCallbackParam();
    testPtrToFuncPtr();
    testStructFuncPtr();
    testReturnFuncPtr();
    testArrayOfPtrToFuncPtr();
    testNestedCallback();

    // New test scenarios
    testMultipleInheritance();
    testDiamondVirtualInheritance();
    testStdFunction();
    testMemberFuncPtr();
    testFunctor();
    testRecursion();
    testFunctionTemplates();
    testGenericLambda();
    testBind();
    testVirtualCtorDtor();
    testAbstractDispatch();
    testCRTP();
    testTernaryDispatch();
    testStateMachine();
    testDefaultArgs();
    testVariadicTemplate();
    testNestedLambda();
    testLoopFuncPtrReassign();
    testOperatorOverloadCall();
    testStructDispatchTable();
    testStdInvoke();
    testMultiLevelVirtual();
    testMemFn();
    testBindMemberFunc();
    testDelegatePattern();
    testMapDispatch();
    testIfConstexpr();
    testFoldExpression();
    testStdApply();
    testOverloadedLambda();
    testVariantVisit();
    testRecursiveLambda();

    // Complex struct pointer passing
    testNestedStructPtr();
    testLinkedStructList();
    testTreeStructPtr();
    testStructPtrCallback();
    testDoublePtrStruct();
    testStructArrayPtr();
    testStructPtrToFuncPtr();
    testStructPtrSetter();

    // C++ syntactic sugar
    testRangeFor();
    testVirtualRefParam();
    testDynamicCast();
    testArrayNewDelete();
    testMoveSemantics();
    testTernaryAsArg();
    testSubscriptOperator();
    testArrowOperator();
    testMagicStatic();
    testStructuredBinding();
    testDelegatingCtor();
    testStaticMemberFuncPtr();
    testFactoryPattern();
    testLinkageVariants();
    testVirtualMemberPtr();
    testConstexprRuntime(5);
    return 0;
}
