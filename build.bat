@echo off
rem Build speak.exe with the MSVC build tools + their bundled CMake/Ninja.
rem
rem Finds Visual Studio in this order:
rem   1. %SPEAK_VSBT%            explicit override (CI, or an unusual install)
rem   2. vswhere                 any 2022+ edition with the C++ workload
rem   3. the default Build Tools path
setlocal

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe

if defined SPEAK_VSBT (
    set VSBT=%SPEAK_VSBT%
) else (
    set VSBT=
    if exist "%VSWHERE%" (
        for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
            -property installationPath`) do set VSBT=%%i
    )
    if not defined VSBT set VSBT=C:\Program Files ^(x86^)\Microsoft Visual Studio\2022\BuildTools
)

set CMAKE=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

rem Fall back to whatever is on PATH — GitHub runners ship both.
if not exist "%CMAKE%" for /f "delims=" %%i in ('where cmake 2^>nul') do set CMAKE=%%i
if not exist "%NINJA%" for /f "delims=" %%i in ('where ninja 2^>nul') do set NINJA=%%i

if not exist "%CMAKE%" (
    echo Could not find CMake at "%CMAKE%".
    echo Install Visual Studio 2022 Build Tools with the C++ workload,
    echo or set SPEAK_VSBT to your Visual Studio installation path.
    exit /b 1
)
if not exist "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo Could not find vcvars64.bat under "%VSBT%".
    echo Set SPEAK_VSBT to your Visual Studio installation path.
    exit /b 1
)

echo Using Visual Studio: %VSBT%
call "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"
"%CMAKE%" -B .build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="%NINJA%" || exit /b 1
"%CMAKE%" --build .build || exit /b 1
echo BUILD_OK
