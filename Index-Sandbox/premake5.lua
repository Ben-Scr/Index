group "Game"
project "Index-Sandbox"
    location "."
    kind "SharedLib"
    language "C#"
    dotnetframework "net9.0"
    clr "Unsafe"

    targetdir ("../bin/" .. outputdir .. "/%{prj.name}")
    objdir ("../bin-int/" .. outputdir .. "/%{prj.name}")

    vsprops {
        AppendTargetFrameworkToOutputPath = "false",
        Nullable = "enable",
        AllowUnsafeBlocks = "true",
        EnableDynamicLoading = "true",
    }

    files {
        "Source/**.cs"
    }

    links {
        "Index-ScriptCore",
    }

    -- Engine packages the Sandbox preset opts into. Each is linked only when
    -- the package is actually installed (its manifest is present under
    -- packages/<Name>/). A checkout without it — CI, a fresh clone, or a user
    -- who hasn't fetched it from the registry — then builds clean instead of
    -- emitting MSB3245 for an unresolvable Pkg.<Name> assembly reference.
    -- Per the engine's "no mandatory dependencies" rule every entry is
    -- removable; `Index.Hardware.Cpu` etc. simply won't resolve without it.
    local optionalPackages = { "Index.Hardware" }
    for _, pkg in ipairs(optionalPackages) do
        if IndexEnginePackageInstalled(pkg) then
            links { "Pkg." .. pkg }
        else
            print("[Index-Sandbox] Optional package '" .. pkg ..
                "' not installed; skipping its Pkg." .. pkg .. " reference.")
        end
    end

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        optimize "Off"
        symbols "On"
        defines { "IDX_DEBUG" }

    filter "configurations:Release"
        optimize "On"
        symbols "On"
        defines { "IDX_RELEASE" }

    filter "configurations:Dist"
        optimize "Full"
        symbols "Off"
        defines { "IDX_DIST" }
