@echo off
rem Build speak.exe with the MSVC build tools + their bundled CMake/Ninja.
setlocal
set VSBT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set CMAKE=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

if not exist "%CMAKE%" (
    echo Could not find CMake at "%CMAKE%".
    echo Install Visual Studio 2022 Build Tools with the C++ workload, or edit VSBT above.
    exit /b 1
)

call "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"
"%CMAKE%" -B .build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="%NINJA%" || exit /b 1
"%CMAKE%" --build .build || exit /b 1
echo BUILD_OK
