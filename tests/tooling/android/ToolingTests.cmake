# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME lineage24-builder-lane
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_builder_lane.py
        --source-root ${PROJECT_SOURCE_DIR} --part verifiers
    LABELS android;audit
    TIMEOUT 180)

uml_add_tooling_test(
    NAME lineage24-target-files-fixtures
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_builder_lane.py
        --source-root ${PROJECT_SOURCE_DIR} --part target-files
    LABELS android;audit
    TIMEOUT 120)

uml_add_tooling_test(
    NAME lineage24-aosp-patch-regenerator
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_aosp_patch_regenerator.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-routed-workspaces
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_routed_workspaces.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 900)

uml_add_tooling_test(
    NAME lineage24-mesa-venus-source-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_mesa_venus_source_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 300)

uml_add_tooling_test(
    NAME lineage24-mesa-venus-port-generator
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_generate_mesa_venus_port.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-first-boot-precompile
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_precompile_first_boot_apps.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-sign-release
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_sign_release.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME lineage24-release-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_release_contract.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 30)

# The list of paths that decide a release names every producer, and the object
# IDs it yields do not depend on history.
uml_add_tooling_test(
    NAME lineage24-release-inputs
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_release_inputs.py
        --source-root ${PROJECT_SOURCE_DIR}
    LABELS android;audit
    TIMEOUT 60)

uml_add_tooling_test(
    NAME android17-macos-launcher-options
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_boot_android.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-instance-rules
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_android_instance.py
    LABELS android
    TIMEOUT 30)

# Each Android runner's default adb forward port is its guest slot's first
# port, and its own fixed port outside a slot.
uml_add_tooling_test(
    NAME android-runner-slot-ports
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_slot_port_defaults.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-adb-debugging-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_adb_debugging_runner.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-guest-proc-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_guest_proc_runner.py
    LABELS android
    TIMEOUT 60)

uml_add_tooling_test(
    NAME android17-provision-helper
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_android_provision.py
    LABELS android
    TIMEOUT 30)

uml_add_tooling_test(
    NAME android17-oobe-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_oobe_runner.py
    LABELS android
    TIMEOUT 30)

# The thermal runner's reading of the guest's dumps, and the records it sends.
uml_add_tooling_test(
    NAME android17-thermal-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_thermal_runner.py
    LABELS android
    TIMEOUT 30)

# The orientation runner without a guest: the wire against the golden frames,
# the parsers of the dumps it reads, the rotation each request earns, its host
# against a fake guest on loopback, and the probe application's manifest.
uml_add_tooling_test(
    NAME android17-orientation-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_orientation_runner.py
    LABELS android
    TIMEOUT 60)

# The Wi-Fi network runner's reading of the guest's status, listing, dumps and
# overlay list, and of the boot script's settle time; its upgrade checks against
# scripted answers.
uml_add_tooling_test(
    NAME android17-wifi-network-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_wifi_network_runner.py
    LABELS android
    TIMEOUT 60)

# The gamepad runner without a guest: the host's end of the wire against the
# golden frames and the C++ header, the controller families against the
# drivers' own report structures, the parsers of what the guest prints, the
# checks, and the reader thread against a fake relay on loopback.
uml_add_tooling_test(
    NAME android17-gamepad-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_gamepad_runner.py
    LABELS android
    TIMEOUT 60)

# The keyboard and mouse runner's reading of the guest's input dump and of the
# probe's log, its VNC client's wire, and its key table against the launcher's.
uml_add_tooling_test(
    NAME android17-keymouse-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_keymouse_runner.py
    LABELS android
    TIMEOUT 30)

# The camera runner's plan for every layout, its reading of the guest's dumps,
# screenshot and still, and its arguments against the launcher's option parser.
uml_add_tooling_test(
    NAME android17-camera-runner
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_camera_runner.py
    LABELS android
    TIMEOUT 60)

# The microG spoofing probe's sources agree with each other and with the
# certificate LineageOS compares with, and its runner reads `am instrument`.
uml_add_tooling_test(
    NAME android17-microg-probe
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_microg_probe.py
    LABELS android
    TIMEOUT 60)

# The camera layouts: the closed set of names the host composes, their fallback
# order, and the permission, media profiles and init files the generator renders
# from them (overlay/camera/generated holds exactly its output).
uml_add_tooling_test(
    NAME android17-camera-layouts
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/android/test_camera_layouts.py
    LABELS android
    TIMEOUT 30)
