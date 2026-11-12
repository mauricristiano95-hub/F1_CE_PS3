cmake_minimum_required(VERSION 3.20)
project(f1ce_recomp C CXX)

# Static recompilation of Formula One: Championship Edition (PS3) for x86-64 Windows/Linux.
# Generated sources come from tools/recomp.py (run it first); game data is read at runtime.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(F1CE_HOST SDL CACHE STRING "Host backend: SDL (window/audio/input) or NULL (headless)")
option(F1CE_VULKAN "Vulkan renderer next to OpenGL, F1CE_RENDERER=vulkan selects it (needs third_party/glslang)" OFF)
option(F1CE_TRACE_PC "Track the guest PC of memory accesses and F1CE_WATCH_LINE stores (debugging, slower)" OFF)
if(F1CE_TRACE_PC)
  add_compile_definitions(F1CE_TRACE_PC)
endif()

# Profile-guided and link-time optimisation (clang; applies to the game and its SDL/glslang, not the launcher).
# GEN = instrumented training build, writes <F1CE_PGO_DIR>/default_<id>.profraw when the game exits;
# USE = optimised with F1CE_PGO_PROFILE (tools/pgo_train.sh runs the training and merges the profile).
set(F1CE_PGO OFF CACHE STRING "Profile-guided optimisation: OFF, GEN (training build) or USE")
set_property(CACHE F1CE_PGO PROPERTY STRINGS OFF GEN USE)
set(F1CE_PGO_DIR "${CMAKE_SOURCE_DIR}/pgo/raw" CACHE PATH "Where the F1CE_PGO=GEN build writes its raw profiles")
set(F1CE_PGO_PROFILE "${CMAKE_SOURCE_DIR}/pgo/f1ce.profdata" CACHE FILEPATH "Merged profile for F1CE_PGO=USE")
option(F1CE_LTO "ThinLTO across the game, SDL and glslang" OFF)
set(F1CE_OPT_COMPILE)
set(F1CE_OPT_LINK)
if(F1CE_PGO STREQUAL "GEN")
  list(APPEND F1CE_OPT_COMPILE -fprofile-generate=${F1CE_PGO_DIR})
  list(APPEND F1CE_OPT_LINK -fprofile-generate=${F1CE_PGO_DIR})
  add_compile_definitions(F1CE_PGO_GEN)
elseif(F1CE_PGO STREQUAL "USE")
  if(NOT EXISTS "${F1CE_PGO_PROFILE}")
    message(FATAL_ERROR "F1CE_PGO=USE: no profile at ${F1CE_PGO_PROFILE} (run tools/pgo_train.sh)")
  endif()
  # functions changed since the training (recomp.py, runtime edits) just lose their profile
  list(APPEND F1CE_OPT_COMPILE -fprofile-use=${F1CE_PGO_PROFILE} -Wno-profile-instr-unprofiled
    -Wno-profile-instr-out-of-date -Wno-profile-instr-missing -Wno-backend-plugin)
elseif(F1CE_PGO)
  message(FATAL_ERROR "F1CE_PGO must be OFF, GEN or USE")
endif()
if(F1CE_LTO)
  list(APPEND F1CE_OPT_COMPILE -flto=thin)
  list(APPEND F1CE_OPT_LINK -flto=thin -Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto_cache)
endif()
add_compile_options(${F1CE_OPT_COMPILE})
add_link_options(${F1CE_OPT_LINK})

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/ppu_*.cpp)
if(NOT GENERATED_SOURCES)
  message(FATAL_ERROR "No generated sources: run 'python tools/recomp.py' first")
endif()

set(RUNTIME_SOURCES
  runtime/core.cpp
  runtime/loader.cpp
  runtime/lv2.cpp
  runtime/fs.cpp
  runtime/mods.cpp
  runtime/hle_sys.cpp
  runtime/hle_misc.cpp
  runtime/hle_aspect.cpp
  runtime/hle_cubemap.cpp
  runtime/hle_gfxopts.cpp
  runtime/hle_skipfn.cpp
  runtime/hle_joblist.cpp
  runtime/hle_quickrace.cpp
  runtime/hle_boot.cpp
  runtime/hle_frontend.cpp
  runtime/hle_physics.cpp
  runtime/hle_rumble.cpp
  runtime/hle_aidrive.cpp
  runtime/hle_usbd.cpp
  runtime/hle_camera.cpp
  runtime/hle_gcm.cpp
  runtime/hle_spurs.cpp
  runtime/hle_savedata.cpp
  runtime/spu.cpp
  runtime/spu_pm.cpp
)
if(F1CE_HOST STREQUAL "SDL")
  list(APPEND RUNTIME_SOURCES runtime/host_sdl.cpp runtime/wheel_sdl.cpp runtime/rsx.cpp runtime/rsx_common.cpp runtime/rsx_gl.cpp)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/SDL3-3.4.18 EXCLUDE_FROM_ALL)
  if(F1CE_VULKAN)
    # GLSL -> SPIR-V for the Vulkan renderer: its programs are translated from guest microcode at run time
    foreach(opt ENABLE_OPT ENABLE_HLSL ENABLE_GLSLANG_BINARIES ENABLE_SPVREMAPPER GLSLANG_TESTS BUILD_EXTERNAL ENABLE_PCH)
      set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory(third_party/glslang EXCLUDE_FROM_ALL)
    list(APPEND RUNTIME_SOURCES runtime/rsx_vk.cpp runtime/rsx_vk_spirv.cpp)
  endif()
else()
  list(APPEND RUNTIME_SOURCES runtime/host_null.cpp runtime/rsx_null.cpp)
endif()

# x86-64-v3 (AVX2/FMA): needed for fused VMX multiply-add and fast byte swaps.
# Spelled out as features because zig's driver does not accept -march=x86-64-v3.
set(ARCH_FLAGS -msse4.2 -mpopcnt -mavx -mavx2 -mfma -mbmi -mbmi2 -mlzcnt -mmovbe -mf16c)

add_library(ppu_generated OBJECT ${GENERATED_SOURCES})
# files whose giant functions (up to ~14k lines) take hours to compile instrumented: trained without a profile
set(F1CE_PGO_SKIP ppu_030 ppu_035 ppu_045 CACHE STRING "Generated files built without PGO instrumentation")
if(F1CE_PGO STREQUAL "GEN")
  foreach(f ${F1CE_PGO_SKIP})
    set_property(SOURCE ${CMAKE_SOURCE_DIR}/generated/${f}.cpp APPEND PROPERTY COMPILE_OPTIONS -fno-profile-generate)
  endforeach()
endif()
target_include_directories(ppu_generated PRIVATE runtime generated)
target_compile_options(ppu_generated PRIVATE ${ARCH_FLAGS} -O2 -fno-strict-aliasing -Wno-unused-label
  -Wno-unused-variable -Wno-parentheses-equality -fwrapv)

# Ahead-of-time translated SPU blocks (tools/spu_aot.py): straight-line calls of the instruction handlers with
# constant words; -O2 folds the decoding away. The table file always exists (empty without translations).
file(GLOB SPU_AOT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/spu/*.cpp)
if(NOT SPU_AOT_SOURCES)
  file(WRITE ${CMAKE_SOURCE_DIR}/generated/spu/spu_aot_table.cpp
    "#include \"spu.h\"
const SpuAotBlock g_spu_aot[1] = {{0, nullptr, -1}};
const u32 g_spu_aot_count = 0;
const SpuAotCluster g_spu_aot_clusters[1] = {{0, 0, 0, nullptr, 0, nullptr, nullptr, 0}};
const u32 g_spu_aot_cluster_count = 0;
")
  set(SPU_AOT_SOURCES ${CMAKE_SOURCE_DIR}/generated/spu/spu_aot_table.cpp)
endif()
add_library(spu_aot OBJECT ${SPU_AOT_SOURCES})
target_include_directories(spu_aot PRIVATE runtime generated)
target_compile_options(spu_aot PRIVATE ${ARCH_FLAGS} -O2 -fno-strict-aliasing -fwrapv -Wno-unused-function)

add_executable(f1ce ${RUNTIME_SOURCES} $<TARGET_OBJECTS:ppu_generated> $<TARGET_OBJECTS:spu_aot>)
target_include_directories(f1ce PRIVATE runtime generated)
target_compile_options(f1ce PRIVATE ${ARCH_FLAGS} -fno-strict-aliasing -fwrapv)
if(F1CE_PGO STREQUAL "USE")   # a new profile recompiles the game's own sources
  set_property(SOURCE ${RUNTIME_SOURCES} ${GENERATED_SOURCES} ${SPU_AOT_SOURCES} APPEND PROPERTY OBJECT_DEPENDS
    ${F1CE_PGO_PROFILE})
endif()

if(F1CE_HOST STREQUAL "SDL")
  target_link_libraries(f1ce PRIVATE SDL3::SDL3-static)
  if(F1CE_VULKAN)
    target_compile_definitions(f1ce PRIVATE F1CE_VULKAN)
    target_link_libraries(f1ce PRIVATE glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
    target_include_directories(f1ce PRIVATE third_party/SDL3-3.4.18/src/video/khronos)   # Vulkan headers
  endif()
  if(WIN32)
    target_link_libraries(f1ce PRIVATE opengl32 winmm synchronization shell32)
  endif()
endif()
if(WIN32)
  target_link_options(f1ce PRIVATE -static -Wl,--stack,8388608 -Wl,-Map=f1ce.map)
  enable_language(RC)
  add_executable(f1ce_launcher WIN32 launcher/f1ce_launcher.cpp launcher/wheel_tab.cpp launcher/f1ce_launcher.rc)
  set_target_properties(f1ce_launcher PROPERTIES COMPILE_OPTIONS "" LINK_OPTIONS "")   # no PGO/LTO
  target_compile_definitions(f1ce_launcher PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)
  target_link_options(f1ce_launcher PRIVATE -static -municode)
  # (SDL: the Steering Wheel tab reads the wheel as the game does)
  target_link_libraries(f1ce_launcher PRIVATE shell32 ole32 comctl32 uxtheme dxgi SDL3::SDL3-static)
  # the built-in wheel profiles (Steering Wheel tab), next to the launcher
  add_custom_command(TARGET f1ce_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/launcher/wheels $<TARGET_FILE_DIR:f1ce_launcher>/wheels)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(f1ce PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  # native builds (linux/build.sh): libstdc++/libgcc linked in, so the AppImage needs only the build machine's glibc
  # or newer (the zig cross build links libc++ statically and targets glibc 2.28)
  if(NOT CMAKE_CROSSCOMPILING)
    target_link_options(f1ce PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()
