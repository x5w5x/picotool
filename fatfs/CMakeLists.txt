# FatFs_SPI layout, using the project's Drivers/tf.c instead of sd_driver.
add_library(FatFs_SPI STATIC
    ff15/source/ffsystem.c
    ff15/source/ffunicode.c
    ff15/source/ff.c
    src/glue.c
    src/f_util.c
    src/ff_stdio.c
    src/my_debug.c
    src/rtc.c
)

target_include_directories(FatFs_SPI PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/ff15/source
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/../Drivers
)

target_link_libraries(FatFs_SPI PUBLIC
    drivelib
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_rtc
)

target_compile_options(FatFs_SPI PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-unused-variable
    -finput-charset=UTF-8
    -fexec-charset=GBK
)
