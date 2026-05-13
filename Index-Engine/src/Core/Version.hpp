#pragma once

#define IDX_VERSION "2026.1.0"

//
// Build Configuration
//
#if defined(IDX_DEBUG)
#define IDX_BUILD_CONFIG_NAME "Debug"
#elif defined(IDX_RELEASE)
#define IDX_BUILD_CONFIG_NAME "Release"
#elif defined(IDX_DIST)
#define IDX_BUILD_CONFIG_NAME "Dist"
#else
#error Undefined configuration?
#endif

//
// Build Platform
//
#if defined(IDX_PLATFORM_WINDOWS)
#define IDX_BUILD_PLATFORM_NAME "Windows x64"
#elif defined(IDX_PLATFORM_LINUX)
#define IDX_BUILD_PLATFORM_NAME "Linux x64"
#else
#error Unsupported Platform!
#endif

#define IDX_VERSION_LONG "Index " IDX_VERSION " (" IDX_BUILD_PLATFORM_NAME " " IDX_BUILD_CONFIG_NAME ")"