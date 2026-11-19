# Game Boy: Game Boy and Game Boy Color games on the ColecoVision. Pick a
# .gb or .gbc in the menu and press A: Peanut-GB runs it on the cart, and
# gbcv sends each picture and the sound to the console as the Game Boy
# kernel's lists. RESET goes back to the menu.
#
#   cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
#   ninja -C build                      -> build/GB.CVP
#
# Copy GB.CVP into the card's /PLUGINS folder.
cmake_minimum_required(VERSION 3.13)

# the cart's SDK: the copy in sdk/ (sdk/README.md)
set(PARROTCART_SDK ${CMAKE_CURRENT_LIST_DIR}/sdk/firmware/revc)
set(PICO_BOARD_HEADER_DIRS ${PARROTCART_SDK}/cvcart_revc/boards)
set(PICO_BOARD cvcart_revc)
set(PICO_PLATFORM rp2350-arm-s)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# the USB host: sdk/tinyusb (TinyUSB 0.21 and its fixes since), not the pico-sdk's 0.18
include(${PARROTCART_SDK}/sdk/cvpad_tinyusb.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(gb C CXX ASM)
pico_sdk_init()
include(${PARROTCART_SDK}/sdk/cvplugin.cmake)

# The same source makes the same file, whenever it's built: the compiler's
# __DATE__ and __TIME__ (the build date in the binary info) come from
# SOURCE_DATE_EPOCH, fixed here at the tested build's day, 2026-11-18.
set(BUILD_EPOCH 1795003200)
set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})
set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})

add_executable(gb src/gb.c src/gbcv.c src/gbplay.c vendor/minigb_apu/minigb_apu.c)
set_source_files_properties(src/gb.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
# src/ for the plugin's own headers, and this directory for gbplay.c's
# "vendor/..." includes
target_include_directories(gb PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src ${CMAKE_CURRENT_LIST_DIR} ${PARROTCART_SDK}/midihello)
# as the cart menu's own Game Boy mode
target_compile_definitions(gb PRIVATE MINIGB_APU_AUDIO_FORMAT_S16SYS=1 PEANUT_FULL_GBC_SUPPORT=1)
cvframes_use(gb)
cvframes_serve2(gb)   # core 1's bus loop acts on two samples of the pins that agree (sdk/cvfserve.S)
cvsd_use(gb)
cvpsram_use(gb)
cvpad_use(gb)                               # USB game pads: the port is a host
pico_enable_stdio_usb(gb 0)
pico_enable_stdio_uart(gb 0)
cvcart_plugin(gb ID parrotcart.gameboy NAME "Game Boy" VERSION 0.1.3 BY "Parrot Cart"
              HOME /gb OPENS .gb .gbc USES usb-host psram ICON src/gb_icon.txt FILE_ICON src/gb_file_icon.txt
              README CART-README.md)
