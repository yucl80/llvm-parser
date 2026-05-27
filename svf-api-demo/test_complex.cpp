// Complex test case for call graph analysis
// Covers: function pointers, virtual inheritance, lambdas

#include <cstdio>
#include <functional>

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
    return 0;
}
