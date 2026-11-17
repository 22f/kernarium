// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Where a value points, named from the text of /proc/<pid>/maps (the guest
 * debug program, aot_guest_debug.cpp, and the host row that checks it against
 * the tombstone digest, debug_rows.py:name_value). Header only and freestanding
 * (no libc call), because the guest program is built with no headers of its
 * own.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aot_mapping_names {

inline constexpr size_t kOverflow = static_cast<size_t>(-1);

inline uint64_t parse_hex(const char *&at)
{
    uint64_t value = 0;
    for (;; ++at) {
        const char c = *at;
        if (c >= '0' && c <= '9') {
            value = value * 16 + static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value = value * 16 + static_cast<uint64_t>(c - 'a' + 10);
        } else {
            return value;
        }
    }
}

struct Line {
    uint64_t low;
    uint64_t high;
    uint64_t offset;
    const char *protection;
    const char *name;
    const char *name_end;
};

// One line of the maps text, or false at its end.
inline bool read_line(const char *&at, Line &line)
{
    if (*at == 0) {
        return false;
    }
    line.low = parse_hex(at);
    if (*at == '-') {
        ++at;
    }
    line.high = parse_hex(at);
    while (*at == ' ') {
        ++at;
    }
    line.protection = at;
    while (*at != ' ' && *at != '\n' && *at != 0) {
        ++at;
    }
    if (*at == ' ') {
        ++at;
    }
    line.offset = parse_hex(at);
    for (int field = 0; field < 2 && *at != 0; ++field) {  // device, inode
        while (*at == ' ') {
            ++at;
        }
        while (*at != ' ' && *at != '\n' && *at != 0) {
            ++at;
        }
    }
    while (*at == ' ') {
        ++at;
    }
    line.name = at;
    while (*at != '\n' && *at != 0) {
        ++at;
    }
    line.name_end = at;
    if (*at == '\n') {
        ++at;
    }
    return true;
}

// A bounded string that counts what it could not hold.
class Text {
public:
    Text(char *out, size_t capacity) : out_(out), capacity_(capacity) {}

    void put(char c)
    {
        if (length_ + 1 < capacity_) {
            out_[length_] = c;
        }
        ++length_;
    }

    void put(const char *begin, const char *end)
    {
        for (const char *c = begin; c < end; ++c) {
            put(*c);
        }
    }

    void put(const char *text)
    {
        for (; *text != 0; ++text) {
            put(*text);
        }
    }

    // Always with its prefix, zero included: 0x0.
    void hex(uint64_t value)
    {
        put("0x");
        digits(value, 16);
    }

    void decimal(uint64_t value) { digits(value, 10); }

    // The text and its length, or kOverflow if it did not fit.
    size_t finish()
    {
        if (length_ + 1 > capacity_) {
            return kOverflow;
        }
        out_[length_] = 0;
        return length_;
    }

private:
    void digits(uint64_t value, unsigned base)
    {
        char reversed[20];
        int count = 0;
        do {
            const unsigned digit = static_cast<unsigned>(value % base);
            reversed[count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
            value /= base;
        } while (value != 0);
        while (count > 0) {
            put(reversed[--count]);
        }
    }

    char *out_;
    size_t capacity_;
    size_t length_ = 0;
};

inline bool same_protection(const char *a, const char *b)
{
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
        if (a[i] == 0) {
            return true;
        }
    }
    return true;
}

// Writes the name of what `value` points into, or returns 0 if no mapping
// holds it (kOverflow if the name does not fit in `capacity`): a file's
// name and the offset in the file, a named mapping's name and the offset in
// it, an unnamed mapping's size and protection, its place among the unnamed
// mappings of that size and protection (lowest address first), how many
// there are, and the offset in it. The place and the number are found by
// counting, so no number of shapes can exhaust the formatter.
inline size_t name(const char *maps, uint64_t value, char *out, size_t capacity)
{
    const char *at = maps;
    Line line{};
    bool found = false;
    while (read_line(at, line)) {
        if (value >= line.low && value < line.high) {
            found = true;
            break;
        }
    }
    if (!found) {
        return 0;
    }
    Text text(out, capacity);
    if (line.name != line.name_end && *line.name == '/') {
        const char *base = line.name;
        for (const char *c = line.name; c < line.name_end; ++c) {
            if (*c == '/') {
                base = c + 1;
            }
        }
        text.put(base, line.name_end);
        text.put('+');
        text.hex(line.offset + value - line.low);
    } else if (line.name != line.name_end) {
        text.put(line.name, line.name_end);
        text.put('+');
        text.hex(value - line.low);
    } else {
        unsigned ordinal = 0;
        unsigned count = 0;
        at = maps;
        Line other{};
        while (read_line(at, other)) {
            if (other.name != other.name_end || other.high - other.low != line.high - line.low ||
                !same_protection(other.protection, line.protection)) {
                continue;
            }
            if (other.low == line.low) {
                ordinal = count;
            }
            ++count;
        }
        text.put("anon[");
        text.hex(line.high - line.low);
        text.put(',');
        for (int i = 0; i < 3 && line.protection[i] != 0 && line.protection[i] != ' '; ++i) {
            text.put(line.protection[i]);
        }
        text.put("]#");
        text.decimal(ordinal);
        text.put('/');
        text.decimal(count);
        text.put('+');
        text.hex(value - line.low);
    }
    return text.finish();
}

}  // namespace aot_mapping_names
