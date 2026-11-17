// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Names the values given on the command line (hexadecimal) by the maps text
 * in a file, with the formatter the guest debug program prints its registers
 * with (tests/integration/aot/mapping_names.hpp), one line per value: the
 * name, "none" when no mapping holds the value, "overflow" when the name does
 * not fit the buffer. test_aot_mapping_names.py compares the lines with the
 * digest's own naming.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "mapping_names.hpp"

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <maps file> <hex value>...\n", argv[0]);
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "%s: cannot read\n", argv[1]);
        return 2;
    }
    const std::string maps((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    for (int i = 2; i < argc; ++i) {
        const uint64_t value = std::strtoull(argv[i], nullptr, 16);
        char name[256];
        const size_t length = aot_mapping_names::name(maps.c_str(), value, name, sizeof(name));
        if (length == 0) {
            std::puts("none");
        } else if (length == aot_mapping_names::kOverflow) {
            std::puts("overflow");
        } else {
            std::puts(name);
        }
    }
    return 0;
}
