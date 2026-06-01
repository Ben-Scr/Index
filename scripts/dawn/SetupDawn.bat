@echo off
REM ============================================================================
REM SetupDawn.bat - Fetch and build the Dawn WebGPU implementation as a
REM                monolithic static library at External\dawn\, then
REM                regenerate Index.sln so the engine is build-ready.
REM
REM Run once after cloning the engine. Subsequent engine builds link against
REM the pre-built webgpu_dawn.lib.
REM
REM Prerequisites:
REM   * Git (with HTTPS access to dawn.googlesource.com)
REM   * Visual Studio 2022 (any edition with C++ workload)
REM   * Python 3 (Dawn's CMake invokes Python for code generation)
REM   * CMake -- either at vendor\cmake\bin\cmake.exe (vendored portable;
REM     run scripts\dawn\BootstrapCMake.bat to fetch it) or on PATH.
REM ============================================================================

setlocal EnableDelayedExpansion

pushd "%~dp0\..\.."

set "DAWN_DIR=External\dawn"
set "DAWN_BUILD_DIR=%DAWN_DIR%\build"
set "VENDOR_CMAKE_EXE=vendor\cmake\bin\cmake.exe"

echo ============================================
echo   Dawn WebGPU Setup
echo ============================================
echo Target: %DAWN_DIR%
echo.

REM --- Tool checks ------------------------------------------------------------
where git >nul 2>nul
if errorlevel 1 (
    echo [ERROR] git is not on PATH.
    goto :error
)

REM CMake resolution: prefer vendored portable, then PATH.
set "CMAKE_EXE="
if exist "%VENDOR_CMAKE_EXE%" set "CMAKE_EXE=%VENDOR_CMAKE_EXE%"
if not defined CMAKE_EXE (
    where cmake >nul 2>nul
    if not errorlevel 1 set "CMAKE_EXE=cmake"
)
if not defined CMAKE_EXE (
    echo [ERROR] CMake not found at %VENDOR_CMAKE_EXE% nor on PATH.
    echo         Run scripts\dawn\BootstrapCMake.bat to vendor a portable copy,
    echo         or install CMake yourself.
    goto :error
)
echo [info] Using cmake: !CMAKE_EXE!

where python >nul 2>nul
if not errorlevel 1 goto :python_ok
where py >nul 2>nul
if not errorlevel 1 goto :python_ok
echo [ERROR] python / py not on PATH. Dawn's codegen needs Python 3.
goto :error
:python_ok

REM --- Resolve pinned revision ------------------------------------------------
REM DAWN_REVISION (env) wins; otherwise read scripts\dawn\dawn-revision.txt if
REM present; otherwise stay empty and clone tip-of-main (the original behaviour
REM for local devs who want the latest). CI relies on the revision file so the
REM build is reproducible and the Dawn cache key stays stable.
if not defined DAWN_REVISION (
    if exist "scripts\dawn\dawn-revision.txt" (
        for /f "usebackq tokens=1 delims= " %%R in ("scripts\dawn\dawn-revision.txt") do (
            if not defined DAWN_REVISION set "DAWN_REVISION=%%R"
        )
    )
)

REM --- Clone ------------------------------------------------------------------
if exist "%DAWN_DIR%\CMakeLists.txt" goto :clone_done
if defined DAWN_REVISION (
    echo [1/5] Fetching Dawn at pinned revision !DAWN_REVISION! ^(this may take a few minutes^)...
    git init "%DAWN_DIR%"
    if errorlevel 1 (
        echo [ERROR] git init failed.
        goto :error
    )
    REM remote add fails harmlessly if origin already exists from a prior run.
    git -C "%DAWN_DIR%" remote add origin https://dawn.googlesource.com/dawn 2>nul
    git -C "%DAWN_DIR%" fetch --depth=1 origin !DAWN_REVISION!
    if errorlevel 1 (
        echo [ERROR] git fetch of pinned revision !DAWN_REVISION! failed.
        echo         Check the SHA in scripts\dawn\dawn-revision.txt is reachable on dawn.googlesource.com.
        goto :error
    )
    git -C "%DAWN_DIR%" checkout --detach FETCH_HEAD
    if errorlevel 1 (
        echo [ERROR] git checkout of pinned revision failed.
        goto :error
    )
) else (
    echo [1/5] Cloning Dawn ^(tip of main; this may take a few minutes^)...
    git clone --depth=1 https://dawn.googlesource.com/dawn "%DAWN_DIR%"
    if errorlevel 1 (
        echo [ERROR] git clone failed.
        goto :error
    )
)
goto :fetch_deps
:clone_done
echo [1/5] Dawn already present at %DAWN_DIR% ^(skipping clone^).

REM --- Fetch trimmed deps -----------------------------------------------------
REM Dawn's stock fetcher (invoked by -DDAWN_FETCH_DEPENDENCIES=ON) pulls 18
REM submodules with a hardcoded list. We pre-fetch only the 16 we need via
REM our wrapper, then leave DAWN_FETCH_DEPENDENCIES off so CMake doesn't
REM re-pull glfw3 / googletest / google_benchmark.
:fetch_deps
echo [2/5] Fetching Dawn third-party deps ^(trimmed list^)...
where python >nul 2>nul
if not errorlevel 1 (
    python "scripts\dawn\FetchDawnDepsMinimal.py"
) else (
    py "scripts\dawn\FetchDawnDepsMinimal.py"
)
if errorlevel 1 (
    echo [ERROR] Dep fetch failed.
    goto :error
)

REM --- Configure --------------------------------------------------------------
:configure
echo [3/5] Configuring Dawn via CMake...
"!CMAKE_EXE!" -S "%DAWN_DIR%" -B "%DAWN_BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC -DDAWN_ENABLE_INSTALL=OFF -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_USE_GLFW=OFF -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF
if errorlevel 1 (
    echo [ERROR] CMake configure failed.
    goto :error
)

REM --- Build ------------------------------------------------------------------
REM DAWN_BUILD_CONFIGS (env) selects which configs to build; defaults to both
REM for local dev (the editor's Debug build links the Debug lib). CI sets it to
REM "Release" only -- the Windows CI job builds the solution in Release, so the
REM Debug lib (~1.36 GB, slow to build) is dead weight there.
if not defined DAWN_BUILD_CONFIGS set "DAWN_BUILD_CONFIGS=Debug Release"
echo [4/5] Building webgpu_dawn ^(configs: !DAWN_BUILD_CONFIGS!^)...
for %%C in (!DAWN_BUILD_CONFIGS!) do (
    echo     - building %%C...
    "!CMAKE_EXE!" --build "%DAWN_BUILD_DIR%" --config %%C --target webgpu_dawn
    if errorlevel 1 (
        echo [ERROR] %%C build failed.
        goto :error
    )
)

REM --- Regenerate Index.sln ---------------------------------------------------
REM CI sets DAWN_SKIP_PREMAKE=1: the workflow regenerates project files itself
REM (scripts/Setup.py) with the correct module-profile / options right after
REM this, so a redundant default-option premake run here is skipped.
if defined DAWN_SKIP_PREMAKE (
    echo [5/5] DAWN_SKIP_PREMAKE set - skipping premake regen.
    goto :done
)
echo [5/5] Regenerating Index.sln via Premake...
if not exist "vendor\bin\premake5.exe" goto :no_premake
"vendor\bin\premake5.exe" vs2022
if errorlevel 1 echo [WARN] Premake regen returned non-zero. Run manually: vendor\bin\premake5.exe vs2022
goto :done
:no_premake
echo [WARN] vendor\bin\premake5.exe not found. Run scripts\Setup.bat first.

:done
echo.
echo ============================================
echo   Dawn build complete.
echo ============================================
echo Headers : %DAWN_DIR%\include
echo Headers : %DAWN_BUILD_DIR%\gen\include
echo Lib(D)  : %DAWN_BUILD_DIR%\src\dawn\native\Debug\webgpu_dawn.lib
echo Lib(R)  : %DAWN_BUILD_DIR%\src\dawn\native\Release\webgpu_dawn.lib
echo.
echo Open Index.sln in Visual Studio 2022 and build.
echo.

popd
exit /b 0

:error
popd
echo.
echo Setup aborted.
exit /b 1
