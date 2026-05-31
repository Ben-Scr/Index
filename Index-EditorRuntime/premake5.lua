group "Runtime"
project "Index-EditorRuntime"
    location "."
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    cdialect "C17"
    staticruntime "off"
    warnings "Extra"

    targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "src/**.cpp",
        "src/**.h",
        "src/**.hpp"
    }

    -- Pulls Index-Engine + ImGui + GLFW so this lib's TUs can see the engine
    -- headers (Core/Application.hpp, Core/Window.hpp, Gui/ImGuiImplWebGPU.hpp,
    -- Packages/PackageImGuiBridge.hpp) and the imgui headers used by the
    -- titlebar and context-setup code. The consumer .exe (Launcher / Editor /
    -- Runtime / Sandbox) resolves the actual link-time symbols via its own
    -- UseDependencySet(EditorRuntimeCommon) — webgpu_dawn / nethost / etc.
    -- are pulled in there during link.exe.
    --
    -- Use the *NoLink* variant deliberately: MSVC's lib.exe ARCHIVES every
    -- input .lib's object members into the output, so if links() ran here
    -- the ~185 MB webgpu_dawn.lib (and the 1.36 GB Debug variant) would be
    -- merged wholesale into Index-EditorRuntime.lib. We need only the
    -- includes / defines / build-order from the dep set.
    UseDependencySetNoLink(Dependency.EngineSelectedModules)
    defines(GetIndexModuleDefines())

    -- INDEX_API resolves to __declspec(dllimport) here so this lib's TUs
    -- agree with the consumer .exe's view of engine.dll symbols.
    defines { "IDX_IMPORT_DLL" }

    includedirs { "src" }
    includedirs { IndexEntityBitsConfigIncludeDir }

    filter "system:windows"
        flags { "MultiProcessorCompile" }
        buildoptions { "/utf-8", "/FS", "/Zc:preprocessor" }
        systemversion "latest"
        defines { "IDX_PLATFORM_WINDOWS" }

    filter "system:linux"
        defines { "IDX_PLATFORM_LINUX" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"
        defines { "IDX_DEBUG", "_DEBUG" }

    filter "configurations:Release"
        runtime "Release"
        optimize "On"
        symbols "On"
        defines { "IDX_RELEASE", "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "Full"
        symbols "Off"
        defines { "IDX_DIST", "NDEBUG" }

    filter {}

    -- No ApplyDawnLibDirs call: this StaticLib intentionally does not link
    -- (see UseDependencySetNoLink above), so it has no use for Dawn's per-
    -- config libdirs. The consumer .exe applies ApplyDawnLibDirs itself.
