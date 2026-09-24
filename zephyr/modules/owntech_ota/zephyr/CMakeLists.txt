if(CONFIG_OWNTECH_OTA)
  if(CONFIG_THINGSET_DFU)
    message(FATAL_ERROR "OTA owns the secondary slot; disable legacy THINGSET_DFU")
  endif()
  if(CONFIG_THINGSET_STORAGE_FLASH OR CONFIG_SETTINGS_NVS)
    message(FATAL_ERROR "OTA shares Core's NVS owner; another NVS backend must not mount storage_partition")
  endif()
  zephyr_include_directories(public_api)
  zephyr_library()
  zephyr_library_sources(src/ota_protocol.c src/ota_participant.cpp
    src/ota_coordinator.cpp src/ota_safety.cpp src/ota_storage.cpp
    src/ota_runtime.cpp src/ota_thingset.cpp src/ota_feedback.cpp)
  zephyr_library_sources_ifdef(CONFIG_OWNTECH_OTA_LEAD src/ota_usb.cpp)
endif()
