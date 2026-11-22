cmake_minimum_required(VERSION 3.20)
# macOS: run on macOS 12 and later, not only on the SDK's own version (must be set before project()).
# Without this, Apple's compiler stamps the runner's newest macOS as the minimum and older Macs refuse to launch the app.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()
project(WeztRecordingTools VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # (before any target is created) M_PI and friends, no min/max macros from windows.h, UTF-8 sources, and a correct __cplusplus value
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
  add_compile_options(/utf-8 /Zc:__cplusplus)
endif()

option(WEZT_BUILD_UI "Build the Dear ImGui + GLFW app (wezt-studio) and the UI tests" OFF)
option(WEZT_ENABLE_VST3 "Host VST3 plugins (needs the Steinberg VST3 SDK, MIT licensed)" OFF)
set(WEZT_VST3_SDK_DIR "" CACHE PATH "Path to a checkout of github.com/steinbergmedia/vst3sdk (with submodules)")
option(WEZT_ENABLE_CLAP "Host CLAP plugins (needs the MIT-licensed CLAP headers)" OFF)
set(WEZT_CLAP_DIR "" CACHE PATH "Path to a checkout of github.com/free-audio/clap")
option(WEZT_ENABLE_BRIDGE "Out-of-process plugin bridge (wezt-bridge helper); needs WEZT_ENABLE_VST3" OFF)
set(WEZT_VST3_TEST_PLUGIN "" CACHE FILEPATH "A .vst3 for the host tests; defaults to the SDK's again-sample-accurate when built")
option(WEZT_ENABLE_VST2 "Enable VST2 loader (requires your own SDK headers)" OFF)

add_library(wezt_core
  src/engine/graph.cpp
  src/engine/engine.cpp
  src/io/wav.cpp
  src/io/audio_file.cpp
  src/util/prefs.cpp
  src/ui/logo.cpp
  src/ui/logo_sdf.cpp
  src/ui/intro_sound.cpp
  src/audio/null_device.cpp
  src/audio/wasapi_device.cpp
  src/audio/coreaudio_device.cpp
  src/audio/platform_stub.cpp
  src/plugins/aax_twin.cpp
  src/plugins/plugin_library.cpp
  src/session/track_source.cpp
  src/session/automated_effect.cpp
  src/session/session.cpp
  src/edit/memory_locations.cpp
  src/music/midi.cpp
  src/music/notation.cpp
  src/music/theory.cpp
  src/music/synth.cpp
  src/music/song.cpp
  src/ai/mix_assistant.cpp
  src/ai/producer.cpp
  src/ai/tutor.cpp
  src/mcp/server.cpp
  src/music/audio_to_midi.cpp
  src/music/separation.cpp
  src/music/psola.cpp
  src/dsp/elastic.cpp
  src/voice/voice_model.cpp
  src/voice/voice_bank.cpp
  src/voice/singing.cpp
  src/agent/semantics.cpp
  src/agent/plugin_agent.cpp
  src/voice/voice_analysis.cpp
  src/voice/voice_takes.cpp
  src/midi/midi_input.cpp
  src/midi/midi_input_win.cpp
  src/midi/midi_input_mac.cpp
  src/music/vocal_fit.cpp
  src/edit/clip_tools.cpp
  src/edit/piano_roll.cpp
  src/edit/playlists.cpp
  src/edit/beat_detective.cpp
  src/music/midi_edit.cpp
  src/ai/features.cpp
  src/ai/profile.cpp
  src/ai/advisor.cpp
  src/ai/master.cpp
  src/ai/audio_file.cpp
  src/ai/llm.cpp
  src/ai/assistant.cpp
  src/ui/commands.cpp
  src/instruments/sampler.cpp
  src/instruments/wezt_synth.cpp
  src/session/midi_player.cpp
  src/immersive/layout.cpp
  src/immersive/renderer.cpp
  src/immersive/binaural.cpp
  src/immersive/loudness.cpp
  src/immersive/adm.cpp)
target_include_directories(wezt_core PUBLIC src include)
target_include_directories(wezt_core SYSTEM PRIVATE third_party/minimp3)
if(MSVC)
  target_compile_options(wezt_core PRIVATE /W4)
else()
  target_compile_options(wezt_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
  target_link_libraries(wezt_core PUBLIC ole32 avrt uuid winmm)
elseif(APPLE)
  target_link_libraries(wezt_core PUBLIC "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation" "-framework CoreMIDI")
else()
  find_package(Threads REQUIRED)
  target_link_libraries(wezt_core PUBLIC Threads::Threads)
endif()

if(WEZT_ENABLE_VST3)
  if(NOT WEZT_VST3_SDK_DIR)
    message(FATAL_ERROR "WEZT_ENABLE_VST3 needs -DWEZT_VST3_SDK_DIR=<path to vst3sdk>")
  endif()
  set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
  set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
  set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
  # Set -DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=ON to also build the SDK sample plugins (used as test subjects).
  if(NOT DEFINED SMTG_ENABLE_VST3_PLUGIN_EXAMPLES)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(${WEZT_VST3_SDK_DIR} vst3sdk EXCLUDE_FROM_ALL)
  # The SDK compiles these straight into each host instead of shipping them in a library.
  set(WEZT_VST3_SDK_SOURCES
    ${WEZT_VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${WEZT_VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp)
  if(WIN32)
    list(APPEND WEZT_VST3_SDK_SOURCES ${WEZT_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp)
  elseif(APPLE)
    # The SDK's Objective-C++ module loader, compiled by the C++ driver with -x objective-c++ (enable_language(OBJCXX)
    # fails to set up its compile rule on current CMake after the SDK has configured its own OBJCXX targets).
    set(_mm ${WEZT_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm)
    set_source_files_properties(${_mm} PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x objective-c++ -fobjc-arc")
    list(APPEND WEZT_VST3_SDK_SOURCES ${_mm})
  else()
    list(APPEND WEZT_VST3_SDK_SOURCES ${WEZT_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
  endif()
  add_library(wezt_vst3 src/plugins/vst3_host.cpp ${WEZT_VST3_SDK_SOURCES})
  target_include_directories(wezt_vst3 PUBLIC src ${WEZT_VST3_SDK_DIR})
  target_link_libraries(wezt_vst3 PUBLIC wezt_core sdk_hosting sdk sdk_common base pluginterfaces ${CMAKE_DL_LIBS})
  if(APPLE)
    target_link_libraries(wezt_vst3 PUBLIC "-framework CoreFoundation" "-framework Foundation")   # module_mac.mm uses NSBundle, NSFileManager, NSLog
  endif()
  target_compile_definitions(wezt_core PUBLIC WEZT_HAS_VST3=1)
endif()

if(WEZT_ENABLE_VST3 AND NOT WEZT_VST3_TEST_PLUGIN AND SMTG_ENABLE_VST3_PLUGIN_EXAMPLES)
  set(WEZT_VST3_TEST_PLUGIN "${CMAKE_BINARY_DIR}/VST3/${CMAKE_BUILD_TYPE}/again-sample-accurate.vst3")
endif()

enable_testing()
add_executable(wezt_engine_test tests/test_engine.cpp)
target_link_libraries(wezt_engine_test PRIVATE wezt_core)
add_test(NAME wezt_engine_test COMMAND wezt_engine_test)

add_executable(wezt_wav_test tests/test_wav.cpp)
target_link_libraries(wezt_wav_test PRIVATE wezt_core)
add_test(NAME wezt_wav_test COMMAND wezt_wav_test)
add_executable(wezt_voice_takes tools/voice_takes.cpp)
target_link_libraries(wezt_voice_takes PRIVATE wezt_core)
add_executable(wezt_voice_analyze tools/voice_analyze.cpp)
target_link_libraries(wezt_voice_analyze PRIVATE wezt_core)
add_executable(wezt_wavdump tests/tools/wavdump.cpp)
target_link_libraries(wezt_wavdump PRIVATE wezt_core)

add_executable(wezt_tests tests/test_main.cpp)
target_link_libraries(wezt_tests PRIVATE wezt_core)
add_test(NAME wezt_tests COMMAND wezt_tests)

# Simulators let us run the real Windows/macOS backend sources on a Linux machine. They model the
# OS APIs; they do not replace testing on the real systems. Only built on hosts that are neither.
if(NOT WIN32 AND NOT APPLE)
  add_executable(wezt_sim_macos
    tests/test_sim_macos.cpp
    tests/sim/macos/sim_coreaudio.cpp
    src/audio/coreaudio_device.cpp
    src/engine/engine.cpp
    src/engine/graph.cpp)
  target_include_directories(wezt_sim_macos PRIVATE tests/sim/macos src)
  target_compile_definitions(wezt_sim_macos PRIVATE WEZT_SIM_MACOS=1)
  target_link_libraries(wezt_sim_macos PRIVATE Threads::Threads)
  add_test(NAME wezt_sim_macos COMMAND wezt_sim_macos)

  # Same idea for WASAPI: the real backend source, compiled against a model of the Windows API.
  # tests/sim/windows is put first on the include path so <windows.h>, <audioclient.h>... resolve
  # to the model.
  add_executable(wezt_sim_windows
    tests/test_sim_windows.cpp
    tests/sim/windows/sim_windows.cpp
    src/audio/wasapi_device.cpp
    src/engine/engine.cpp
    src/engine/graph.cpp)
  target_include_directories(wezt_sim_windows BEFORE PRIVATE tests/sim/windows)
  target_include_directories(wezt_sim_windows PRIVATE src)
  target_compile_definitions(wezt_sim_windows PRIVATE WEZT_SIM_WINDOWS=1)
  target_link_libraries(wezt_sim_windows PRIVATE Threads::Threads)
  add_test(NAME wezt_sim_windows COMMAND wezt_sim_windows)
endif()

if(WEZT_ENABLE_VST3)
  add_executable(wezt_vst3_test tests/test_vst3.cpp)
  target_link_libraries(wezt_vst3_test PRIVATE wezt_vst3)
  add_test(NAME wezt_vst3_test COMMAND wezt_vst3_test $<$<BOOL:${WEZT_VST3_TEST_PLUGIN}>:${WEZT_VST3_TEST_PLUGIN}>)

  # MIDI delivery to real VST3 instruments: classes from the SDK's mda-vst3 bundle.
  add_executable(wezt_vst3_midi_test tests/test_vst3_midi.cpp)
  target_link_libraries(wezt_vst3_midi_test PRIVATE wezt_vst3)
  if(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES)
    set(WEZT_VST3_TEST_INSTRUMENT "${CMAKE_BINARY_DIR}/VST3/${CMAKE_BUILD_TYPE}/mda-vst3.vst3" CACHE FILEPATH "")
    add_dependencies(wezt_vst3_midi_test mda-vst3)
    add_test(NAME wezt_vst3_midi_piano COMMAND wezt_vst3_midi_test ${WEZT_VST3_TEST_INSTRUMENT} "mda Piano")
    add_test(NAME wezt_vst3_midi_dx10 COMMAND wezt_vst3_midi_test ${WEZT_VST3_TEST_INSTRUMENT} "mda DX10")
    add_test(NAME wezt_vst3_midi_epiano COMMAND wezt_vst3_midi_test ${WEZT_VST3_TEST_INSTRUMENT} "mda EPiano")
    add_test(NAME wezt_vst3_midi_jx10 COMMAND wezt_vst3_midi_test ${WEZT_VST3_TEST_INSTRUMENT} "mda JX10")
  endif()
endif()

if(WEZT_ENABLE_CLAP)
  if(NOT WEZT_CLAP_DIR)
    message(FATAL_ERROR "WEZT_ENABLE_CLAP needs -DWEZT_CLAP_DIR=<path to clap checkout>")
  endif()
  add_library(wezt_clap src/plugins/clap_host.cpp)
  target_include_directories(wezt_clap PUBLIC src ${WEZT_CLAP_DIR}/include)
  target_link_libraries(wezt_clap PUBLIC wezt_core ${CMAKE_DL_LIBS})
  target_compile_definitions(wezt_core PUBLIC WEZT_HAS_CLAP=1)

  # Test subject: a tiny CLAP plugin written for this project.
  add_library(wezt_test_gain MODULE tests/clap_gain/gain_plugin.cpp)
  target_include_directories(wezt_test_gain PRIVATE ${WEZT_CLAP_DIR}/include ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(wezt_test_gain PROPERTIES PREFIX "" SUFFIX ".clap")
  add_executable(wezt_clap_test tests/test_clap.cpp)
  target_link_libraries(wezt_clap_test PRIVATE wezt_clap)
  add_test(NAME wezt_clap_test COMMAND wezt_clap_test $<TARGET_FILE:wezt_test_gain>)

  # Test subject for MIDI delivery: a CLAP instrument that takes note events (and a twin that takes raw MIDI).
  add_library(wezt_test_synth MODULE tests/clap_synth/synth_plugin.cpp)
  target_include_directories(wezt_test_synth PRIVATE ${WEZT_CLAP_DIR}/include)
  set_target_properties(wezt_test_synth PROPERTIES PREFIX "" SUFFIX ".clap")
  add_executable(wezt_clap_midi_test tests/test_clap_midi.cpp)
  target_link_libraries(wezt_clap_midi_test PRIVATE wezt_clap)
  add_test(NAME wezt_clap_midi_test COMMAND wezt_clap_midi_test $<TARGET_FILE:wezt_test_synth> $<TARGET_FILE:wezt_test_gain>)
endif()

if(WEZT_ENABLE_BRIDGE)
  if(NOT WEZT_ENABLE_VST3)
    message(FATAL_ERROR "WEZT_ENABLE_BRIDGE needs WEZT_ENABLE_VST3 (the helper hosts the plugin)")
  endif()
  add_library(wezt_bridge STATIC src/plugins/bridge/bridge_host.cpp src/plugins/bridge/bridge_ipc.cpp)
  target_include_directories(wezt_bridge PUBLIC src)
  target_link_libraries(wezt_bridge PUBLIC wezt_core)
  if(NOT WIN32 AND NOT APPLE)
    target_link_libraries(wezt_bridge PUBLIC rt)
  endif()
  target_compile_definitions(wezt_core PUBLIC WEZT_HAS_BRIDGE=1)

  # The helper is built once per bitness: the 64-bit one ships with Wezt; a 32-bit one is built
  # separately (cmake -A Win32 on Windows) and pointed to by BridgeConfig::helperExecutable.
  add_executable(wezt-bridge src/plugins/bridge/bridge_helper.cpp)
  target_link_libraries(wezt-bridge PRIVATE wezt_bridge wezt_vst3)
  if(WEZT_ENABLE_CLAP)
    target_link_libraries(wezt-bridge PRIVATE wezt_clap)   # sets WEZT_HAS_CLAP through wezt_core
  endif()

  add_executable(wezt_bridge_test tests/test_bridge.cpp)
  target_link_libraries(wezt_bridge_test PRIVATE wezt_bridge wezt_vst3)
  if(WEZT_ENABLE_CLAP)
    target_link_libraries(wezt_bridge_test PRIVATE wezt_clap)
  endif()
  add_test(NAME wezt_bridge_test COMMAND wezt_bridge_test $<TARGET_FILE:wezt-bridge> $<$<BOOL:${WEZT_VST3_TEST_PLUGIN}>:${WEZT_VST3_TEST_PLUGIN}> $<$<AND:$<BOOL:${WEZT_VST3_TEST_PLUGIN}>,$<BOOL:${WEZT_VST3_TEST_INSTRUMENT}>>:${WEZT_VST3_TEST_INSTRUMENT}> $<$<BOOL:${WEZT_ENABLE_CLAP}>:--clap $<TARGET_FILE:wezt_test_gain>>)
endif()

# Make `cmake --build` + `ctest` work with no manual steps: when the SDK's sample plugins are enabled,
# build the gain plugin the host tests load, and point the tests at it.
if(WEZT_ENABLE_VST3 AND TARGET again-sample-accurate)
  foreach(t wezt_vst3_test wezt_bridge_test)
    if(TARGET ${t})
      add_dependencies(${t} again-sample-accurate)
    endif()
  endforeach()
endif()

add_executable(wezt_midi_test tests/test_midi.cpp)
target_link_libraries(wezt_midi_test PRIVATE wezt_core)
add_test(NAME wezt_midi_test COMMAND wezt_midi_test)

add_executable(wezt_commands_test tests/test_commands.cpp)
target_link_libraries(wezt_commands_test PRIVATE wezt_core)
add_test(NAME wezt_commands_test COMMAND wezt_commands_test)

add_executable(wezt_sampler_test tests/test_sampler.cpp)
target_link_libraries(wezt_sampler_test PRIVATE wezt_core)
add_test(NAME wezt_sampler_test COMMAND wezt_sampler_test)

add_executable(wezt_immersive_test tests/test_immersive.cpp)
target_link_libraries(wezt_immersive_test PRIVATE wezt_core)
add_test(NAME wezt_immersive_test COMMAND wezt_immersive_test)

add_executable(wezt_param_automation_test tests/test_param_automation.cpp)
target_link_libraries(wezt_param_automation_test PRIVATE wezt_core)
add_test(NAME wezt_param_automation_test COMMAND wezt_param_automation_test)

add_executable(wezt_intro_test tests/test_intro.cpp)
target_link_libraries(wezt_intro_test PRIVATE wezt_core)
target_compile_definitions(wezt_intro_test PRIVATE WEZT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
add_test(NAME wezt_intro_test COMMAND wezt_intro_test)

add_executable(wezt_session_test tests/test_session.cpp)
target_link_libraries(wezt_session_test PRIVATE wezt_core)
add_test(NAME wezt_session_test COMMAND wezt_session_test)

# ---- User interface (Dear ImGui). Needs the ImGui sources: -DWEZT_BUILD_UI=ON -DWEZT_IMGUI_DIR=<path to imgui>.
# wezt_ui_snapshot renders a frame on the CPU (no window needed); wezt-studio is the desktop app and
# needs GLFW (-DWEZT_GLFW_DIR=<path to glfw source>, or an installed glfw3) and OpenGL.
option(WEZT_BUILD_UI "Build the Dear ImGui user interface" OFF)
set(WEZT_IMGUI_DIR "" CACHE PATH "Dear ImGui source folder")
set(WEZT_GLFW_DIR "" CACHE PATH "GLFW source folder (optional; an installed glfw3 is used otherwise)")
if(WEZT_BUILD_UI)
  if(NOT EXISTS "${WEZT_IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR "WEZT_IMGUI_DIR must point at a Dear ImGui checkout")
  endif()
  add_library(wezt_ui STATIC
    src/ui/app.cpp src/ui/app_edit.cpp src/ui/app_mix.cpp src/ui/app_sampler.cpp src/ui/app_immersive.cpp src/ui/app_demo.cpp src/ui/app_assistant.cpp src/ui/app_clips.cpp src/ui/app_midi.cpp src/ui/app_voice.cpp src/ui/app_plugins.cpp src/ui/app_settings.cpp src/ui/intro.cpp src/ui/intro_gl.cpp src/ui/insert_factory.cpp
    ${WEZT_IMGUI_DIR}/imgui.cpp ${WEZT_IMGUI_DIR}/imgui_draw.cpp ${WEZT_IMGUI_DIR}/imgui_tables.cpp ${WEZT_IMGUI_DIR}/imgui_widgets.cpp
    ${WEZT_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp)
  target_include_directories(wezt_ui PUBLIC src ${WEZT_IMGUI_DIR})
  target_link_libraries(wezt_ui PUBLIC wezt_core)
  if(WEZT_ENABLE_VST3)
    target_link_libraries(wezt_ui PUBLIC wezt_vst3)
  endif()
  if(WEZT_ENABLE_CLAP)
    target_link_libraries(wezt_ui PUBLIC wezt_clap)
  endif()
  if(WEZT_ENABLE_BRIDGE)
    target_link_libraries(wezt_ui PUBLIC wezt_bridge)
  endif()
  add_executable(wezt_ui_test tests/test_ui.cpp)
  target_link_libraries(wezt_ui_test PRIVATE wezt_ui)
  add_test(NAME wezt_ui_test COMMAND wezt_ui_test)
  add_executable(wezt_ui_snapshot tools/ui_snapshot.cpp)
  target_link_libraries(wezt_ui_snapshot PRIVATE wezt_ui)
  if(UNIX AND NOT APPLE)
    # Dev tool: renders the GL intro offscreen through EGL (Mesa software rendering is enough) to check how it looks.
    add_executable(wezt_intro_gl_snapshot tools/intro_gl_snapshot.cpp)
    target_link_libraries(wezt_intro_gl_snapshot PRIVATE wezt_ui ${CMAKE_DL_LIBS})
  endif()

  set(_glfw_ok OFF)
  if(WEZT_GLFW_DIR AND EXISTS "${WEZT_GLFW_DIR}/CMakeLists.txt")
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${WEZT_GLFW_DIR} glfw EXCLUDE_FROM_ALL)
    set(_glfw_ok ON)
  else()
    find_package(glfw3 QUIET)
    if(glfw3_FOUND)
      set(_glfw_ok ON)
    endif()
  endif()
  find_package(OpenGL QUIET)
  if(_glfw_ok AND OpenGL_FOUND)
    add_executable(wezt-studio src/app/main.cpp
      ${WEZT_IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${WEZT_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(wezt-studio PRIVATE ${WEZT_IMGUI_DIR})
    target_link_libraries(wezt-studio PRIVATE wezt_ui glfw OpenGL::GL)
    if(MSVC)   # a desktop program: no console window behind it (main() stays the entry point)
      set_target_properties(wezt-studio PROPERTIES WIN32_EXECUTABLE TRUE)
      target_link_options(wezt-studio PRIVATE /ENTRY:mainCRTStartup)
    endif()
  else()
    message(STATUS "wezt-studio not built: GLFW or OpenGL not found (the snapshot tool is still built)")
  endif()
endif()

add_executable(wezt_edit_test tests/test_edit.cpp)
target_link_libraries(wezt_edit_test PRIVATE wezt_core)
add_test(NAME wezt_edit_test COMMAND wezt_edit_test)

add_executable(wezt_ai_test tests/test_ai.cpp)
target_link_libraries(wezt_ai_test PRIVATE wezt_core)
add_test(NAME wezt_ai_test COMMAND wezt_ai_test)

add_executable(wezt_assistant_test tests/test_assistant.cpp)
target_link_libraries(wezt_assistant_test PRIVATE wezt_core)
add_test(NAME wezt_assistant_test COMMAND wezt_assistant_test)

add_executable(wezt_music_test tests/test_music.cpp)
target_link_libraries(wezt_music_test PRIVATE wezt_core)
add_test(NAME wezt_music_test COMMAND wezt_music_test)

add_executable(wezt-mcp src/mcp/main.cpp)
target_link_libraries(wezt-mcp PRIVATE wezt_core)

add_executable(wezt_agent_test tests/test_agent.cpp)
target_link_libraries(wezt_agent_test PRIVATE wezt_core)
add_test(NAME wezt_agent_test COMMAND wezt_agent_test)

add_executable(wezt_mcp_test tests/test_mcp.cpp)
target_link_libraries(wezt_mcp_test PRIVATE wezt_core)
add_test(NAME wezt_mcp_test COMMAND wezt_mcp_test)

add_executable(wezt_elastic_test tests/test_elastic.cpp)
target_link_libraries(wezt_elastic_test PRIVATE wezt_core)
add_test(NAME wezt_elastic_test COMMAND wezt_elastic_test)

add_executable(wezt_voice_test tests/test_voice.cpp)
target_link_libraries(wezt_voice_test PRIVATE wezt_core)
add_test(NAME wezt_voice_test COMMAND wezt_voice_test)

add_executable(wezt_edit2_test tests/test_edit2.cpp)
target_link_libraries(wezt_edit2_test PRIVATE wezt_core)
add_test(NAME wezt_edit2_test COMMAND wezt_edit2_test)

add_executable(wezt_sing_test tests/test_sing.cpp)
target_link_libraries(wezt_sing_test PRIVATE wezt_core)
add_test(NAME wezt_sing_test COMMAND wezt_sing_test)
add_executable(wezt_sing_human_test tests/test_sing_human.cpp)
target_link_libraries(wezt_sing_human_test PRIVATE wezt_core)
add_test(NAME wezt_sing_human_test COMMAND wezt_sing_human_test)
add_executable(wezt_param_ring_test tests/test_param_ring.cpp)
target_link_libraries(wezt_param_ring_test PRIVATE wezt_core)
add_test(NAME wezt_param_ring_test COMMAND wezt_param_ring_test)

add_executable(wezt_voice_bank_test tests/test_voice_bank.cpp)
target_link_libraries(wezt_voice_bank_test PRIVATE wezt_core)
add_test(NAME wezt_voice_bank_test COMMAND wezt_voice_bank_test)

add_executable(wezt_voice_takes_test tests/test_voice_takes.cpp)
target_link_libraries(wezt_voice_takes_test PRIVATE wezt_core)
add_test(NAME wezt_voice_takes_test COMMAND wezt_voice_takes_test)
add_executable(wezt_midi_input_test tests/test_midi_input.cpp)
target_link_libraries(wezt_midi_input_test PRIVATE wezt_core)
add_test(NAME wezt_midi_input_test COMMAND wezt_midi_input_test)

add_executable(wezt_voice_analysis_test tests/test_voice_analysis.cpp)
target_link_libraries(wezt_voice_analysis_test PRIVATE wezt_core)
add_test(NAME wezt_voice_analysis_test COMMAND wezt_voice_analysis_test)
