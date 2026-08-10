@echo off
setlocal EnableExtensions

for %%I in ("%~dp0..") do set "VILLIEN_ROOT=%%~fI"
set "VILLIEN_BUILD=%VILLIEN_ROOT%\build-codex"
set "VILLIEN_EDITOR=%VILLIEN_BUILD%\bin\Debug\VillienEditor.exe"

if not exist "%VILLIEN_ROOT%\CMakeLists.txt" goto :missing_root

set "CMAKE_EXE=cmake"
where cmake >nul 2>&1
if not errorlevel 1 goto :cmake_ready

set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if exist "%CMAKE_EXE%" goto :cmake_ready

echo [VILLIEN Editor] CMake was not found.
echo Install CMake or Visual Studio 2026 with the Desktop development with C++ workload.
echo Then run this launcher again.
pause
exit /b 1

:cmake_ready
if exist "%VILLIEN_BUILD%\CMakeCache.txt" goto :build_editor

echo [VILLIEN Editor] Configuring the root project...
"%CMAKE_EXE%" -S "%VILLIEN_ROOT%" -B "%VILLIEN_BUILD%" -G "Visual Studio 18 2026" -A x64
if errorlevel 1 goto :configure_failed

:build_editor
echo [VILLIEN Editor] Building the Debug editor target...
"%CMAKE_EXE%" --build "%VILLIEN_BUILD%" --config Debug --target VillienEditor -j
if errorlevel 1 goto :build_failed

if not exist "%VILLIEN_EDITOR%" goto :missing_editor

echo [VILLIEN Editor] Starting from the project root...
start "VILLIEN Editor" /D "%VILLIEN_ROOT%" "%VILLIEN_EDITOR%" --editor
if errorlevel 1 goto :launch_failed
exit /b 0

:missing_root
echo [VILLIEN Editor] The project root could not be found next to featuretools.
echo Expected: "%VILLIEN_ROOT%\CMakeLists.txt"
pause
exit /b 1

:configure_failed
echo.
echo [VILLIEN Editor] Root project configuration failed.
echo Review the CMake error above, then delete only "%VILLIEN_BUILD%" if the cache uses the wrong generator.
pause
exit /b 1

:build_failed
echo.
echo [VILLIEN Editor] The VillienEditor Debug build failed.
echo Fix the compiler error shown above and run this launcher again.
pause
exit /b 1

:missing_editor
echo [VILLIEN Editor] The build reported success, but the executable was not found.
echo Expected: "%VILLIEN_EDITOR%"
echo Check the target output settings in the root CMakeLists.txt.
pause
exit /b 1

:launch_failed
echo [VILLIEN Editor] Windows could not start the editor.
echo Try running "%VILLIEN_EDITOR%" --editor from "%VILLIEN_ROOT%".
pause
exit /b 1
