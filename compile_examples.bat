@echo off
setlocal
set STAZEC=build\Release\stazec.exe
if not exist "%STAZEC%" (
  echo stazec.exe not found. Run build_windows.bat first.
  exit /b 1
)
"%STAZEC%" examples\hello.stz3 -o hello.exe || exit /b 1
"%STAZEC%" examples\arithmetic_control.stz3 -o arithmetic_control.exe || exit /b 1
"%STAZEC%" examples\rich_cardinality.stz3 -o rich_cardinality.exe || exit /b 1
"%STAZEC%" examples\rich_resources.stz3 -o rich_resources.exe || exit /b 1
"%STAZEC%" examples\rich_caller_storage.stz3 -o rich_caller_storage.exe || exit /b 1
"%STAZEC%" examples\heap_allocation.stz3 -o heap_allocation.exe || exit /b 1
"%STAZEC%" examples\fault_payload_native.stz3 -o fault_payload_native.exe || exit /b 1
echo.
echo Built scalar and Compiler 0.5 rich representation examples.
endlocal
