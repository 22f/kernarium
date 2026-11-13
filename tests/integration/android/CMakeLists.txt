# SPDX-License-Identifier: GPL-2.0-or-later

# The host's side of an Android guest's disks (android_disk_tool.cpp): the
# runner creates and checks the data disk with the library both launchers use.
add_executable(uml_android_disk_tool android_disk_tool.cpp)
target_link_libraries(uml_android_disk_tool PRIVATE UML::AndroidBoot)
target_compile_options(uml_android_disk_tool PRIVATE -Wall -Wextra -Werror)
target_compile_features(uml_android_disk_tool PRIVATE cxx_std_20)
add_dependencies(integration-test-binaries uml_android_disk_tool)

# A guest boots on the release's read-only system disk and a blank data disk
# the host created, then on the same data disk again.
uml_add_guest_integration_test(
    NAME guest-android-disks
    SCRIPT run_guest_android_disks.py
    ARGS --disk-tool $<TARGET_FILE:uml_android_disk_tool>
        --android-root ${PROJECT_SOURCE_DIR}/android/lineage-24
    LABELS guest;boot;android
    TIMEOUT 600
    CPUS 1)

# The release's adb debugging setting and adbd's key rule (the p0007 and p0011
# adb topics), on a fresh data disk and across reboots. It boots Android with
# the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_ADB_DEBUGGING_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-adb-debugging test boots")
if(UML_ANDROID_ADB_DEBUGGING_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-adb-debugging
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_adb_debugging.py
            --release ${UML_ANDROID_ADB_DEBUGGING_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# A fresh guest starts in Setup Wizard; the runner completes the wizard through
# its pages and checks that the harness helper (boot_android.py --provision)
# reaches the same end state. It boots Android twice with the macOS launcher, so
# it is opt-in: name the release under test.
set(UML_ANDROID_OOBE_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-oobe test boots")
if(UML_ANDROID_OOBE_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-oobe
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_oobe.py
            --release ${UML_ANDROID_OOBE_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# microG's restricted signature spoofing (route p0007): with nothing installed
# under the two microG names PackageManager reports neither, and test-key
# decoys with those names report their own signer, never Google's certificate.
# It boots Android with the macOS launcher, so it is opt-in: name the release
# under test and the directory that tests/integration/android/microg/build-apks.sh
# filled.
set(UML_ANDROID_MICROG_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-microg-spoofing test boots")
set(UML_ANDROID_MICROG_PROBE_DIR "" CACHE PATH
    "Directory with the three APKs of the microG probe (microg/build-apks.sh)")
if(UML_ANDROID_MICROG_RELEASE AND UML_ANDROID_MICROG_PROBE_DIR AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-microg-spoofing
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_microg_spoofing.py
            --release ${UML_ANDROID_MICROG_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            --probe-dir ${UML_ANDROID_MICROG_PROBE_DIR}
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The command that Andarium's bug report sends to the guest to read its /proc
# counters (AndariumCore's GuestProcFiles.swift, the one source of the string),
# run as the application's adb channel runs it, in an Android guest: every file
# arrives with content or says why it did not. It boots Android with the macOS
# launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_GUEST_PROC_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-guest-proc test boots")
if(UML_ANDROID_GUEST_PROC_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-guest-proc
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_guest_proc.py
            --release ${UML_ANDROID_GUEST_PROC_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 1800
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The host's hardware keyboard and mouse (the image's device configuration,
# the on-screen keyboard default and the pointer capture LED of the input
# topic): the runner plays the display's user over the launcher's VNC display
# with --hardware-input and reads Android's input dump and the probe app's log.
# It boots Android with the macOS launcher, so it is opt-in: name the release
# under test and the probe's APK (tests/integration/android/keymouse).
set(UML_ANDROID_KEYMOUSE_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-keymouse test boots")
set(UML_ANDROID_KEYMOUSE_PROBE_APK "" CACHE FILEPATH
    "AndariumKeyMouseProbe APK that the android-keymouse test installs")
if(UML_ANDROID_KEYMOUSE_RELEASE AND UML_ANDROID_KEYMOUSE_PROBE_APK AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-keymouse
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_keymouse.py
            --release ${UML_ANDROID_KEYMOUSE_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            --probe-apk ${UML_ANDROID_KEYMOUSE_PROBE_APK}
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The Virtio video devices across a guest suspend (the transport keeps a codec
# stream's queued buffers, docs/architecture.md): the gallery plays a clip and
# screenrecord films the screen while the guest suspends by the host's route and
# by its own with its screen off; the picture moves again, screenrecord ends
# cleanly, Back closes the player, and the host closes every stream by a destroy
# and none by a reset. It boots Android with the macOS launcher, so it is
# opt-in: name the release under test.
set(UML_ANDROID_VIDEO_SUSPEND_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-video-suspend test boots")
if(UML_ANDROID_VIDEO_SUSPEND_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-video-suspend
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_video_suspend.py
            --release ${UML_ANDROID_VIDEO_SUSPEND_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The guest's one Wi-Fi network (the Wi-Fi module's overlay and the boot script
# of android/lineage-24, and the validation of routes p0029 and p0033): with the
# internet checks failing, the network stays enabled and joined, validated, and
# shows only Android's Wi-Fi mark, also when a sign-in or block page answers the
# probe; it joins again after a suspend and after a restart; a joined network
# keeps its settings, a forgotten one stays forgotten and Wi-Fi that is off
# stays off. With previous releases named, the data disk of each is booted by the
# release under test: one that a release left with the network disabled is
# repaired once, and one that never disables it is left alone, and a failed check
# shows only the warning on both. It boots Android with the macOS launcher, so
# it is opt-in: name the release under test, and the previous releases (a
# ;-separated list) for the upgrade sections.
set(UML_ANDROID_WIFI_NETWORK_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-wifi-network test boots")
set(UML_ANDROID_WIFI_NETWORK_PREVIOUS_RELEASE "" CACHE STRING
    "Installed Android release directories (a ;-separated list) whose data disks the android-wifi-network test upgrades (empty: no upgrade section)")
if(UML_ANDROID_WIFI_NETWORK_RELEASE AND TARGET uml-darwin-android17)
    set(wifi_network_previous)
    foreach(previous_release IN LISTS UML_ANDROID_WIFI_NETWORK_PREVIOUS_RELEASE)
        list(APPEND wifi_network_previous --previous-release ${previous_release})
    endforeach()
    uml_add_integration_test(
        NAME android-wifi-network
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_wifi_network.py
            --release ${UML_ANDROID_WIFI_NETWORK_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            ${wifi_network_previous}
        LABELS guest;boot;android
        TIMEOUT 9000
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The host's cameras (routes p0030 and p0031 and the camera layouts): the runner
# boots the release with the launcher's synthetic camera and a camera layout on the
# vendor SKU, and checks the layout's features and media profiles, the cameras
# Android lists, one preview frame and one still of the camera application, and,
# in a blocked session, the same cameras with black frames and no still. It boots
# Android with the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_CAMERA_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-camera tests boot")
if(UML_ANDROID_CAMERA_RELEASE AND TARGET uml-darwin-android17)
    foreach(mode live blocked)
        if(mode STREQUAL "live")
            set(camera_test_name android-camera)
        else()
            set(camera_test_name android-camera-blocked)
        endif()
        uml_add_integration_test(
            NAME ${camera_test_name}
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/run_android_camera.py
                --release ${UML_ANDROID_CAMERA_RELEASE}
                --emulator $<TARGET_FILE:uml-darwin-android17>
                --mode ${mode}
            LABELS guest;boot;android
            TIMEOUT 3600
            PROCESSORS 4
            RESOURCE_GROUPS cpus:4)
    endforeach()
endif()

# The thermal HAL and the 30 fps cap (route p0007's display topic): the runner
# plays the host application over a vsock forward to the HAL's port and checks
# the thermal manager, the display manager's vote, SurfaceFlinger's render rate
# and the frame rates of the guest and the host at each severity. It boots
# Android with the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_THERMAL_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-thermal test boots")
if(UML_ANDROID_THERMAL_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-thermal
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_thermal.py
            --release ${UML_ANDROID_THERMAL_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The device-orientation sensor (android/lineage-24/overlay/sensors) and the
# display rotation it drives: the runner plays the host application over a vsock
# forward to the motion port, sends orientation records and reads Android's
# rotation and sensor service, with auto-rotate on and off, applications that
# ask for an orientation, the lock screen and the setup wizard. It boots Android
# with the macOS launcher, so it is opt-in: name the release under test.
set(UML_ANDROID_ORIENTATION_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-orientation test boots")
if(UML_ANDROID_ORIENTATION_RELEASE AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-orientation
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_orientation.py
            --release ${UML_ANDROID_ORIENTATION_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
        LABELS guest;boot;android
        TIMEOUT 5400
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()

# The gamepad relay, a vendor daemon of the image's overlay
# (android/lineage-24/overlay/gamepad): the runner plays the host application
# over a vsock forward to the relay's port, creates a controller of each driver
# family and checks what Android sees (devices, keys, axes), what comes back
# (rumble, lights, the drivers' feature requests), hot plug, the relay's
# recovery, and the guest's cost per report. It boots Android with the macOS
# launcher, so it is opt-in: name the release under test and the probe app's
# APK (tests/integration/android/gamepad/build-apk.sh).
set(UML_ANDROID_GAMEPAD_RELEASE "" CACHE PATH
    "Installed Android release directory that the android-gamepad test boots")
set(UML_ANDROID_GAMEPAD_PROBE_APK "" CACHE FILEPATH
    "AndariumGamepadProbe.apk that the android-gamepad test installs")
if(UML_ANDROID_GAMEPAD_RELEASE AND UML_ANDROID_GAMEPAD_PROBE_APK AND TARGET uml-darwin-android17)
    uml_add_integration_test(
        NAME android-gamepad
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_android_gamepad.py
            --release ${UML_ANDROID_GAMEPAD_RELEASE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            --probe-apk ${UML_ANDROID_GAMEPAD_PROBE_APK}
        LABELS guest;boot;android
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()
