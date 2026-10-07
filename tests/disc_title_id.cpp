#include "platform/disc.h"
#include <exception>
#include <iomanip>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        std::cout << std::hex << std::setfill('0') << std::setw(16)
                  << mscharged::ReadDiscTitleId(argv[1]) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
