@echo off
setlocal
cmake -S . -B build -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1
echo.
echo Built: build\Release\stazec.exe
endlocal
