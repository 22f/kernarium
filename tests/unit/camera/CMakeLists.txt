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

# What a still reports of its exposure, from a literal photo-metadata dictionary
# (no camera is opened).
add_executable(uml_camera_still_exposure_tests camera_still_exposure_tests.cpp)
target_link_libraries(uml_camera_still_exposure_tests PRIVATE UML::AppleCamera
    "-framework CoreFoundation" "-framework CoreGraphics" "-framework ImageIO")
target_include_directories(uml_camera_still_exposure_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin/platform/apple)
uml_add_unit_test(NAME camera-still-exposure COMMAND uml_camera_still_exposure_tests
    LABELS "device;camera")

# The Apple host's accelerated transform against the portable one.
add_executable(uml_camera_apple_transform_tests camera_apple_transform_tests.cpp)
target_link_libraries(uml_camera_apple_transform_tests PRIVATE UML::AppleCamera)
uml_add_unit_test(NAME camera-apple-transform COMMAND uml_camera_apple_transform_tests
    LABELS "device;camera")

# The synthetic camera is the platform camera that the other camera tests use.
if(TARGET UML::SyntheticCamera)
    add_executable(uml_camera_synthetic_tests camera_synthetic_tests.cpp)
    target_link_libraries(uml_camera_synthetic_tests PRIVATE UML::SyntheticCamera)
    uml_add_unit_test(NAME camera-synthetic COMMAND uml_camera_synthetic_tests
        LABELS "device;camera")
endif()

# The launcher's way to the platform cameras (UML::CameraSource), without a
# session or a permission request.
add_executable(uml_camera_source_tests camera_source_tests.cpp)
target_link_libraries(uml_camera_source_tests PRIVATE UML::CameraSource)
if(TARGET UML::SyntheticCamera)
    target_compile_definitions(uml_camera_source_tests PRIVATE
        UML_TEST_EXPECTS_SYNTHETIC=1)
else()
    target_compile_definitions(uml_camera_source_tests PRIVATE
        UML_TEST_EXPECTS_SYNTHETIC=0)
endif()
uml_add_unit_test(NAME camera-source COMMAND uml_camera_source_tests
    LABELS "device;camera")

# The camera device's pure parts: each is a function of its inputs, checked
# without a device.
add_executable(uml_camera_jpeg_gap_tests camera_jpeg_gap_tests.cpp)
target_link_libraries(uml_camera_jpeg_gap_tests PRIVATE UML::CameraDevice)
uml_add_unit_test(NAME camera-jpeg-gap COMMAND uml_camera_jpeg_gap_tests
    LABELS "device;camera")

# How the guest's camera orientation follows the device where the host owns the
# rotation: pure arithmetic on the platform's camera description.
add_executable(uml_camera_orientation_tests camera_orientation_tests.cpp)
target_link_libraries(uml_camera_orientation_tests PRIVATE UML::CameraDevice)
uml_add_unit_test(NAME camera-orientation COMMAND uml_camera_orientation_tests
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

    # The device behind the real transport across a guest's suspend. The front
    # end is the GPU tests', so their support header and protocol header are on
    # the path.
    add_executable(uml_camera_held_requests_tests camera_held_requests_tests.cpp)
    target_include_directories(uml_camera_held_requests_tests BEFORE PRIVATE
        ${PROJECT_SOURCE_DIR}/tests/unit/gpu
        ${PROJECT_SOURCE_DIR}/darwin/device/gpu)
    target_link_libraries(uml_camera_held_requests_tests PRIVATE
        UML::CameraDevice UML::SyntheticCamera UML::VhostUserTransport)
    uml_add_unit_test(NAME camera-held-requests COMMAND uml_camera_held_requests_tests
        LABELS "device;camera;vhost-user" TIMEOUT 120)

    add_executable(uml_camera_device_tests camera_device_tests.cpp)
    target_link_libraries(uml_camera_device_tests PRIVATE UML::CameraDevice UML::SyntheticCamera)
    uml_add_unit_test(NAME camera-device COMMAND uml_camera_device_tests
        LABELS "device;camera" TIMEOUT 120)
endif()

# The layouts a host composes for the Android image: the closed set and its fallback order against the image's own
# table (the generator's output, which the test takes as its argument), and the concurrent feature against the HAL's
# own rule (its headers compile without Android).
if(TARGET UML::SyntheticCamera)
    add_executable(uml_camera_layout_tests camera_layout_tests.cpp)
    target_include_directories(uml_camera_layout_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/hal
        ${PROJECT_SOURCE_DIR}/kernel/common/video)
    target_link_libraries(uml_camera_layout_tests PRIVATE UML::CameraDevice UML::SyntheticCamera)
    uml_add_unit_test(NAME camera-layout
        COMMAND uml_camera_layout_tests
            ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/camera/generated/layouts.tsv
        LABELS "device;camera")
endif()
