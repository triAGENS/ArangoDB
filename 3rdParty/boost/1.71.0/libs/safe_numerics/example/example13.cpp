#include <stdexcept>
#include <iostream>

#include <boost/safe_numerics/safe_integer.hpp>

int main(int, const char *[]){
    // problem: cannot recover from arithmetic errors
    std::cout << "example 7: ";
    std::cout << "cannot recover from arithmetic errors" << std::endl;
    std::cout << "Not using safe numerics" << std::endl;

    try{
        int x = 1;
        int y = 0;
        // can't do this as it will crash the program with no
        // opportunity for recovery - comment out for example
        // std::cout << x / y;
        std::cout << "error cannot be handled at runtime!" << std::endl;
    }
    catch(const std::exception &){
        std::cout << "error handled at runtime!" << std::endl;
    }
    // solution: replace int with safe<int>
    std::cout << "Using safe numerics" << std::endl;
    try{
        using namespace boost::safe_numerics;
        const safe<int> x = 1;
        const safe<int> y = 0;
        std::cout << x / y;
        std::cout << "error NOT detected!" << std::endl;
    }
    catch(const std::exception & e){
        std::cout << "error handled at runtime!" << e.what() << std::endl;
    }
    return 0;
}
