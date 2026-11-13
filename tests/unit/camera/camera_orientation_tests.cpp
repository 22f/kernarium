// SPDX-License-Identifier: GPL-2.0-or-later
// How a camera's SENSOR_ORIENTATION and picture turn follow the turn of the
// device body where the host owns the guest's rotation (an iPad window): the
// table of the four interface orientations and the two facings, the phone case
// that leaves the platform's values, and the turn of the picture after a boot.

#include "camera_orientation.hpp"
#include "require.hpp"

#include <array>
#include <cstdint>

using namespace uml::camera;
using uml::platform::CameraPosition;
using uml::testing::require;

namespace {

/* The device body's clockwise turn from portrait of each interface
 * orientation: UIKit's landscapeLeft has the home button on the left, which is
 * the device turned a quarter clockwise. */
struct Interface {
    const char* name;
    std::uint32_t turn;
    std::uint32_t back;
    std::uint32_t front;
};

constexpr std::array<Interface, 4> interfaces {{
    {"portrait", 0, 90, 270},
    {"landscapeLeft", 90, 180, 180},
    {"portraitUpsideDown", 180, 270, 90},
    {"landscapeRight", 270, 0, 0},
}};

void testTheTableOfOrientations()
{
    // The platform's own values: 90 for a back camera, 270 for a front camera.
    for (const auto& one : interfaces) {
        require(turnedSensorOrientation(CameraPosition::back, 90, one.turn) == one.back, one.name);
        require(turnedSensorOrientation(CameraPosition::front, 270, one.turn) == one.front, one.name);
        // An external camera is not on the body.
        require(turnedSensorOrientation(CameraPosition::external, 90, one.turn) == 90, one.name);
    }
    // The sensor's native buffer is upright with the home button on the right
    // (landscapeRight): neither facing needs a turn there.
    require(turnedSensorOrientation(CameraPosition::back, 90, 270) == 0 &&
                turnedSensorOrientation(CameraPosition::front, 270, 270) == 0,
            "the native buffer needs no turn in landscapeRight");
}

void testAPhoneKeepsThePlatformsValues()
{
    // A guest that rotates itself states no turn.
    for (const std::uint32_t base : {0u, 90u, 180u, 270u}) {
        for (const auto position : {CameraPosition::back, CameraPosition::front, CameraPosition::external})
            require(turnedSensorOrientation(position, base, 0) == base, "turn 0 leaves the platform's value");
    }
}

void testThePictureTurnsByTheChangeSinceTheBoot()
{
    for (const auto& boot : interfaces) {
        for (const auto& now : interfaces) {
            const auto delta = (now.turn + 360 - boot.turn) % 360;
            // A back camera's picture turns with the body, a front camera's against it.
            require(pictureTurn(CameraPosition::back, boot.turn, now.turn) == delta, boot.name);
            require(pictureTurn(CameraPosition::front, boot.turn, now.turn) == (360 - delta) % 360, boot.name);
            require(pictureTurn(CameraPosition::external, boot.turn, now.turn) == 0, boot.name);
            // The boot's orientation and the turn together are the orientation now.
            require((turnedSensorOrientation(CameraPosition::back, 90, boot.turn) +
                     pictureTurn(CameraPosition::back, boot.turn, now.turn)) % 360 == now.back,
                    "back: boot orientation plus the change is the orientation now");
            require((turnedSensorOrientation(CameraPosition::front, 270, boot.turn) +
                     pictureTurn(CameraPosition::front, boot.turn, now.turn)) % 360 == now.front,
                    "front: boot orientation plus the change is the orientation now");
        }
    }
    require(pictureTurn(CameraPosition::back, 0, 0) == 0 && pictureTurn(CameraPosition::front, 270, 270) == 0,
            "no change, no turn");
}

} // namespace

int main()
{
    testTheTableOfOrientations();
    testAPhoneKeepsThePlatformsValues();
    testThePictureTurnsByTheChangeSinceTheBoot();
    return 0;
}
