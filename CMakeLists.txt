cmake_minimum_required(VERSION 3.22)

include(FetchContent)
get_filename_component(prj_name ${CMAKE_CURRENT_SOURCE_DIR} NAME)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(pico_sdk_import)

project(
    ${prj_name}
    VERSION 0.1.0
    LANGUAGES C ASM CXX
)

option(
    THERMAL_OTA_BOOT_PROBE
    "Erase and verify only the safe OTA header sector once during boot"
    OFF
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=ROOT)


set(DEFAULT_C_FLAGS
    -Wall
    -Wextra
    -Werror
    -Wbuiltin-macro-redefined
)

# U6 is ZD25WQ16C (16 Mbit / 2 MiB). Set this before pico_sdk_init() so the
# generated linker FLASH region also uses the physical device size.
set(PICO_FLASH_SIZE_BYTES 2097152)
pico_sdk_init()

# Pico SDK 2.2.0 carries TinyUSB 0.18.0.  Backport only the CDC-NCM host
# negotiation fix from TinyUSB PR #3630 into a build-local source overlay.
# This keeps the SDK checkout immutable and makes the workaround reproducible.
set(THERMAL_TINYUSB_NCM_OVERLAY
    ${CMAKE_CURRENT_BINARY_DIR}/tinyusb-ios26
)
file(RELATIVE_PATH
    THERMAL_TINYUSB_NCM_OVERLAY_RELATIVE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${THERMAL_TINYUSB_NCM_OVERLAY}
)
if(THERMAL_TINYUSB_NCM_OVERLAY_RELATIVE MATCHES "^\\.\\.")
    message(FATAL_ERROR
        "TinyUSB overlay must stay inside the pico_tn160 workspace"
    )
endif()
set(THERMAL_TINYUSB_NCM_PATCH
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tinyusb-0.18-ncm-ios26.patch
)
set(THERMAL_TINYUSB_DHCP_PATCH
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tinyusb-0.18-dhserver-stale-lease-nak.patch
)
set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${THERMAL_TINYUSB_NCM_PATCH}
    ${THERMAL_TINYUSB_DHCP_PATCH}
)
file(MAKE_DIRECTORY ${THERMAL_TINYUSB_NCM_OVERLAY}/src/class/net)
file(MAKE_DIRECTORY ${THERMAL_TINYUSB_NCM_OVERLAY}/lib/networking)
foreach(TINYUSB_NCM_FILE ncm_device.c ncm.h net_device.h)
    configure_file(
        ${PICO_TINYUSB_PATH}/src/class/net/${TINYUSB_NCM_FILE}
        ${THERMAL_TINYUSB_NCM_OVERLAY}/src/class/net/${TINYUSB_NCM_FILE}
        COPYONLY
    )
    # The SDK checkout can be mounted with executable permission bits.  Keep
    # the build-local copy a normal source file so git apply accepts the
    # patch's 100644 headers on every host filesystem.
    file(CHMOD
        ${THERMAL_TINYUSB_NCM_OVERLAY}/src/class/net/${TINYUSB_NCM_FILE}
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    )
endforeach()
configure_file(
    ${PICO_TINYUSB_PATH}/lib/networking/dhserver.c
    ${THERMAL_TINYUSB_NCM_OVERLAY}/lib/networking/dhserver.c
    COPYONLY
)
file(CHMOD
    ${THERMAL_TINYUSB_NCM_OVERLAY}/lib/networking/dhserver.c
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)
find_package(Git REQUIRED)
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn
            --directory=${THERMAL_TINYUSB_NCM_OVERLAY_RELATIVE}
            ${THERMAL_TINYUSB_NCM_PATCH}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE THERMAL_TINYUSB_PATCH_RESULT
    ERROR_VARIABLE THERMAL_TINYUSB_PATCH_ERROR
)
if(NOT THERMAL_TINYUSB_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Failed to backport TinyUSB PR #3630: ${THERMAL_TINYUSB_PATCH_ERROR}"
    )
endif()
execute_process(
    COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn
            --directory=${THERMAL_TINYUSB_NCM_OVERLAY_RELATIVE}
            ${THERMAL_TINYUSB_DHCP_PATCH}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE THERMAL_TINYUSB_DHCP_PATCH_RESULT
    ERROR_VARIABLE THERMAL_TINYUSB_DHCP_PATCH_ERROR
)
if(NOT THERMAL_TINYUSB_DHCP_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Failed to patch TinyUSB DHCP stale leases: "
        "${THERMAL_TINYUSB_DHCP_PATCH_ERROR}"
    )
endif()

# Do not silently reuse a stale, unpatched overlay after a workspace/cache
# migration.  The Release target below compiles these generated copies, so a
# successful configure must prove that both required backports are present.
file(READ
    ${THERMAL_TINYUSB_NCM_OVERLAY}/src/class/net/ncm_device.c
    THERMAL_TINYUSB_NCM_OVERLAY_SOURCE
)
string(FIND
    "${THERMAL_TINYUSB_NCM_OVERLAY_SOURCE}"
    "NCM_SET_NTB_INPUT_SIZE"
    THERMAL_TINYUSB_NCM_MARKER_OFFSET
)
if(THERMAL_TINYUSB_NCM_MARKER_OFFSET LESS 0)
    message(FATAL_ERROR
        "TinyUSB NCM overlay is stale or unpatched"
    )
endif()
file(READ
    ${THERMAL_TINYUSB_NCM_OVERLAY}/lib/networking/dhserver.c
    THERMAL_TINYUSB_DHCP_OVERLAY_SOURCE
)
string(FIND
    "${THERMAL_TINYUSB_DHCP_OVERLAY_SOURCE}"
    "requested_ip = get_ip(dhcp_data.dp_ciaddr)"
    THERMAL_TINYUSB_DHCP_MARKER_OFFSET
)
if(THERMAL_TINYUSB_DHCP_MARKER_OFFSET LESS 0)
    message(FATAL_ERROR
        "TinyUSB DHCP overlay is stale or unpatched"
    )
endif()

get_target_property(
    THERMAL_TINYUSB_DEVICE_SOURCES tinyusb_device_base INTERFACE_SOURCES
)
list(REMOVE_ITEM THERMAL_TINYUSB_DEVICE_SOURCES
    ${PICO_TINYUSB_PATH}/src/class/net/ncm_device.c
)
list(APPEND THERMAL_TINYUSB_DEVICE_SOURCES
    ${THERMAL_TINYUSB_NCM_OVERLAY}/src/class/net/ncm_device.c
)
set_property(
    TARGET tinyusb_device_base PROPERTY INTERFACE_SOURCES
    "${THERMAL_TINYUSB_DEVICE_SOURCES}"
)

set(THERMAL_WEB_ASSETS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html
    ${CMAKE_CURRENT_SOURCE_DIR}/src/web/assets/sr-fast.wasm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/web/assets/sr-fast-simd.wasm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/web/assets/sr-fast.bin
)
set(THERMAL_WEB_ASSEMBLY ${CMAKE_CURRENT_BINARY_DIR}/web_assets.S)
configure_file(src/web_assets.S.in ${THERMAL_WEB_ASSEMBLY} @ONLY)
set_source_files_properties(
    ${THERMAL_WEB_ASSEMBLY}
    PROPERTIES OBJECT_DEPENDS "${THERMAL_WEB_ASSETS}"
)

set(FAMILY rp2040)
set(BOARD pico_sdk)
set(TINYUSB_FAMILY_PROJECT_NAME_PREFIX "tinyusb_dev_")
set(TINYUSB_LWIP_PATH ${PICO_LWIP_PATH})
set(TOP ${PICO_TINYUSB_PATH})
set(PICO_TINYUSB_CONFIG_PATH
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(
    ${PROJECT_NAME} 
    src/sdk.c
    src/main.c
    src/thermal_calibration_v1.c
    src/thermal_ncm.c
    src/thermal_ota.c
    src/cmap/turbo.c
    src/init.c
    src/led.c
    src/static_data.c
    src/static_data2.c
    src/dutx.c
    src/utils.c
    src/usb_descriptors.c
    ${THERMAL_WEB_ASSEMBLY}
    ${THERMAL_TINYUSB_NCM_OVERLAY}/lib/networking/dhserver.c
    src/dnserver.c
)
target_link_options(
    ${PROJECT_NAME} PRIVATE
    "LINKER:--script=${CMAKE_CURRENT_SOURCE_DIR}/pico_flash_region_custom.ld"
)
set_property(
    TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/pico_flash_region_custom.ld
)
set_property(TARGET ${PROJECT_NAME} PROPERTY OUTPUT_NAME "${PROJECT_NAME}.elf")
set_property(TARGET ${PROJECT_NAME} PROPERTY SUFFIX "")
set_property(
    SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
    THERMAL_OTA_BOOT_PROBE=$<BOOL:${THERMAL_OTA_BOOT_PROBE}>
)
target_compile_options(${PROJECT_NAME} PRIVATE ${DEFAULT_C_FLAGS})
target_compile_definitions(
    ${PROJECT_NAME} PRIVATE
    REG_DATE_SPI_LEN=1286
    REG_DATE_SPI_LEN32=320
    PICO_FLASH_SIZE_BYTES=2097152
    PICO_STACK_SIZE=2048
    PICO_CORE1_STACK_SIZE=4096
    THERMAL_NCM_MODE=1
    PICO_TN160_VERSION=\"${PROJECT_VERSION}\"
)

target_link_libraries(
    ${PROJECT_NAME} PRIVATE
    pico_stdlib
    hardware_pio
    hardware_spi
    hardware_dma
    pico_unique_id
    tinyusb_device
    tinyusb_board
    pico_lwip_nosys
    pico_multicore
    hardware_adc
    hardware_flash
)

target_include_directories(
    ${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${PICO_TINYUSB_PATH}/lib/networking>
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wno-error=sign-conversion
    -Wno-error=conversion
)

target_link_options(
    ${PROJECT_NAME} PRIVATE
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/th160.map
)


pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/pio0)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/pio0_io)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/piospi0)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/dutx)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/rei)
pico_enable_stdio_usb(${PROJECT_NAME} 0)
pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_add_extra_outputs(${PROJECT_NAME})

