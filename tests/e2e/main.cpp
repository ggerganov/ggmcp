#include <cstdio>
#include <string>

#include "harness.hpp"

int main(int argc, char** argv) {
    const std::string argument = argc > 1 ? argv[1] : std::string();
    if (argument == "--list") {
        for (const std::string& name : e2e::names()) std::printf("%s\n", name.c_str());
        return 0;
    }
    if (argument == "--help" || argument == "-h") {
        std::printf("usage: ggmcp-e2e [case | --list]\n");
        return 0;
    }
    return e2e::run_all(argument);
}
