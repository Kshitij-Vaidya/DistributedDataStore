#include "novacache/version.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "novacache-server " << novacache::version() << '\n';
        return 0;
    }

    std::cerr << "novacache-server: networking is not implemented until Phase 1\n"
              << "Run with --version to verify this Phase 0 build.\n";
    return 1;
}
