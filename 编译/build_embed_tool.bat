@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul 2>nul
cd /d "%~dp0.."

REM Build NepgearEmbed.exe. The embedded templates are "skeletons": a complete
REM version.dll whose RCDATA payload is cut down to a 1-byte placeholder. That
REM payload is always replaced by the user's .chs at conversion time, so keeping
REM 23 MB of it inside the tool would be pure waste. Skeleton in, full DLL out.

set "WORK=tools\NepgearEmbed"
set "FULL_X86=Release\version.dll"
set "FULL_X64=x64\Release\version.dll"
set "SKEL_X86=%WORK%\template_x86.dll"
set "SKEL_X64=%WORK%\template_x64.dll"
set "TOOL=Release\NepgearEmbed.exe"

if not exist "%FULL_X86%" goto :need_x86
if not exist "%FULL_X64%" goto :need_x64

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" goto :no_vswhere

set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH goto :no_vcpp

set "MSBUILD=!VS_PATH!\MSBuild\Current\Bin\MSBuild.exe"
if not exist "!MSBUILD!" goto :no_msbuild

REM Step 1: stripping needs a working NepgearEmbed.exe. If it is missing, build
REM one from the full DLLs first (big, but only an intermediate product).
if exist "%TOOL%" goto :strip
echo [1/3] 首次构建：先用完整 DLL 编一版临时工具 ...
copy /y "%FULL_X86%" "%SKEL_X86%" >nul
copy /y "%FULL_X64%" "%SKEL_X64%" >nul
"!MSBUILD!" "%WORK%\NepgearEmbed.vcxproj" -p:Configuration=Release -p:Platform=Win32 -t:Rebuild -v:minimal -nologo
if errorlevel 1 goto :fail_bootstrap

:strip
echo [2/3] 裁剪模板 ...
"%TOOL%" --strip "%FULL_X86%" "%SKEL_X86%"
if errorlevel 1 goto :fail_strip_x86
"%TOOL%" --strip "%FULL_X64%" "%SKEL_X64%"
if errorlevel 1 goto :fail_strip_x64

echo [3/3] 正在编译 NepgearEmbed.exe ...
"!MSBUILD!" "%WORK%\NepgearEmbed.vcxproj" -p:Configuration=Release -p:Platform=Win32 -t:Rebuild -v:minimal -nologo
set "RESULT=!ERRORLEVEL!"

del /q "%SKEL_X86%" "%SKEL_X64%" >nul 2>nul

if not "!RESULT!"=="0" goto :fail_build

echo.
for %%F in ("%TOOL%") do echo   完成: %TOOL% ^(%%~zF 字节^)
echo   双击它，把 .chs 拖进窗口；顺手把游戏 EXE 拖进去会按它的位数生成。
exit /b 0

:need_x86
echo [错误] 找不到 %FULL_X86%
echo        先运行 build.bat x86 生成 version.dll，再回来编译这个工具。
exit /b 1

:need_x64
echo [错误] 找不到 %FULL_X64%
echo        先运行 build.bat x64 生成 version.dll，再回来编译这个工具。
exit /b 1

:no_vswhere
echo [错误] 找不到 vswhere.exe，请安装带 C++ 工作负载的 Visual Studio。
exit /b 1

:no_vcpp
echo [错误] 没有找到带 C++ 工具集的 Visual Studio。
exit /b 1

:no_msbuild
echo [错误] 找不到 !MSBUILD!
exit /b 1

:fail_bootstrap
echo [错误] 临时工具编译失败。
del /q "%SKEL_X86%" "%SKEL_X64%" >nul 2>nul
exit /b 1

:fail_strip_x86
echo [错误] 裁剪 x86 模板失败。
del /q "%SKEL_X86%" "%SKEL_X64%" >nul 2>nul
exit /b 1

:fail_strip_x64
echo [错误] 裁剪 x64 模板失败。
del /q "%SKEL_X86%" "%SKEL_X64%" >nul 2>nul
exit /b 1

:fail_build
echo [错误] 编译失败。
exit /b 1
