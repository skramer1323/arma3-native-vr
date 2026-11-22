cmake_minimum_required(VERSION 3.24)

project(Arma3NativeVR VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
set(BUILD_SPECIFICATION OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(INSTALL_SYMBOLS OFF CACHE BOOL "" FORCE)

# Pin dependencies so builds use the same source revisions.
FetchContent_Declare(
    openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK-Source.git
    GIT_TAG c07ad64839653712190e05dbd8cf460e1d239513
    GIT_SUBMODULES ""
)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(openxr minhook)

# OpenXR sets its static loader's CRT explicitly. Match the parent when it
# selects a CRT so the loader and the other objects can link into one DLL.
if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
    set_property(TARGET openxr_loader PROPERTY MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
endif()

# Dear ImGui (MIT) for the in-game settings menu.
add_library(imgui_lib STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC third_party/imgui third_party/imgui/backends)

set(ARMA_DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK checkout")
option(ARMA_WITH_DLSS "Build the experimental DLSS path" OFF)

add_library(Arma3NativeVR SHARED
    src/DlssBalanced.cpp
    src/DlssWorld.cpp
    src/Calibration.cpp
    src/Calibration.h
    src/Config.cpp
    src/CursorImage.cpp
    src/Config.h
    src/DepthStereo.cpp
    src/DepthStereo.h
    src/LensMask.cpp
    src/LensMask.h
    src/Menu.cpp
    src/Menu.h
    src/OpenXRMenu.cpp
    src/InputBlock.cpp
    src/InputBlock.h
    src/ResolutionHook.cpp
    src/ResolutionHook.h
    src/Dibr.cpp
    src/Dibr.h
    src/DInputProxy.cpp
    src/BodyMask.cpp
    src/EngineStereo.cpp
    src/WorldShadingRate.cpp
    src/EngineStereo.h
    src/FrameProbe.cpp
    src/Perf.cpp
    src/PerfPasses.cpp
    src/ProjectionCache.cpp
    src/StaleEyeWarp.cpp
    src/FrameProbe.h
    src/HeadTracking.cpp
    src/HeadTracking.h
    src/Hooks.cpp
    src/Hooks.h
    src/Log.cpp
    src/Log.h
    src/OpenXRRuntime.cpp
    src/OpenXRRuntime.h
    src/QuadMath.h
    src/dinput8.def
)

target_compile_definitions(Arma3NativeVR PRIVATE
    ARMA_NATIVE_VR_VERSION="${PROJECT_VERSION}"
    DIRECTINPUT_VERSION=0x0800
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    XR_USE_PLATFORM_WIN32
    XR_USE_GRAPHICS_API_D3D11
)

if(MSVC)
    target_compile_options(Arma3NativeVR PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endif()

target_link_libraries(Arma3NativeVR PRIVATE imgui_lib minhook openxr_loader d3d11 dxgi d3dcompiler dxguid windowscodecs ole32 oleaut32 uuid
    user32 advapi32 crypt32 wintrust shlwapi)
if(ARMA_WITH_DLSS)
    if(NOT EXISTS "${ARMA_DLSS_SDK_ROOT}/include/nvsdk_ngx_helpers.h" OR
       NOT EXISTS "${ARMA_DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
        message(FATAL_ERROR "Set ARMA_DLSS_SDK_ROOT to the official NVIDIA DLSS SDK checkout")
    endif()
    target_compile_definitions(Arma3NativeVR PRIVATE ARMA_HAS_DLSS)
    target_include_directories(Arma3NativeVR SYSTEM PRIVATE "${ARMA_DLSS_SDK_ROOT}/include")
    target_link_libraries(Arma3NativeVR PRIVATE "${ARMA_DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
endif()

set_target_properties(Arma3NativeVR PROPERTIES
    OUTPUT_NAME dinput8
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist"
)

add_custom_command(TARGET Arma3NativeVR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/arma3_native_vr.ini"
        "$<TARGET_FILE_DIR:Arma3NativeVR>/arma3_native_vr.ini"
)

# FreeTrack client the game loads for head look (HeadTracking=1); lives next to dinput8.dll.
add_library(Arma3NativeVRFreeTrack SHARED src/FreeTrackClient.cpp src/HeadTracking.h)
target_compile_definitions(Arma3NativeVRFreeTrack PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(Arma3NativeVRFreeTrack PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endif()
set_target_properties(Arma3NativeVRFreeTrack PROPERTIES
    OUTPUT_NAME FreeTrackClient64
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist"
)

# Unit tests for the pure helpers (no game, no OpenXR runtime).
add_executable(Arma3NativeVRTests tests/QuadMathTests.cpp)
target_include_directories(Arma3NativeVRTests PRIVATE src)
if(MSVC)
    target_compile_options(Arma3NativeVRTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
endif()
set_target_properties(Arma3NativeVRTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests"
)
enable_testing()
add_test(NAME QuadMath COMMAND Arma3NativeVRTests)
