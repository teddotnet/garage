# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/bjorn/esp/esp-idf/components/bootloader/subproject"
  "/home/bjorn/projects/p4_mqtt_cam/build/bootloader"
  "/home/bjorn/projects/p4_mqtt_cam/build/bootloader-prefix"
  "/home/bjorn/projects/p4_mqtt_cam/build/bootloader-prefix/tmp"
  "/home/bjorn/projects/p4_mqtt_cam/build/bootloader-prefix/src/bootloader-stamp"
  "/home/bjorn/projects/p4_mqtt_cam/build/bootloader-prefix/src"
  "/home/bjorn/projects/p4_mqtt_cam/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/bjorn/projects/p4_mqtt_cam/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/bjorn/projects/p4_mqtt_cam/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
