// Complex test case for call graph analysis
// Covers: function pointers, virtual inheritance, lambdas

#include <cstdio>

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
    return 0;
}
