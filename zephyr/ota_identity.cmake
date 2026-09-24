# PlatformIO may remove its entire build directory immediately before CMake.
# The pre-hook keeps the identity outside that directory and also refreshes the
# build copy on ordinary incremental invocations where CMake does not run.
if(NOT DEFINED OWNTECH_OTA_IDENTITY_DIR OR
   NOT EXISTS "${OWNTECH_OTA_IDENTITY_DIR}/owntech_build_info.h" OR
   NOT EXISTS "${OWNTECH_OTA_IDENTITY_DIR}/identity.json")
  message(FATAL_ERROR "OTA identity missing: build with the OTA or USB_LEAD PlatformIO environment")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ota_generated")
configure_file("${OWNTECH_OTA_IDENTITY_DIR}/owntech_build_info.h"
  "${CMAKE_CURRENT_BINARY_DIR}/ota_generated/owntech_build_info.h" COPYONLY)
configure_file("${OWNTECH_OTA_IDENTITY_DIR}/identity.json"
  "${CMAKE_CURRENT_BINARY_DIR}/ota_generated/identity.json" COPYONLY)
