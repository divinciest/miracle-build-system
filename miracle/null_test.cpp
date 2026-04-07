#include <iostream>
#include <sstream>
#include <string>

int main() {
    try {
        std::stringstream ss;
        const char* p = nullptr;
        ss << p;
        std::cout << "Successfully streamed null: " << ss.str() << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Caught: " << e.what() << std::endl;
    }
    
    try {
        const char* p = nullptr;
        std::string s(p);
        std::cout << "Successfully constructed string from null" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "Caught string construction from null: " << e.what() << std::endl;
    }
    return 0;
}
