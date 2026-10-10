@echo off
setlocal
set HERE=%~dp0
set CXX=g++.exe
if exist "%HERE%..\..\w64devkit\bin\g++.exe" set CXX="%HERE%..\..\w64devkit\bin\g++.exe"

echo [HL2Craft] Compiling HL2Minecraft.addon32...
%CXX% -m32 -shared -O3 -std=c++20 -static -DWIN32_LEAN_AND_MEAN -DNOMINMAX ^
  -I "%HERE%third_party\reshade" -I "%HERE%src" ^
  "%HERE%src\hl2_bridge.cpp" "%HERE%src\compositor.cpp" "%HERE%src\d3d9_render.cpp" "%HERE%src\addon_entry.cpp" ^
  -luser32 -lgdi32 -ld3d9 -o "%HERE%HL2Minecraft.addon32"

if %ERRORLEVEL% EQU 0 (
  echo [HL2Craft] Build SUCCESSFUL: %HERE%HL2Minecraft.addon32
  copy /y "%HERE%HL2Minecraft.addon32" "%HERE%..\fabric\src\main\resources\assets\hl2craft\HL2Minecraft.addon32" >nul
  echo [HL2Craft] Bundled addon updated in fabric resources.
) else (
  echo [HL2Craft] Build FAILED!
  exit /b 1
)

