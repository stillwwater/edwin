@echo off

taskkill /im test.exe /f >nul 2>&1

rem Compiling as C
clang-cl /nologo /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Zi /W4 /std:c11 /I. /c edwin.c

rem Compiling as C++
cl /nologo /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Zi /W4 /std:c++17 /I. /c /TP edwin.c

cl /nologo /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Zi /W4 /std:c++17 /I. /c test.cpp

if errorlevel 1 exit /b 1

rem Optional: generate manifest to get newer win32 controls
link /nologo /debug:full /subsystem:windows user32.lib gdi32.lib comctl32.lib msimg32.lib edwin.obj test.obj /out:test.exe ^
    /manifestdependency:"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'"

if "%1"=="/r" (
    start test.exe
)
