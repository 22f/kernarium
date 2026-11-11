# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(uml_virtio_input_tests virtio_input_tests.cpp)
target_link_libraries(uml_virtio_input_tests PRIVATE UML::VirtioInput)
uml_add_unit_test(NAME virtio-input-protocol COMMAND uml_virtio_input_tests
    LABELS input)

# The HID usage table, against rows of the kernel's.
add_executable(uml_virtio_input_hid_keyboard_tests hid_keyboard_tests.cpp)
target_link_libraries(uml_virtio_input_hid_keyboard_tests PRIVATE UML::VirtioInput)
uml_add_unit_test(NAME virtio-input-hid-keyboard
    COMMAND uml_virtio_input_hid_keyboard_tests
    LABELS input)

# The host's side of the hardware keyboard, mouse and pointer: the reports it
# builds, the pointer's mapping, and the three devices together.
add_executable(uml_virtio_input_hardware_tests hardware_tests.cpp)
target_link_libraries(uml_virtio_input_hardware_tests PRIVATE UML::VirtioInput)
uml_add_unit_test(NAME virtio-input-hardware
    COMMAND uml_virtio_input_hardware_tests
    LABELS input)

# What the macOS launcher's --hardware-input does with a display's keys and
# pointer: the plug policy, the buttons, the wheels and the unplug.
add_executable(uml_virtio_input_bridge_tests hardware_bridge_tests.cpp)
target_link_libraries(uml_virtio_input_bridge_tests PRIVATE
    UML::VirtioInput UML::VncInput)
target_include_directories(uml_virtio_input_bridge_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/emulator/src
    ${PROJECT_SOURCE_DIR}/darwin/device)
uml_add_unit_test(NAME virtio-input-bridge
    COMMAND uml_virtio_input_bridge_tests
    LABELS input)
