@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul 2>nul
cd /d "%~dp0.."

REM 构建 tools\NepgearEmbed\NepgearEmbed.exe：把 Release\version.dll 作为模板
REM 打进 exe，得到一个不依赖 Visual Studio / Python 的 .chs -> version.dll 转换器。

set "TEMPLATE=Release\version.dll"
if not exist "%TEMPLATE%" (
    echo [错误] 找不到 %TEMPLATE%
    echo         先运行 build.bat x86 生成 version.dll，再回来编译这个工具。
    exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [错误] 找不到 vswhere.exe，请安装带 C++ 工作负载的 Visual Studio。
    exit /b 1
)

set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo [错误] 没有找到带 C++ 工具集的 Visual Studio。
    exit /b 1
)

set "MSBUILD=!VS_PATH!\MSBuild\Current\Bin\MSBuild.exe"
if not exist "!MSBUILD!" (
    echo [错误] 找不到 !MSBUILD!
    exit /b 1
)

set "WORK=tools\NepgearEmbed"
copy /y "%TEMPLATE%" "%WORK%\template.dll" >nul

echo 正在编译 NepgearEmbed.exe ...
"!MSBUILD!" "%WORK%\NepgearEmbed.vcxproj" -p:Configuration=Release -p:Platform=Win32 -t:Rebuild -v:minimal -nologo
set "RESULT=!ERRORLEVEL!"

del /q "%WORK%\template.dll" >nul 2>nul

if not "!RESULT!"=="0" (
    echo [错误] 编译失败，退出码 !RESULT!
    exit /b !RESULT!
)

echo.
for %%F in ("Release\NepgearEmbed.exe") do echo   完成: Release\NepgearEmbed.exe ^(%%~zF 字节^)
echo   双击它，把 .chs 拖进窗口即可生成 version.dll。
exit /b 0
