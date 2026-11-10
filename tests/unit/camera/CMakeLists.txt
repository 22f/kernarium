# SPDX-License-Identifier: GPL-2.0-or-later

# The camera wire is header-only here: its layout is checked at compile time and
# its values at run time, with no device behind it.
add_executable(uml_camera_protocol_tests camera_protocol_tests.cpp)
target_include_directories(uml_camera_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/kernel/common/video)
uml_add_unit_test(NAME camera-protocol COMMAND uml_camera_protocol_tests
    LABELS "device;camera")
