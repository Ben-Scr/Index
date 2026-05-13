cmake_minimum_required(VERSION 3.20)
project(Index-NativeScripts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Dist" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_FLAGS_DIST "${CMAKE_CXX_FLAGS_RELEASE}" CACHE STRING "Flags used by the C++ compiler for Dist builds." FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DIST "${CMAKE_SHARED_LINKER_FLAGS_RELEASE}" CACHE STRING "Flags used by the shared linker for Dist builds." FORCE)

function(idx_normalize_native_arch out_var raw_arch)
    string(TOLOWER "${raw_arch}" raw_arch_lower)
    if(raw_arch_lower STREQUAL "amd64" OR raw_arch_lower STREQUAL "x86_64" OR raw_arch_lower STREQUAL "x64")
        set(${out_var} "x86_64" PARENT_SCOPE)
    elseif(raw_arch_lower STREQUAL "arm64" OR raw_arch_lower STREQUAL "aarch64")
        set(${out_var} "arm64" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "Unsupported native script architecture: ${raw_arch}")
    endif()
endfunction()

set(INDEX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

file(GLOB_RECURSE NATIVE_SCRIPT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.hpp"
)

add_library(Index-NativeScripts SHARED ${NATIVE_SCRIPT_SOURCES})

# Native scripts include the engine public headers for IntelliSense and template APIs.
# Non-inline runtime calls should still go through NativeEngineAPI or exported bindings.
target_include_directories(Index-NativeScripts PRIVATE
    "${INDEX_DIR}/Index-Engine/src"
    "${INDEX_DIR}/External/spdlog/include"
    "${INDEX_DIR}/External/glm"
    "${INDEX_DIR}/External/entt/src"
    "${INDEX_DIR}/External/stb"
    "${INDEX_DIR}/External/magic_enum/include"
    "${INDEX_DIR}/External/cereal/include"
    "${INDEX_DIR}/External/glfw/include"
    "${INDEX_DIR}/External/glad/include"
    "${INDEX_DIR}/External/miniaudio"
    "${INDEX_DIR}/External/box2d/include"
    "${INDEX_DIR}/External/Axiom-Physics/include"
    "${INDEX_DIR}/External/dotnet"
)

target_compile_definitions(Index-NativeScripts PRIVATE
    INDEX_ALL_MODULES=1
    $<$<CONFIG:Debug>:IDX_DEBUG;_DEBUG>
    $<$<CONFIG:Release>:IDX_RELEASE;NDEBUG>
    $<$<CONFIG:Dist>:IDX_DIST;NDEBUG>
)

if(WIN32)
    idx_normalize_native_arch(IDX_NATIVE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    set(IDX_NATIVE_PLATFORM "windows-${IDX_NATIVE_ARCH}")
    target_compile_definitions(Index-NativeScripts PRIVATE IDX_PLATFORM_WINDOWS)
elseif(UNIX AND NOT APPLE)
    idx_normalize_native_arch(IDX_NATIVE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    set(IDX_NATIVE_PLATFORM "linux-${IDX_NATIVE_ARCH}")
    target_compile_definitions(Index-NativeScripts PRIVATE IDX_PLATFORM_LINUX)
else()
    message(FATAL_ERROR "Unsupported platform for native scripts")
endif()

target_link_directories(Index-NativeScripts PRIVATE
    "$<$<CONFIG:Debug>:${INDEX_DIR}/bin/Debug-${IDX_NATIVE_PLATFORM}/Index-Engine>"
    "$<$<CONFIG:Release>:${INDEX_DIR}/bin/Release-${IDX_NATIVE_PLATFORM}/Index-Engine>"
    "$<$<CONFIG:Dist>:${INDEX_DIR}/bin/Dist-${IDX_NATIVE_PLATFORM}/Index-Engine>"
)
target_link_libraries(Index-NativeScripts PRIVATE Index-Engine)

if(MSVC)
    target_compile_options(Index-NativeScripts PRIVATE /utf-8)
endif()

set(ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
foreach(config Debug Release Dist)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(Index-NativeScripts PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${ENGINE_ROOT}/bin/${config}-${IDX_NATIVE_PLATFORM}/Index-NativeScripts"
        LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${ENGINE_ROOT}/bin/${config}-${IDX_NATIVE_PLATFORM}/Index-NativeScripts"
    )
endforeach()
