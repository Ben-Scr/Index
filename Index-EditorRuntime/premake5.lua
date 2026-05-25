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
    -- titlebar and context-setup code. As a StaticLib we don't actually link
    -- anything — the final consumer .exe (Launcher / Editor) resolves the
    -- symbols. DependsOn is what we need; the Links list rides along
    -- harmlessly because StaticLib kind ignores it.
    UseDependencySet(Dependency.EngineSelectedModules)
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

    -- Per-config libdirs for webgpu_dawn.lib (inherited via EngineCore-
    -- Render's Links). Premake propagates the dep set's Links list into
    -- this StaticLib's <Lib>/AdditionalDependencies, so the libdirs need
    -- to be set per-config the same way the editor / launcher do — see
    -- ApplyDawnLibDirs in the root premake5.lua for the LNK2038
    -- runtime-mismatch rationale.
    ApplyDawnLibDirs("../")
