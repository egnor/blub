# Sysbuild hook for this Zephyr module (see ../zephyr/module.yml).
#
# Attaches the Feather-specific Kconfig fragments and devicetree overlays in
# ../feather_nrf9151/ to the serial modem app, MCUboot and B0 images. This is
# the out-of-tree equivalent of the per-board files ncs-serial-modem keeps
# under app/boards/ and app/sysbuild/*/boards/ for Nordic's own boards.

if(NOT BOARD MATCHES "^circuitdojo_feather_nrf9151")
  return()
endif()

# Append 'file' to the <image>_<var> cache entry, which sysbuild forwards to
# that image's CMake as plain <var> (EXTRA_CONF_FILE / EXTRA_DTC_OVERLAY_FILE)
function(cell_modem_firmware_add_image_file image var file)
  set(cache_var ${image}_${var})
  set(files ${${cache_var}})
  if(NOT "${file}" IN_LIST files)
    list(APPEND files ${file})
    set(${cache_var} "${files}" CACHE STRING "${var} for ${image}" FORCE)
  endif()
endfunction()

# Called by sysbuild just before each image is configured (after all modules,
# including sdk-nrf's bootloader logic, have added their images and overlays)
function(cell_modem_firmware_pre_image_cmake)
  cmake_parse_arguments(ARG "" "IMAGE" "IMAGES" ${ARGN})
  if(ARG_IMAGE STREQUAL DEFAULT_IMAGE)
    set(stem app)
  elseif(ARG_IMAGE STREQUAL mcuboot)
    set(stem mcuboot)
  elseif(ARG_IMAGE STREQUAL b0)
    set(stem b0)
  else()
    # (mcuboot_s1_variant derives its config and devicetree from mcuboot's
    # build output, so it needs nothing extra here)
    return()
  endif()

  # (this runs in another scope, so don't rely on variables set above)
  cmake_path(SET feather_dir NORMALIZE
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../feather_nrf9151")
  message(STATUS "cell_modem_firmware: ${ARG_IMAGE} gets ${feather_dir}/${stem}.{conf,overlay}")
  cell_modem_firmware_add_image_file(${ARG_IMAGE} EXTRA_CONF_FILE
    ${feather_dir}/${stem}.conf)
  cell_modem_firmware_add_image_file(${ARG_IMAGE} EXTRA_DTC_OVERLAY_FILE
    ${feather_dir}/${stem}.overlay)
endfunction()
