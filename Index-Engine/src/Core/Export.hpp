#pragma once

// Public feature/export contract for Index core consumers:
// - INDEX_API / INDEX_*_API describe symbol visibility
// - INDEX_WITH_* describes which optional modules the current translation unit opted into
// - INDEX_CORE_ONLY is true when no optional module flags are enabled

#if defined(IDX_PLATFORM_WINDOWS)
#if defined(IDX_BUILD_DLL)
#define INDEX_API __declspec(dllexport)
#elif defined(IDX_IMPORT_DLL)
#define INDEX_API __declspec(dllimport)
#else
#define INDEX_API
#endif
#else
#define INDEX_API
#endif

#if defined(INDEX_ALL_MODULES)
#undef INDEX_WITH_RENDER
#undef INDEX_WITH_AUDIO
#undef INDEX_WITH_PHYSICS
#undef INDEX_WITH_SCRIPTING
#undef INDEX_WITH_EDITOR
#undef INDEX_WITH_APPLICATION
#define INDEX_WITH_RENDER 1
#define INDEX_WITH_AUDIO 1
#define INDEX_WITH_PHYSICS 1
#define INDEX_WITH_SCRIPTING 1
#define INDEX_WITH_EDITOR 1
#define INDEX_WITH_APPLICATION 1
#endif

#ifndef INDEX_WITH_RENDER
#define INDEX_WITH_RENDER 0
#endif

#ifndef INDEX_WITH_AUDIO
#define INDEX_WITH_AUDIO 0
#endif

#ifndef INDEX_WITH_PHYSICS
#define INDEX_WITH_PHYSICS 0
#endif

#ifndef INDEX_WITH_SCRIPTING
#define INDEX_WITH_SCRIPTING 0
#endif

#ifndef INDEX_WITH_EDITOR
#define INDEX_WITH_EDITOR 0
#endif

#ifndef INDEX_WITH_APPLICATION
#define INDEX_WITH_APPLICATION 0
#endif

#if !INDEX_WITH_RENDER && !INDEX_WITH_AUDIO && !INDEX_WITH_PHYSICS && !INDEX_WITH_SCRIPTING && !INDEX_WITH_EDITOR
#define INDEX_CORE_ONLY 1
#else
#define INDEX_CORE_ONLY 0
#endif

#define INDEX_CORE_API INDEX_API
#define INDEX_RENDER_API INDEX_API
#define INDEX_AUDIO_API INDEX_API
#define INDEX_PHYSICS_API INDEX_API
#define INDEX_SCRIPTING_API INDEX_API
#define INDEX_EDITOR_API INDEX_API
