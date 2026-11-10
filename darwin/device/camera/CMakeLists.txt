# SPDX-License-Identifier: GPL-2.0-or-later

# The Virtio camera device (darwin/platform/camera.hpp on the host side, the
# video wire in kernel/common/video on the guest's). It builds on the video
# device's guest-buffer writer and links no capture backend: the launcher hands
# it the platform camera to serve.
add_library(uml_camera_device STATIC
    calibration_store.cpp
    camera_controls.cpp
    camera_description.cpp
    camera_device.cpp
    in_process_service.cpp
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
