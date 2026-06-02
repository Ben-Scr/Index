@echo off
setlocal enabledelayedexpansion
pushd %~dp0
echo ============================================
echo   Index Engine Setup
echo ============================================
echo.

REM Pre-approve installs via -y / --assume-yes / --yes, or INDEX_SETUP_ASSUME_YES=1.
set "ASSUME_YES="
if /i "%INDEX_SETUP_ASSUME_YES%"=="1" set "ASSUME_YES=1"
echo %* | findstr /i /c:"--assume-yes" /c:"--yes" /c:"-y" >nul && set "ASSUME_YES=1"

call :find_python
if defined PYTHON goto have_python

REM Python 3.10+ missing -> offer to install it (the version check below needs
REM Python, so this prompt has to live here rather than in Setup.py).
call :offer_python_install
call :find_python
if defined PYTHON goto have_python

echo.
echo [ERROR] Python 3.10+ is still not available.
echo         Download it from https://www.python.org/downloads/
PAUSE
exit /b 1

:have_python
echo [Index Setup] Using Python: %PYTHON_VERSION%
%PYTHON% Setup.py %*
set "EXITCODE=%ERRORLEVEL%"
PAUSE
exit /b %EXITCODE%

REM ---------------------------------------------------------------------------
REM Locate a Python 3.10+ interpreter. Tries the launcher / PATH first, then
REM well-known install directories so a freshly winget-installed Python is
REM picked up without reopening the shell. Sets PYTHON + PYTHON_VERSION.
:find_python
set "PYTHON="
set "PYTHON_VERSION="
py -3.10 --version >nul 2>&1 && set "PYTHON=py -3.10"
if not defined PYTHON (
    py -3 --version >nul 2>&1 && set "PYTHON=py -3"
)
if not defined PYTHON (
    python --version >nul 2>&1 && set "PYTHON=python"
)
if not defined PYTHON (
    for /d %%D in ("%LocalAppData%\Programs\Python\Python3*") do (
        if exist "%%D\python.exe" set "PYTHON="%%D\python.exe""
    )
)
if not defined PYTHON (
    for /d %%D in ("%ProgramFiles%\Python3*") do (
        if exist "%%D\python.exe" set "PYTHON="%%D\python.exe""
    )
)
if not defined PYTHON goto :eof
REM Enforce the 3.10+ minimum; clear PYTHON if the found interpreter is older.
for /f "delims=" %%i in ('%PYTHON% --version 2^>^&1') do set "PYTHON_VERSION=%%i"
%PYTHON% -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>&1
if errorlevel 1 set "PYTHON="
goto :eof

REM ---------------------------------------------------------------------------
:offer_python_install
echo [Index Setup] Python 3.10+ was not found.
if defined PYTHON_VERSION echo [Index Setup] Closest match: %PYTHON_VERSION%
where winget >nul 2>&1
if errorlevel 1 (
    echo [Index Setup] winget is unavailable, so Python cannot be installed automatically.
    echo [Index Setup] Install it from https://www.python.org/downloads/ and re-run setup.
    goto :eof
)
set "DOINSTALL="
if defined ASSUME_YES (
    set "DOINSTALL=1"
) else (
    set /p "ANSWER=[Index Setup] Install Python 3.12 via winget now? [Y/n] "
    if /i "!ANSWER!"=="" set "DOINSTALL=1"
    if /i "!ANSWER!"=="y" set "DOINSTALL=1"
    if /i "!ANSWER!"=="yes" set "DOINSTALL=1"
)
if not defined DOINSTALL (
    echo [Index Setup] Skipping Python installation.
    goto :eof
)
echo [Index Setup] Installing Python 3.12 via winget...
winget install --exact --id Python.Python.3.12 --accept-package-agreements --accept-source-agreements --disable-interactivity
goto :eof
