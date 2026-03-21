# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

# ---- pico-extras (Phase 3 I2S audio — optional) ----
set(PICO_EXTRAS_PATH ${USERHOME}/.pico-sdk/pico-extras/2.2.0)
set(PICO_EXTRAS_IMPORT ${CMAKE_CURRENT_LIST_DIR}/pico_extras_import.cmake)
if (EXISTS ${PICO_EXTRAS_PATH} AND EXISTS ${PICO_EXTRAS_IMPORT})
    include(${PICO_EXTRAS_IMPORT})
    set(HAVE_PICO_EXTRAS TRUE)
    message(STATUS "pico-extras found — I2S audio enabled")
else()
    set(HAVE_PICO_EXTRAS FALSE)
    message(STATUS "pico-extras not found — building without I2S audio output (Phase 3 disabled)")
endif()

project(pico_nfc_player C CXX ASM)

pico_sdk_init()


add_executable(pico_nfc_player
    main.c
    mbedtls_time.c
)

target_include_directories(pico_nfc_player PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

set(LINK_LIBS
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_lwip_mbedtls
    pico_mbedtls
)

if (HAVE_PICO_EXTRAS)
    list(APPEND LINK_LIBS pico_audio_i2s)
    target_compile_definitions(pico_nfc_player PRIVATE
        PICO_AUDIO_I2S_ENABLED=1
        PICO_AUDIO_I2S_DATA_PIN=9
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=10  # GP10=BCK, GP11=LRCK
    )
endif()

target_link_libraries(pico_nfc_player ${LINK_LIBS})

pico_enable_stdio_usb(pico_nfc_player 1)
pico_enable_stdio_uart(pico_nfc_player 0)

pico_add_extra_outputs(pico_nfc_player)
