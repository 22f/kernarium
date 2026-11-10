# SPDX-License-Identifier: GPL-2.0-or-later

# The device is driven through its queues and its event descriptor with
# synthetic fixes; no guest is involved.
add_executable(uml_gnss_nmea_writer_tests nmea_writer_tests.cpp)
target_link_libraries(uml_gnss_nmea_writer_tests PRIVATE UML::GnssDevice)
uml_add_unit_test(NAME gnss-nmea-writer
    COMMAND uml_gnss_nmea_writer_tests
    LABELS "device;gnss")

add_executable(uml_gnss_synthetic_source_tests synthetic_source_tests.cpp)
target_link_libraries(uml_gnss_synthetic_source_tests
    PRIVATE UML::GnssSyntheticSource)
uml_add_unit_test(NAME gnss-synthetic-source
    COMMAND uml_gnss_synthetic_source_tests
    LABELS "device;gnss")

add_executable(uml_gnss_device_tests virtio_gnss_device_tests.cpp)
target_link_libraries(uml_gnss_device_tests PRIVATE UML::GnssDevice)
uml_add_unit_test(NAME virtio-gnss-device
    COMMAND uml_gnss_device_tests
    LABELS "device;gnss")
