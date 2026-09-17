# Sysbuild hook picked up as part of the Zephyr module (../zephyr/module.yml).
#
# Updates the CMake cache to add Kconfig fragments and devicetree overlays
# as desired for the serial modem build in this environment.

if(BOARD MATCHES "^circuitdojo_feather_nrf9151")
  # If 'value' is not in ${cache_var}, appends it and force-updates the cache.
  function(_cm_append cache_var value)
    sysbuild_cache_set(VAR ${cache_var} APPEND REMOVE_DUPLICATES "${value}")
  endfunction()

  # Sets per-domain (app & bootloader levels) overrides for cell_modm_firmware.
  # https://docs.zephyrproject.org/latest/build/sysbuild/index.html#sysbuild-cmake-namespace
  function(cell_modem_firmware_pre_cmake)
    set(here "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    _cm_append("${DEFAULT_IMAGE}_EXTRA_CONF_FILE" "${here}/app.conf")
    _cm_append("${DEFAULT_IMAGE}_EXTRA_DTC_OVERLAY_FILE" "${here}/app.overlay")
    _cm_append(b0_EXTRA_CONF_FILE "${here}/b0.conf")
    _cm_append(b0_EXTRA_DTC_OVERLAY_FILE "${here}/b0.overlay")
    _cm_append(mcuboot_EXTRA_CONF_FILE "${here}/mcuboot.conf")
    _cm_append(mcuboot_EXTRA_DTC_OVERLAY_FILE "${here}/mcuboot.overlay")
  endfunction()
endif()
