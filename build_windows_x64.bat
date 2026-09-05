@echo off
setlocal
cmake -S . -B build -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1
echo.
echo Staze C++23 Compiler 0.8.0 built successfully.
echo Compiler: build\Release\stazec.exe
endlocal
