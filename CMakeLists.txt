cmake_minimum_required(VERSION 3.22)

include(FetchContent)
get_filename_component(prj_name ${CMAKE_CURRENT_SOURCE_DIR} NAME)

project(
    ${prj_name}
    VERSION 0.1.0
    LANGUAGES C ASM CXX
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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(pico_sdk_import)
pico_sdk_init()

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
    src/init.c
    src/led.c
    src/static_data.c
    src/static_data2.c
    src/dutx.c
    src/utils.c
    src/usb_descriptors.c
    # src/usb_descriptors_cdc.c
    src/uvc.c
    src/cmap/hot.c
    src/cmap/cool.c
    src/cmap/magma.c
    src/cmap/turbo.c
    src/cmap/gray.c
    src/cmap/deepgreen.c
    src/yuy2_smpte_bars.c
    src/yuy2_red.c
    src/yuy2_green.c
    src/yuy2_blue.c
    src/frame_FF0000.c
    src/frame_00FF00.c
    src/frame_0000FF.c
    src/frame_FFFFFF.c
)
set_property(TARGET ${PROJECT_NAME} PROPERTY OUTPUT_NAME "${PROJECT_NAME}.elf")
target_compile_options(${PROJECT_NAME} PRIVATE ${DEFAULT_C_FLAGS})
target_compile_definitions(
    ${PROJECT_NAME} PRIVATE
    REG_DATE_SPI_LEN=1286
    REG_DATE_SPI_LEN32=320
    PICO_FLASH_SIZE_BYTES=4194304
    PICO_STACK_SIZE=2048
    PICO_STACK_SIZE_CORE1=4096
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
    pico_multicore
    hardware_adc
    hardware_flash
)

target_include_directories(
    ${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
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

