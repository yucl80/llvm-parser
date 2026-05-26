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

// ===== Main =====
int main() {
    directCall();
    testFuncPtr();
    testOverloaded();
    testVirtual();
    testVirtualDispatch();
    testLambda();
    testFuncPtrArray();
    return 0;
}
