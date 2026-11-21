cmake_minimum_required(VERSION 3.20)

# WildWaters PC host. RecompiledFuncs/ must exist first (build.cmd runs the recompilers).
# N64_TOOLS points at the folder with N64ModernRuntime, rt64 and RecompFrontend checkouts.

project(WildWatersPC C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

option(WW_RT64 "Render with RT64 in an SDL window (OFF: headless test build)" ON)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
if (NOT N64_TOOLS)
    set(N64_TOOLS "${REPO_ROOT}/../../Tools/N64 tools")
endif()
get_filename_component(N64_TOOLS "${N64_TOOLS}" ABSOLUTE)
set(RT64_DIR "${N64_TOOLS}/rt64")

add_subdirectory("${N64_TOOLS}/N64ModernRuntime" "${CMAKE_CURRENT_BINARY_DIR}/N64ModernRuntime")
if (MSVC)
    # Their GCC-style -Wno-unused-parameter is a command-line error for cl.
    set_property(TARGET ultramodern librecomp PROPERTY COMPILE_OPTIONS "")
    # MSVC 14.5x hits an internal compiler error optimising librecomp's flash.cpp.
    set_source_files_properties("${N64_TOOLS}/N64ModernRuntime/librecomp/src/flash.cpp"
        DIRECTORY "${N64_TOOLS}/N64ModernRuntime/librecomp"
        PROPERTIES COMPILE_OPTIONS "/Od")
endif()

file(GLOB RECOMPILED_FUNCS CONFIGURE_DEPENDS "${REPO_ROOT}/RecompiledFuncs/funcs_*.c")
add_library(RecompiledFuncs STATIC ${RECOMPILED_FUNCS})
target_include_directories(RecompiledFuncs PRIVATE
    "${N64_TOOLS}/N64ModernRuntime/N64Recomp/include"
    "${REPO_ROOT}/RecompiledFuncs")
if (MSVC)
    target_compile_options(RecompiledFuncs PRIVATE /W0 /bigobj)
endif()

add_executable(WildWatersPC
    src/main.cpp
    src/null_renderer.cpp
    src/asset_capture.cpp
    src/restored_events.cpp
    src/ultra_extras.cpp
    src/overlays.cpp
    src/input_script.cpp
    src/music.cpp
    src/restore.cpp
    src/frame_limit.cpp
    src/audio_dump.cpp
    "${REPO_ROOT}/RecompiledFuncs/rsp/aspMain.cpp")
if (MSVC)
    set_source_files_properties("${REPO_ROOT}/RecompiledFuncs/rsp/aspMain.cpp" PROPERTIES COMPILE_OPTIONS "/W0;/bigobj")
endif()
target_include_directories(WildWatersPC PRIVATE
    "${N64_TOOLS}/N64ModernRuntime/N64Recomp/include"
    "${REPO_ROOT}/RecompiledFuncs")
target_link_libraries(WildWatersPC PRIVATE RecompiledFuncs librecomp ultramodern)

if (WW_RT64)
    set(RT64_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${RT64_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/rt64")

    set(sdl2_SOURCE_DIR "${RT64_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3")
    set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH "${N64_TOOLS}/N64ModernRuntime")
    set(RECOMP_FRONTEND_RT64_PATH "${RT64_DIR}")
    # recompui compiles its shaders with rt64's build_*_shader functions, which read
    # these variables from rt64's directory scope: repeat them here.
    set(DXC_COMMON_OPTS "-I${RT64_DIR}/src")
    set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
    set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
    set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
    set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
    set(DXC_ARCH x64)
    set(DXC "${RT64_DIR}/src/contrib/dxc/bin/x64/dxc.exe")
    add_subdirectory("${N64_TOOLS}/RecompFrontend" "${CMAKE_CURRENT_BINARY_DIR}/RecompFrontend")
    if (MSVC)
        set_property(TARGET recompui recompinput PROPERTY COMPILE_OPTIONS "")
    endif()

    target_sources(WildWatersPC PRIVATE src/frontend.cpp src/config.cpp src/audio_output.cpp src/profiler.cpp)
    target_link_libraries(WildWatersPC PRIVATE recompui recompinput nfd rt64)
    target_compile_definitions(WildWatersPC PRIVATE WW_RT64 HLSL_CPU NOMINMAX)
    target_include_directories(WildWatersPC PRIVATE
        "${RT64_DIR}/src"
        "${RT64_DIR}/src/contrib"
        "${RT64_DIR}/src/contrib/plume"
        "${RT64_DIR}/src/contrib/imgui"
        "${RT64_DIR}/src/contrib/hlslpp/include"
        "${RT64_DIR}/src/contrib/mupen64plus-core/src/api"
        "${RT64_DIR}/src/contrib/nativefiledialog-extended/src/include")

    set(SDL2_DIR "${RT64_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3")
    target_include_directories(WildWatersPC PRIVATE "${SDL2_DIR}/include" "${RT64_DIR}/src/contrib/dxc/inc")
    target_link_directories(WildWatersPC PRIVATE "${SDL2_DIR}/lib/x64")
    target_link_libraries(WildWatersPC PRIVATE SDL2)
    add_custom_command(TARGET WildWatersPC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL2_DIR}/lib/x64/SDL2.dll"
            "${RT64_DIR}/src/contrib/dxc/bin/x64/dxcompiler.dll"
            "${RT64_DIR}/src/contrib/dxc/bin/x64/dxil.dll"
            "$<TARGET_FILE_DIR:WildWatersPC>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:WildWatersPC>/assets")
endif()

# Restored data (both builds).
add_custom_command(TARGET WildWatersPC POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets/restore" "$<TARGET_FILE_DIR:WildWatersPC>/assets/restore")

if (WIN32)
    target_link_libraries(WildWatersPC PRIVATE DbgHelp)
    target_link_options(WildWatersPC PRIVATE /OPT:NOICF)
    if (WW_RT64)
        # The window build is a Windows program: no console window next to the game. Its log still
        # reaches redirected output (test scripts); WW_CONSOLE=1 opens a console (host/src/main.cpp).
        set_target_properties(WildWatersPC PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_options(WildWatersPC PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()
