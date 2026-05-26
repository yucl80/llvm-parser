#include <iostream>
using namespace std;
 


int ca(int x)   {
    return x * x;
}

int add(int a, int b) {
    return a + ca(b);
}

int main() 
{
    int result = add(5, 6);
    cout << "Result: " << result << endl;
    cout << "Hello, World!";
    return 0;
}
