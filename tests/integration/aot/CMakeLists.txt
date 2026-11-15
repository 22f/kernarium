# SPDX-License-Identifier: GPL-2.0-or-later

# The ahead-of-time component against a running Android guest: workloads of
# the covered library families and the guest probe, with the component off
# and on (run_aot_guest.py). It needs the framework (UML_AOT_IMAGE) and boots
# Android twice with the macOS launcher, so it exists only where both are.
if(TARGET uml_arm64_aot_framework AND TARGET uml-darwin-android17)
    get_property(_uml_aot_root GLOBAL PROPERTY UML_AOT_IMAGE_ROOT)
    set(_uml_aot_probe_args)

    # The probe is built for Android arm64 against the image's own libc.so and
    # libz.so, with no headers and no runtime of its own.
    set(_uml_aot_probe ${CMAKE_CURRENT_BINARY_DIR}/libaotprobe.so)
    add_custom_command(
        OUTPUT ${_uml_aot_probe}
        COMMAND ${CMAKE_CXX_COMPILER}
            --target=aarch64-linux-android30 -std=c++20 -O1 -fPIC
            -ffreestanding -fno-exceptions -fno-rtti -fno-builtin -nostdlib
            -nostdinc++ -fuse-ld=lld -shared -Wall -Wextra -Werror
            -Wl,-soname,libaotprobe.so -Wl,--no-undefined
            -Wl,-z,max-page-size=16384
            ${CMAKE_CURRENT_SOURCE_DIR}/aot_guest_probe.cpp
            -L${_uml_aot_root}/system/lib64/bootstrap
            -L${_uml_aot_root}/system/lib64 -lc -ldl -lz
            -o ${_uml_aot_probe}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/aot_guest_probe.cpp
        COMMENT "Building the AOT guest probe"
        VERBATIM)
    add_custom_target(uml-aot-guest-probe DEPENDS ${_uml_aot_probe})
    add_dependencies(integration-test-binaries uml-aot-guest-probe)

    set(_uml_aot_diagnostics_args)
    if(UML_AOT_DIAGNOSTICS)
        get_property(_uml_aot_manifest GLOBAL PROPERTY UML_AOT_MANIFEST)
        set(_uml_aot_diagnostics_args --diagnostics
            --manifest ${_uml_aot_manifest}
            # Libraries the workloads and the booted system run.
            --expect-entered libc.so,libz.so)
    endif()
    uml_add_integration_test(
        NAME aot-guest-workloads
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_aot_guest.py
            --release ${UML_AOT_IMAGE}
            --emulator $<TARGET_FILE:uml-darwin-android17>
            --probe ${_uml_aot_probe}
            ${_uml_aot_diagnostics_args}
        LABELS guest;boot;android;aot
        TIMEOUT 3600
        PROCESSORS 4
        RESOURCE_GROUPS cpus:4)
endif()
