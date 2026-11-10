# SPDX-License-Identifier: GPL-2.0-or-later

# The Virtio camera device's pure parts (darwin/platform/camera.hpp on the host
# side, the video wire in kernel/common/video on the guest's): the tiers, the
# calibration store, the Exif gap, the controls and the camera description. The
# device that serves them links no capture backend; the launcher hands it the
# platform camera.
add_library(uml_camera_device STATIC
    calibration_store.cpp
    camera_controls.cpp
    camera_description.cpp
    jpeg_gap.cpp)
add_library(UML::CameraDevice ALIAS uml_camera_device)
uml_declare_architecture_target(uml_camera_device device)
target_include_directories(uml_camera_device PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/kernel/common/video
    ${PROJECT_SOURCE_DIR}/darwin/device/vhost_user
    ${PROJECT_SOURCE_DIR}/darwin/device/video
    ${PROJECT_SOURCE_DIR}/darwin/platform)
target_link_libraries(uml_camera_device PUBLIC UML::VideoGuestBuffers UML::Nv12Transform)
target_link_libraries(uml_camera_device PRIVATE UML::VhostUserTransport)
