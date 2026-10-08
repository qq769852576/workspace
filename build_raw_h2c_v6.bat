@echo off
setlocal
cl /nologo /std:c++17 /EHsc /O2 raw_h2c_v6.cpp setupapi.lib /Fe:raw_h2c_v6.exe
if errorlevel 1 (
  echo BUILD FAIL
  exit /b 1
)
echo BUILD PASS
