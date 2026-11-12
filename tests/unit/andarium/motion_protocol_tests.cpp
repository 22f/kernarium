// SPDX-License-Identifier: GPL-2.0-or-later
//
// The motion wire (vsock 5556) from the guest's side: the sensor records the
// host sends, the control frames the sub-HAL sends, and the orientation part of
// both against the golden frames. argv[1] is the fixture,
// tests/unit/andarium/fixtures/motion-orientation-golden-frames.txt.
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "motion_protocol.hpp"
#include "require.hpp"

using uml::testing::require;
using Bytes = std::vector<uint8_t>;

namespace {
Bytes parse_hex(const std::string& text)
{
    Bytes bytes;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token) {
        require(token.size() == 2, "a fixture byte is two hex digits");
        bytes.push_back(uint8_t(std::stoul(token, nullptr, 16)));
    }
    return bytes;
}

// `name: <hex bytes>` per line; a line that starts with # is a comment.
std::map<std::string, Bytes> read_fixture(const char* path)
{
    std::ifstream file(path);
    require(file.is_open(), "the golden-frame fixture opens");
    std::map<std::string, Bytes> frames;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto colon = line.find(':');
        require(colon != std::string::npos, "a fixture line is name: bytes");
        require(frames.emplace(line.substr(0, colon), parse_hex(line.substr(colon + 1))).second,
                "fixture names are unique");
    }
    return frames;
}

bool starts_with(const std::string& text, const char* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

// The oracle for the orientation records, written out here independently of
// the decoder: the host sends 20 bytes "ANS1", type 27 and one of four whole
// numbers, then two zeros.
bool host_may_send_orientation(const Bytes& frame)
{
    if (frame.size() != 20 || frame[0] != 'A' || frame[1] != 'N' || frame[2] != 'S' || frame[3] != '1' ||
        frame[4] != 27 || frame[5] != 0 || frame[6] != 0 || frame[7] != 0) {
        return false;
    }
    static const Bytes orientations[] = {{0, 0, 0, 0}, {0, 0, 0x80, 0x3f}, {0, 0, 0, 0x40}, {0, 0, 0x40, 0x40}};
    bool known = false;
    for (const auto& orientation : orientations) known |= Bytes(frame.begin() + 8, frame.begin() + 12) == orientation;
    return known && Bytes(frame.begin() + 12, frame.end()) == Bytes(8, 0);
}

void check_fixture(const std::map<std::string, Bytes>& fixture)
{
    // The guest's state behind each control frame of the fixture.
    struct State {
        std::array<bool, 2> active;
        std::array<int64_t, 2> period;
        std::optional<bool> orientation;
    };
    const std::map<std::string, State> controls = {
        {"control-v1-idle", {{false, false}, {10'000'000, 10'000'000}, std::nullopt}},
        {"control-v1-motion", {{true, true}, {20'000'000, 1'000'000'000}, std::nullopt}},
        {"control-v2-idle", {{false, false}, {10'000'000, 10'000'000}, false}},
        {"control-v2-orientation", {{false, false}, {10'000'000, 10'000'000}, true}},
        {"control-v2-all", {{true, true}, {20'000'000, 1'000'000'000}, true}},
    };
    size_t records = 0;
    size_t rejected_records = 0;
    size_t frames = 0;
    for (const auto& [name, bytes] : fixture) {
        if (starts_with(name, "orientation-")) {
            ++records;
            require(host_may_send_orientation(bytes), name);
            const auto sample = andarium::motion::decode(bytes);
            require(sample.has_value(), name);
            require(sample->type == andarium::motion::orientation_type, name);
            require(sample->values[0] == float(name.back() - '0'), name);
            require(sample->values[1] == 0 && sample->values[2] == 0, name);
        } else if (starts_with(name, "reject-orientation-")) {
            ++rejected_records;
            require(!host_may_send_orientation(bytes), name);
            require(!andarium::motion::decode(bytes).has_value(), name);
        } else if (starts_with(name, "control-")) {
            ++frames;
            const auto state = controls.find(name);
            require(state != controls.end(), name);
            const auto frame = andarium::motion::encode_control(
                state->second.active, state->second.period, state->second.orientation);
            require(Bytes(frame.begin(), frame.end()) == bytes, name);
        } else {
            // The guest never encodes a rejected control frame.
            require(starts_with(name, "reject-control-"), name);
            for (const bool a : {false, true}) {
                for (const bool g : {false, true}) {
                    for (const auto o : {std::optional<bool>(), std::optional<bool>(false), std::optional<bool>(true)}) {
                        const auto frame = andarium::motion::encode_control({a, g}, {10'000'000, 10'000'000}, o);
                        require(Bytes(frame.begin(), frame.end()) != bytes, name);
                    }
                }
            }
        }
    }
    require(records == 4 && rejected_records == 6 && frames == controls.size(), "the fixture holds every kind of frame");
}
} // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "usage: motion_protocol_tests <motion-orientation-golden-frames.txt>");
    check_fixture(read_fixture(argv[1]));

    // Independent wire fixture: +1, -2, +0.5 radians/second.
    std::array<uint8_t, 20> bytes{
        'A', 'N', 'S', '1', 4, 0, 0, 0,
        0, 0, 0x80, 0x3f, 0, 0, 0, 0xc0, 0, 0, 0, 0x3f};
    const auto sample = andarium::motion::decode(bytes);
    require(sample.has_value());
    require(sample->type == 4);
    require(sample->values == std::array<float, 3>{1, -2, 0.5f});
    require(!andarium::motion::decode(std::span(bytes).first(19)));
    bytes[0] = 0;
    require(!andarium::motion::decode(bytes));
    bytes[0] = 'A';
    bytes[4] = 99;
    require(!andarium::motion::decode(bytes));
    bytes[4] = 1;
    require(andarium::motion::decode(bytes).has_value());
    bytes[10] = 0x80;
    bytes[11] = 0x7f; // Infinity is never a physical measurement.
    require(!andarium::motion::decode(bytes));
    bytes[10] = 0xc0; // Neither is NaN.
    require(!andarium::motion::decode(bytes));

    // Independent control frame fixtures: the accelerometer alone at 20 ms
    // (the gyroscope's period is carried and not read), then both, at 66 667
    // us and 1 s.
    using andarium::motion::encode_control;
    require(encode_control({true, false}, {20'000'000, 10'000'000}) ==
            std::array<uint8_t, 16>{'A', 'N', 'C', '1', 1, 0, 0, 0,
                                    0x20, 0x4e, 0, 0, 0x10, 0x27, 0, 0});
    require(encode_control({true, true}, {66'667'000, 1'000'000'000}) ==
            std::array<uint8_t, 16>{'A', 'N', 'C', '1', 3, 0, 0, 0,
                                    0x6b, 0x04, 0x01, 0, 0x40, 0x42, 0x0f, 0});
    // Nothing in use, and periods the sensor framework would never ask for,
    // which the frame brings into 10 ms to 1 s.
    require(encode_control({false, false}, {0, 5'000'000'000}) ==
            std::array<uint8_t, 16>{'A', 'N', 'C', '1', 0, 0, 0, 0,
                                    0x10, 0x27, 0, 0, 0x40, 0x42, 0x0f, 0});
    require(encode_control({false, true}, {10'000'000, 10'000'000})[4] == 2);

    // An image with the orientation sensor says so in the magic and the third
    // bit, which the sensors of the older frame leave as they were; the
    // sensor's own period does not exist.
    const auto with_sensor = encode_control({true, false}, {20'000'000, 10'000'000}, false);
    require(with_sensor[2] == 'C' && with_sensor[3] == '2' && with_sensor[4] == 1);
    require(encode_control({true, true}, {20'000'000, 10'000'000}, true)[4] == 7);
    require(encode_control({false, false}, {20'000'000, 10'000'000}, true)[4] == 4);
    require(encode_control({false, false}, {20'000'000, 10'000'000})[3] == '1');

    // The orientation record: the first value alone, one of four whole
    // numbers, in the 20 bytes of every record. An independent fixture of
    // landscape 90 degrees counter-clockwise (1.0), then each way it is wrong.
    std::array<uint8_t, 20> turned{
        'A', 'N', 'S', '1', 27, 0, 0, 0, 0, 0, 0x80, 0x3f, 0, 0, 0, 0, 0, 0, 0, 0};
    const auto orientation = andarium::motion::decode(turned);
    require(orientation.has_value() && orientation->type == 27 && orientation->values[0] == 1.0f);
    for (const float value : {0.0f, 1.0f, 2.0f, 3.0f}) {
        const auto word = std::bit_cast<uint32_t>(value);
        for (size_t i = 0; i < 4; ++i) turned[8 + i] = uint8_t(word >> (8 * i));
        require(andarium::motion::decode(turned).has_value());
    }
    const auto refuse = [&turned](size_t offset, float value) {
        auto bytes = turned;
        const auto word = std::bit_cast<uint32_t>(value);
        for (size_t i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(word >> (8 * i));
        return !andarium::motion::decode(bytes).has_value();
    };
    require(refuse(8, 4.0f) && refuse(8, -1.0f) && refuse(8, 0.5f) && refuse(8, 1e9f));
    require(refuse(8, std::nanf("")) && refuse(8, INFINITY));
    require(refuse(12, 1.0f) && refuse(16, 1.0f));
    require(andarium::motion::orientation_count == 4);
    require(std::string(andarium::motion::orientation_property) == "ro.boot.kernarium.orientation");
}
