@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1
cd /d "%~dp0"
if exist AudioBeast.exe del /F AudioBeast.exe
if exist resource.res del /F resource.res
echo Compiling...
rc.exe /nologo resource.rc
cl.exe /nologo /O2 /EHsc /MT /std:c++17 /Fe:AudioBeast.exe main.cpp resource.res ole32.lib oleaut32.lib mmdevapi.lib comctl32.lib gdi32.lib user32.lib shell32.lib /link /SUBSYSTEM:WINDOWS
if %ERRORLEVEL% neq 0 ( echo FAILED & exit /b 1 )
echo BUILD_OK
echo Launching...
start "" AudioBeast.exe
