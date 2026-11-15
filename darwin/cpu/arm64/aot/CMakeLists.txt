# SPDX-License-Identifier: GPL-2.0-or-later

# Ahead-of-time native code for the Android image's own arm64 libraries
# (docs/architecture.md, "Ahead-of-time code"). The framework is generated
# from the accepted release named by UML_AOT_IMAGE and is bound to it; it is
# never committed. A build without the image has no AOT component at all.

set(_uml_aot_dir ${CMAKE_CURRENT_BINARY_DIR})
set(_uml_aot_tools ${PROJECT_SOURCE_DIR}/tools/aot)
set(_uml_aot_image_dir ${_uml_aot_dir}/image)
set(_uml_aot_blob ${_uml_aot_dir}/blob.bin)
set(_uml_aot_manifest ${_uml_aot_dir}/manifest.json)
set(_uml_aot_fw_parent "${PROJECT_BINARY_DIR}/lib")
set(_uml_aot_fw_dir "${_uml_aot_fw_parent}/UmlArm64Aot.framework")

if(NOT IS_DIRECTORY "${UML_AOT_IMAGE}" OR
   NOT EXISTS "${UML_AOT_IMAGE}/release-manifest.json")
    message(FATAL_ERROR
        "UML_AOT_IMAGE must name an accepted Android release directory")
endif()
if(NOT UML_AOT_FSCK_EROFS OR NOT EXISTS "${UML_AOT_FSCK_EROFS}")
    message(FATAL_ERROR
        "UML_AOT_FSCK_EROFS must name an fsck.erofs that decodes Zstandard")
endif()
if(NOT UML_AOT_LIBRARIES)
    message(FATAL_ERROR "UML_AOT_LIBRARIES is empty")
endif()

file(READ "${UML_AOT_IMAGE}/release-manifest.json" _uml_aot_release_json)
string(JSON _uml_aot_release_id GET "${_uml_aot_release_json}" release_id)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${UML_AOT_IMAGE}/release-manifest.json")

file(MAKE_DIRECTORY "${_uml_aot_fw_dir}")
file(WRITE "${_uml_aot_fw_dir}/Info.plist"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
	<key>CFBundleExecutable</key>
	<string>UmlArm64Aot</string>
	<key>CFBundleIdentifier</key>
	<string>org.uml-darwin.arm64-aot</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>UmlArm64Aot</string>
	<key>CFBundlePackageType</key>
	<string>FMWK</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0</string>
	<key>CFBundleVersion</key>
	<string>1</string>
</dict>
</plist>
")

set(_uml_aot_library_args)
foreach(_library IN LISTS UML_AOT_LIBRARIES)
    list(APPEND _uml_aot_library_args --library "${_library}")
endforeach()

set(_uml_aot_tool_sources
    ${_uml_aot_tools}/a64decode.py
    ${_uml_aot_tools}/asm.py
    ${_uml_aot_tools}/elf64.py
    ${_uml_aot_tools}/rewrite.py)

# Unpack the release's system partition (a Zstandard EROFS image) once.
add_custom_command(
    OUTPUT ${_uml_aot_image_dir}/identity.json
    COMMAND ${Python3_EXECUTABLE} -B ${_uml_aot_tools}/extract_image.py
        --release "${UML_AOT_IMAGE}"
        --fsck-erofs "${UML_AOT_FSCK_EROFS}"
        --out ${_uml_aot_image_dir}
    DEPENDS ${_uml_aot_tools}/extract_image.py
        "${UML_AOT_IMAGE}/release-manifest.json"
        "${UML_AOT_IMAGE}/guest/partition-layout.json"
    COMMENT "Unpacking the accepted release for the AOT framework"
    VERBATIM)

# Rewrite the libraries into one position-independent blob.
add_custom_command(
    OUTPUT ${_uml_aot_blob} ${_uml_aot_manifest}
        ${_uml_aot_dir}/functions.txt
    COMMAND ${Python3_EXECUTABLE} -B ${_uml_aot_tools}/rewrite.py
        --root ${_uml_aot_image_dir}/root
        ${_uml_aot_library_args}
        --release-id "${_uml_aot_release_id}"
        --out ${_uml_aot_dir}
    DEPENDS ${_uml_aot_image_dir}/identity.json ${_uml_aot_tool_sources}
        ${PROJECT_SOURCE_DIR}/tests/unit/arm64/a64_instruction_decode_partitions.tsv
        ${PROJECT_SOURCE_DIR}/darwin/cpu/arm64/layout/assembly_offsets.h
    COMMENT "Rewriting the AOT libraries"
    VERBATIM)

add_library(uml_arm64_aot_framework SHARED
    aot_blob.S
    ${_uml_aot_blob}
    ${_uml_aot_dir}/functions.txt)
uml_declare_architecture_target(uml_arm64_aot_framework cpu gadget)
set_source_files_properties(aot_blob.S PROPERTIES
    COMPILE_DEFINITIONS "AOT_BLOB=\"${_uml_aot_blob}\""
    OBJECT_DEPENDS ${_uml_aot_blob})
set_source_files_properties(${_uml_aot_blob} ${_uml_aot_dir}/functions.txt
    PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
set_target_properties(uml_arm64_aot_framework PROPERTIES
    LINKER_LANGUAGE C
    PREFIX ""
    SUFFIX ""
    OUTPUT_NAME "UmlArm64Aot"
    LIBRARY_OUTPUT_DIRECTORY "${_uml_aot_fw_dir}"
    BUILD_WITH_INSTALL_NAME_DIR TRUE
    INSTALL_NAME_DIR "@rpath/UmlArm64Aot.framework")
add_custom_command(TARGET uml_arm64_aot_framework POST_BUILD
    COMMAND /usr/bin/codesign --force --sign -
        "${_uml_aot_fw_dir}"
    COMMENT "Code-signing UmlArm64Aot.framework"
    VERBATIM)

set_property(GLOBAL PROPERTY UML_AOT_FRAMEWORK_DIR "${_uml_aot_fw_dir}")
set_property(GLOBAL PROPERTY UML_AOT_FRAMEWORK_TARGET
    "uml_arm64_aot_framework")
set_property(GLOBAL PROPERTY UML_AOT_MANIFEST "${_uml_aot_manifest}")
set_property(GLOBAL PROPERTY UML_AOT_IMAGE_ROOT "${_uml_aot_image_dir}/root")
set_property(GLOBAL PROPERTY UML_AOT_FUNCTIONS "${_uml_aot_dir}/functions.txt")

# The part the gadget executor links: the binder and the entry call.
add_library(uml_arm64_aot_runtime OBJECT aot_runtime.cpp)
uml_declare_architecture_target(uml_arm64_aot_runtime cpu gadget)
target_link_libraries(uml_arm64_aot_runtime
    PUBLIC uml_arm64_aot_framework
    PRIVATE UML::BridgeAbi UML::CpuContract UML::MemoryContract)
target_include_directories(uml_arm64_aot_runtime PRIVATE
    ${PROJECT_SOURCE_DIR}/darwin)
