# SPDX-License-Identifier: GPL-2.0-or-later

# AndariumCore is the Andarium application's model as a Swift package with no
# UIKit, so its tests run on the host. SwiftPM keeps its products in the build
# tree, not beside the package.
uml_add_unit_test(NAME andarium-core
    COMMAND /usr/bin/xcrun swift test
        --package-path ${PROJECT_SOURCE_DIR}/Andarium/AndariumCore
        --scratch-path ${CMAKE_BINARY_DIR}/andarium-core-swiftpm
    LABELS andarium
    TIMEOUT 1200)

add_executable(uml_andarium_motion_protocol_tests motion_protocol_tests.cpp)
target_include_directories(uml_andarium_motion_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/sensors)
uml_add_unit_test(NAME andarium-motion-protocol
    COMMAND uml_andarium_motion_protocol_tests LABELS andarium)

# The guest's vibrator HAL (android/lineage-24/overlay/vibrator): the wire format
# against the golden frames that AndariumCore checks too (the fixture's path is
# the test's argument, so the file has one copy), the timelines the framework's
# calls become, and the HAL's state and timing against a fake link.
add_executable(uml_andarium_haptics_protocol_tests haptics_protocol_tests.cpp)
target_include_directories(uml_andarium_haptics_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/vibrator)
uml_add_unit_test(NAME andarium-haptics-protocol
    COMMAND uml_andarium_haptics_protocol_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/haptics-golden-frames.txt
    LABELS andarium)

add_executable(uml_andarium_haptics_player_tests haptics_player_tests.cpp)
target_include_directories(uml_andarium_haptics_player_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/vibrator)
uml_add_unit_test(NAME andarium-haptics-player
    COMMAND uml_andarium_haptics_player_tests LABELS andarium)

# The guest's thermal HAL (android/lineage-24/overlay/thermal): the wire format
# against the golden frames that AndariumCore checks too (the fixture's path is
# the test's argument, so the file has one copy), and the status, temperature
# and thresholds that each severity becomes.
add_executable(uml_andarium_thermal_protocol_tests thermal_protocol_tests.cpp)
target_include_directories(uml_andarium_thermal_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/thermal)
uml_add_unit_test(NAME andarium-thermal-protocol
    COMMAND uml_andarium_thermal_protocol_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/thermal-golden-frames.txt
    LABELS andarium)

add_executable(uml_andarium_thermal_levels_tests thermal_levels_tests.cpp)
target_include_directories(uml_andarium_thermal_levels_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/thermal)
uml_add_unit_test(NAME andarium-thermal-levels
    COMMAND uml_andarium_thermal_levels_tests LABELS andarium)

# The guest's gamepad relay (android/lineage-24/overlay/gamepad): the wire format
# against the golden frames that AndariumCore checks too (the fixture's path is
# the test's argument, so the file has one copy), the ranges of every field and
# the reader that turns a byte stream into frames.
add_executable(uml_andarium_gamepad_protocol_tests gamepad_protocol_tests.cpp)
target_include_directories(uml_andarium_gamepad_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/gamepad)
uml_add_unit_test(NAME andarium-gamepad-protocol
    COMMAND uml_andarium_gamepad_protocol_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/gamepad-golden-frames.txt
    LABELS andarium)
