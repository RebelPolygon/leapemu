cmake_minimum_required(VERSION 3.24)
project(leapemu VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(LEAPEMU_BUILD_GUI   "Build the SDL3 + Dear ImGui frontend" ON)
option(LEAPEMU_BUILD_TESTS "Build unit tests" ON)
option(LEAPEMU_BUILD_TOOLS "Build the analysis and debugging tools in tools/ (for development)" OFF)
option(LEAPEMU_VENDOR_SDL  "Build SDL3 from source and link it statically, ignoring a system SDL3 (release builds)" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Core: pure C++20, no third-party dependencies. Everything that emulates
# hardware lives here so the CLI, GUI and a future libretro core share it.
# ---------------------------------------------------------------------------
add_library(leapcore STATIC
  src/core/arc/cpu.cpp
  src/core/arc/decode.cpp
  src/core/arc/disasm.cpp
  src/core/arc/jit.cpp
  src/core/arc/jit_x64.cpp
  src/core/arc/jit_a64.cpp
  src/core/arc/xy.cpp
  src/core/bus.cpp
  src/core/celp.cpp
  src/core/assets.cpp
  src/core/patch.cpp
  src/core/rom.cpp
  src/core/syn.cpp
  src/core/soundtrack.cpp
  src/core/image.cpp
  src/core/interp.cpp
  src/core/codesig.cpp
  src/core/fileio.cpp
  src/core/drawcap.cpp
  src/core/flash/swf.cpp
  src/core/flash/jpeg.cpp
  src/core/flash/raster.cpp
  src/core/flash/render.cpp
  src/core/flash/timeline.cpp
  src/core/machine.cpp
  src/core/movie.cpp
  src/core/scale.cpp
  src/core/rewind.cpp
)
target_include_directories(leapcore PUBLIC src)

# (Both frontends report this version: --version, Help > About.)
target_compile_definitions(leapcore PUBLIC LEAPEMU_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# CLI frontend
# ---------------------------------------------------------------------------
add_executable(leapemu-cli src/cli/main.cpp)
target_link_libraries(leapemu-cli PRIVATE leapcore)

# ---------------------------------------------------------------------------
# GUI frontend
# ---------------------------------------------------------------------------
if(LEAPEMU_BUILD_GUI)
  include(FetchContent)
  # (Third-party code is built without leapemu's warning options.)
  get_directory_property(leapemu_compile_options COMPILE_OPTIONS)
  set_directory_properties(PROPERTIES COMPILE_OPTIONS "")

  if(NOT LEAPEMU_VENDOR_SDL)
    find_package(SDL3 CONFIG QUIET)
  endif()
  if(NOT SDL3_FOUND)
    message(STATUS "Building SDL3 from source")
    if(LEAPEMU_VENDOR_SDL)
      set(SDL_SHARED OFF CACHE BOOL "" FORCE)
      set(SDL_STATIC ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(SDL3
      GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
      GIT_TAG release-3.4.8
      GIT_SHALLOW TRUE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)
  endif()

  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b-docking
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(imgui)
  set_directory_properties(PROPERTIES COMPILE_OPTIONS "${leapemu_compile_options}")

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC SDL3::SDL3)

  add_executable(leapemu src/gui/main.cpp src/gui/gamelist.cpp src/gui/romwindow.cpp)
  target_link_libraries(leapemu PRIVATE leapcore imgui SDL3::SDL3)
  if(WIN32)
    # A windowed program (no console), with the icon and version information.
    enable_language(RC)
    configure_file(res/leapemu.rc.in leapemu.rc @ONLY)
    target_sources(leapemu PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/leapemu.rc)
    set_target_properties(leapemu PROPERTIES WIN32_EXECUTABLE TRUE)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LEAPEMU_BUILD_TESTS)
  enable_testing()
  add_executable(leapemu-tests tests/cpu_tests.cpp)
  target_link_libraries(leapemu-tests PRIVATE leapcore)
  add_test(NAME cpu COMMAND leapemu-tests)
  add_executable(leapemu-display-tests tests/display_tests.cpp)
  target_link_libraries(leapemu-display-tests PRIVATE leapcore)
  add_test(NAME display COMMAND leapemu-display-tests)
endif()
# ---------------------------------------------------------------------------
# Development tools (tools/README is the overview): the clean-room
# specification tools use an analysis variant of the core with memory-access
# watch hooks (never used by the emulator itself).
# ---------------------------------------------------------------------------
if(LEAPEMU_BUILD_TOOLS)
  get_target_property(LEAPCORE_SOURCES leapcore SOURCES)
  add_library(leapcore_analysis STATIC ${LEAPCORE_SOURCES})
  target_include_directories(leapcore_analysis PUBLIC src)
  target_compile_definitions(leapcore_analysis PUBLIC LEAPEMU_ANALYSIS=1 LEAPEMU_VERSION="${PROJECT_VERSION}")
  add_executable(callmap tools/abi/callmap.cpp)
  target_link_libraries(callmap PRIVATE leapcore_analysis)
  add_executable(cartreads tools/abi/cartreads.cpp)
  target_link_libraries(cartreads PRIVATE leapcore_analysis)
  add_executable(firstuse tools/abi/firstuse.cpp)
  target_link_libraries(firstuse PRIVATE leapcore_analysis)
  add_executable(ioreaders tools/abi/ioreaders.cpp)
  target_link_libraries(ioreaders PRIVATE leapcore_analysis)
  add_executable(argscan tools/abi/argscan.cpp)
  target_link_libraries(argscan PRIVATE leapcore_analysis)
  add_executable(calltree tools/abi/calltree.cpp)
  target_link_libraries(calltree PRIVATE leapcore_analysis)
  add_executable(regionmap tools/abi/regionmap.cpp)
  target_link_libraries(regionmap PRIVATE leapcore_analysis)
  add_executable(surfacemap tools/abi/surfacemap.cpp)
  target_link_libraries(surfacemap PRIVATE leapcore_analysis)
  add_executable(drawmap tools/abi/drawmap.cpp)
  target_link_libraries(drawmap PRIVATE leapcore_analysis)
  add_executable(backend_diff tools/debug/backend_diff.cpp)
  target_link_libraries(backend_diff PRIVATE leapcore)
  add_executable(framecalls tools/abi/framecalls.cpp)
  target_link_libraries(framecalls PRIVATE leapcore_analysis)
  add_executable(blitlog tools/abi/blitlog.cpp)
  target_link_libraries(blitlog PRIVATE leapcore)
  add_executable(calllog tools/abi/calllog.cpp)
  target_link_libraries(calllog PRIVATE leapcore)
  add_executable(tilelog tools/abi/tilelog.cpp)
  target_link_libraries(tilelog PRIVATE leapcore)
  add_executable(drawcap_check tools/debug/drawcap_check.cpp)
  target_link_libraries(drawcap_check PRIVATE leapcore)
  add_executable(flash_check tools/debug/flash_check.cpp)
  target_link_libraries(flash_check PRIVATE leapcore)
  add_executable(interp_eval tools/debug/interp_eval.cpp)
  target_link_libraries(interp_eval PRIVATE leapcore)
  add_executable(swf_play tools/debug/swf_play.cpp)
  target_link_libraries(swf_play PRIVATE leapcore)
  add_executable(jpeg_check tools/debug/jpeg_check.cpp)
  target_link_libraries(jpeg_check PRIVATE leapcore)
  add_executable(cutscene_eval tools/debug/cutscene_eval.cpp)
  target_link_libraries(cutscene_eval PRIVATE leapcore)
  add_executable(interp_timing tools/debug/interp_timing.cpp)
  target_link_libraries(interp_timing PRIVATE leapcore)
  add_executable(jit_diff tools/debug/jit_diff.cpp)
  target_link_libraries(jit_diff PRIVATE leapcore)
  add_executable(code_sig tools/debug/code_sig.cpp)
  target_link_libraries(code_sig PRIVATE leapcore)
  add_executable(capture_survey tools/debug/capture_survey.cpp)
  target_link_libraries(capture_survey PRIVATE leapcore)
  add_executable(lfc2wav tools/speech/lfc2wav.cpp)
  target_link_libraries(lfc2wav PRIVATE leapcore)
endif()

# ---------------------------------------------------------------------------
# Install: the two programs, plus a desktop entry and the icon on Linux.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS leapemu-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(LEAPEMU_BUILD_GUI)
  install(TARGETS leapemu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(UNIX AND NOT APPLE)
    install(FILES res/leapemu.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES res/leapemu.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
  endif()
endif()
install(FILES README.md LICENSE THIRD-PARTY.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
