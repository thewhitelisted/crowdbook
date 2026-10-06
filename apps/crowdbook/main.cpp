#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>

#include "crowdbook/version.hpp"

int main(int argc, char* argv[]) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};

    if (args.size() == 2 && std::string_view{args[1]} == "--version") {
        std::cout << "crowdbook " << crowdbook::version() << '\n';
        return 0;
    }

    std::cerr << "usage: crowdbook --version\n";
    return 2;
}
