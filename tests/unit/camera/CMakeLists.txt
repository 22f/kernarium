# SPDX-License-Identifier: GPL-2.0-or-later

# The camera wire is header-only here: its layout is checked at compile time and
# its values at run time, with no device behind it.
add_executable(uml_camera_protocol_tests camera_protocol_tests.cpp)
target_include_directories(uml_camera_protocol_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/kernel/common/video)
uml_add_unit_test(NAME camera-protocol COMMAND uml_camera_protocol_tests
    LABELS "device;camera")

# The picture helpers of darwin/platform (camera.hpp's neighbours).
add_executable(uml_camera_transform_tests camera_transform_tests.cpp)
target_link_libraries(uml_camera_transform_tests PRIVATE UML::Nv12Transform)
uml_add_unit_test(NAME camera-frame-transform COMMAND uml_camera_transform_tests
    LABELS "device;camera")

add_executable(uml_camera_dng_tests camera_dng_tests.cpp)
target_link_libraries(uml_camera_dng_tests PRIVATE UML::DngColor)
uml_add_unit_test(NAME camera-dng-color COMMAND uml_camera_dng_tests
    LABELS "device;camera")

# Discovery of the host's own cameras: no session, no permission request, no
# capture.
add_executable(uml_camera_apple_discovery_tests camera_apple_discovery_tests.cpp)
target_link_libraries(uml_camera_apple_discovery_tests PRIVATE UML::AppleCamera)
uml_add_unit_test(NAME camera-apple-discovery COMMAND uml_camera_apple_discovery_tests
    LABELS "device;camera")

# The synthetic camera is the platform camera that the other camera tests use.
if(TARGET UML::SyntheticCamera)
    add_executable(uml_camera_synthetic_tests camera_synthetic_tests.cpp)
    target_link_libraries(uml_camera_synthetic_tests PRIVATE UML::SyntheticCamera)
    uml_add_unit_test(NAME camera-synthetic COMMAND uml_camera_synthetic_tests
        LABELS "device;camera")
endif()

# The camera device's pure parts: each is a function of its inputs, checked
# without a device.
add_executable(uml_camera_jpeg_gap_tests camera_jpeg_gap_tests.cpp)
target_link_libraries(uml_camera_jpeg_gap_tests PRIVATE UML::CameraDevice)
uml_add_unit_test(NAME camera-jpeg-gap COMMAND uml_camera_jpeg_gap_tests
    LABELS "device;camera")

add_executable(uml_camera_calibration_tests camera_calibration_tests.cpp)
target_link_libraries(uml_camera_calibration_tests PRIVATE UML::CameraDevice)
uml_add_unit_test(NAME camera-calibration COMMAND uml_camera_calibration_tests
    LABELS "device;camera")

# The device's tests use the synthetic camera as the platform camera.
if(TARGET UML::SyntheticCamera)
    add_executable(uml_camera_controls_tests camera_controls_tests.cpp)
    target_link_libraries(uml_camera_controls_tests PRIVATE UML::CameraDevice UML::SyntheticCamera)
    uml_add_unit_test(NAME camera-controls COMMAND uml_camera_controls_tests
        LABELS "device;camera")

    add_executable(uml_camera_description_tests camera_description_tests.cpp)
    target_link_libraries(uml_camera_description_tests PRIVATE UML::CameraDevice UML::SyntheticCamera)
    uml_add_unit_test(NAME camera-description COMMAND uml_camera_description_tests
        LABELS "device;camera")
endif()
