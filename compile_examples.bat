@echo off
setlocal
set STAZEC=build\Release\stazec.exe
if not exist "%STAZEC%" (
  echo stazec.exe not found. Run build_windows_x64.bat first.
  exit /b 1
)

for %%E in (
  hello
  arithmetic_control
  rich_cardinality
  rich_resources
  lifetime_cleanup
  dynamic_many
  owned_heap_transfer
  borrow_cross_boundary
  shared_scope
  partial_field_move
  transaction_field_undo
  transaction_observable_rollback
  transaction_resource_abort
  transaction_shared_abort
  parallel_tasks
  parallel_shared_task
  parallel_send_task
) do (
  echo Building %%E.exe...
  "%STAZEC%" "examples\%%E.stz3" -o "%%E.exe" || exit /b 1
)

echo.
echo All representative Compiler 0.8.0 examples built successfully.
endlocal
