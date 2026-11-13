# SPDX-License-Identifier: GPL-2.0-or-later

# Kernarium's runtime is an iOS framework, but its console input, the file
# handling of the automation boot payload, the key wake policy and the
# automation launch's ADB authorization decision are plain POSIX and C++ and
# are exercised here on the host.
add_executable(uml_kernarium_console_input_tests console_input_tests.cpp)
target_include_directories(uml_kernarium_console_input_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-console-input
    COMMAND uml_kernarium_console_input_tests
    LABELS kernarium)

add_executable(uml_kernarium_console_output_tests console_output_tests.cpp)
target_include_directories(uml_kernarium_console_output_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-console-output
    COMMAND uml_kernarium_console_output_tests
    LABELS kernarium)

add_executable(uml_kernarium_payload_file_tests payload_file_tests.cpp)
target_include_directories(uml_kernarium_payload_file_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-payload-file
    COMMAND uml_kernarium_payload_file_tests
    LABELS kernarium)

# Which key events wake a guest that suspended itself (a touch never does).
add_executable(uml_kernarium_key_wake_tests key_wake_tests.cpp)
target_include_directories(uml_kernarium_key_wake_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-key-wake
    COMMAND uml_kernarium_key_wake_tests
    LABELS kernarium)

# The run loop of a launch that boots a restarting guest again, with a
# recording machine in place of the guest.
add_executable(uml_kernarium_restart_loop_tests restart_loop_tests.cpp)
target_include_directories(uml_kernarium_restart_loop_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime
    ${PROJECT_SOURCE_DIR}/darwin/api/include
    ${PROJECT_SOURCE_DIR}/darwin/emulator/src)
uml_add_unit_test(NAME kernarium-restart-loop
    COMMAND uml_kernarium_restart_loop_tests
    LABELS kernarium)

# How an automation launch asks to start the background job with its guest.
add_executable(uml_kernarium_automation_job_tests automation_job_tests.cpp)
target_include_directories(uml_kernarium_automation_job_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-automation-job
    COMMAND uml_kernarium_automation_job_tests
    LABELS kernarium)

add_executable(uml_kernarium_adb_authorization_tests
    adb_authorization_tests.cpp)
target_include_directories(uml_kernarium_adb_authorization_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-adb-authorization
    COMMAND uml_kernarium_adb_authorization_tests
    LABELS kernarium)

# The kernel arguments that carry an Android guest's adb key and the request
# that adbd not listen on TCP, and the limit on the whole kernel command line.
add_executable(uml_kernarium_adb_channel_tests adb_channel_tests.cpp)
target_include_directories(uml_kernarium_adb_channel_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-adb-channel
    COMMAND uml_kernarium_adb_channel_tests
    LABELS kernarium)

# The host's time zone an Android guest is given, and which names the runtime
# lets onto the kernel command line.
add_executable(uml_kernarium_guest_time_zone_tests guest_time_zone_tests.cpp)
target_include_directories(uml_kernarium_guest_time_zone_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime)
uml_add_unit_test(NAME kernarium-guest-time-zone
    COMMAND uml_kernarium_guest_time_zone_tests
    LABELS kernarium)

# The features of the host an Android guest is told at boot: the table of bits
# and the kernel arguments they become, and the launcher prefixes it adds. The
# orientation row is checked against the property the guest's motion sub-HAL
# reads.
add_executable(uml_kernarium_host_features_tests host_features_tests.cpp)
target_include_directories(uml_kernarium_host_features_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime
    ${PROJECT_SOURCE_DIR}/android/lineage-24/overlay/sensors)
target_link_libraries(uml_kernarium_host_features_tests PRIVATE
    UML::AndroidBoot)
uml_add_unit_test(NAME kernarium-host-features
    COMMAND uml_kernarium_host_features_tests
    LABELS kernarium)

# How a statistics reading is composed from the emulator's and the display's
# figures, and when the guest's memory may be asked for.
add_executable(uml_kernarium_statistics_tests statistics_tests.cpp)
target_include_directories(uml_kernarium_statistics_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime
    ${PROJECT_SOURCE_DIR}/darwin/api/include)
uml_add_unit_test(NAME kernarium-statistics
    COMMAND uml_kernarium_statistics_tests
    LABELS kernarium)

# How the application's location fix becomes the GNSS device's.
add_executable(uml_kernarium_location_fix_tests location_fix_tests.cpp)
target_include_directories(uml_kernarium_location_fix_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime
    ${PROJECT_SOURCE_DIR}/darwin/device)
uml_add_unit_test(NAME kernarium-location-fix
    COMMAND uml_kernarium_location_fix_tests
    LABELS kernarium)

# How the host's response to the system's memory pressure reaches the
# application: the runtime's words for a report and the one receiver of them.
add_executable(uml_kernarium_memory_pressure_tests memory_pressure_tests.cpp)
target_include_directories(uml_kernarium_memory_pressure_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/Kernarium/Runtime
    ${PROJECT_SOURCE_DIR}/darwin)
uml_add_unit_test(NAME kernarium-memory-pressure
    COMMAND uml_kernarium_memory_pressure_tests
    LABELS kernarium)

# What an Android guest is told about the host's cameras: the camera fields of
# the boot request, the layout composed from the host's cameras, the lens cover,
# and the camera state and rotation the application and the device exchange.
if(TARGET UML::SyntheticCamera)
    add_executable(uml_kernarium_camera_tests camera_tests.cpp)
    target_include_directories(uml_kernarium_camera_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/Kernarium/Runtime
        ${PROJECT_SOURCE_DIR}/darwin/device)
    target_link_libraries(uml_kernarium_camera_tests PRIVATE
        UML::CameraSource)
    uml_add_unit_test(NAME kernarium-camera
        COMMAND uml_kernarium_camera_tests
        LABELS "kernarium;camera")
endif()
