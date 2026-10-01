@echo off
rem Builds build\CRStreamingFix.addon64 with the MSVC x64 toolchain (VS 2022 or its Build Tools).
rem   build.bat                                      build the add-on
rem   build.bat test                                 also run the offline lifecycle test (no game needed)
rem   build.bat test "path\to\CONTROLResonant.exe"   ... and check that the signatures resolve in that exe
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if errorlevel 1 call :find_msvc
where cl >nul 2>nul
if errorlevel 1 (
    echo MSVC not found. Run this from an "x64 Native Tools Command Prompt for VS 2022".
    exit /b 1
)

if not exist build mkdir build

rc /nologo /fo build\CRStreamingFix.res src\CRStreamingFix.rc || exit /b 1
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W4 /wd4100 /DUNICODE /D_UNICODE src\CRStreamingFix.cpp build\CRStreamingFix.res /Fobuild\ /Febuild\CRStreamingFix.addon64 /link /DLL psapi.lib || exit /b 1
del build\CRStreamingFix.lib build\CRStreamingFix.exp 2>nul
echo Built build\CRStreamingFix.addon64

if /i not "%~1"=="test" exit /b 0

rem The add-on only wakes up inside CONTROLResonant.exe, so the test harness is built under that name.
if not exist build\lifecycle mkdir build\lifecycle
cl /nologo /Od /MT /EHsc /std:c++17 /W4 /DUNICODE /D_UNICODE src\test_lifecycle.cpp /Fobuild\lifecycle\ /Febuild\lifecycle\CONTROLResonant.exe || exit /b 1
copy /y build\CRStreamingFix.addon64 build\lifecycle\ >nul
build\lifecycle\CONTROLResonant.exe || exit /b 1

if "%~2"=="" exit /b 0
cl /nologo /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE src\test_locate.cpp /Fobuild\ /Febuild\test_locate.exe /link psapi.lib || exit /b 1
build\test_locate.exe "%~2"
exit /b %errorlevel%

:find_msvc
for %%R in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
    for %%E in (BuildTools Community Professional Enterprise) do (
        if exist "%%~R\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
            call "%%~R\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
            exit /b 0
        )
    )
)
exit /b 1
