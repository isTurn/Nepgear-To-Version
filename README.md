# Nepgear

[![build](https://github.com/isTurn/Nepgear-To-Version/actions/workflows/build.yml/badge.svg)](https://github.com/isTurn/Nepgear-To-Version/actions/workflows/build.yml)

**给 Winlator 等 Android 模拟器用的 Galgame 汉化修复方案。**

日文 Galgame 的汉化补丁一般以 `.chs` 数据包 + Windows 侧前置加载器的形式分发，那个加载器要做三件事：从系统目录取真身 DLL、改进程的区域与代码页、把替换字体塞进进程。这套逻辑只在桌面 Windows 上成立。原封不动搬进 Winlator（Wine + Box86 / Box64）这类容器后，前置条件全都不在了——真身 DLL 的取法不匹配、locale 改不动、字体装不上，于是补丁加载不上、文本乱码，严重时启动即崩。**这类问题改游戏文件或调模拟器设置都解决不了**：原来那套「`.chs` + 前置加载器」在模拟器上没有落地条件，照搬必然失败。

所以才有了这个项目。Nepgear 保留 `.chs` 的数据格式，只把加载方式换掉：不再靠独立加载器，而是把补丁数据和全部修复做进一个游戏本来就会导入的系统 DLL。产物伪装成 `version.dll` 放在游戏目录，随游戏启动自动生效；17 个 `Ver*` 导出照旧转发给系统真实 `version.dll`，游戏原有功能不受影响。`.chs` 里的补丁数据可以整体内嵌进 DLL，部署时只留 `version.dll` + `Nepgear.ini`，容器里不需要任何额外加载器。

在桌面 Windows 上同样可用，但它的目标场景是模拟器。

支持 x86 / x64。多数 Galgame 为 32 位，通常用 x86 版本。

## 功能概览

| 模块 | 说明 |
| --- | --- |
| DLL 代理 | 伪装 `version.dll`，17 个导出转发给系统真实 DLL，不改动游戏文件 |
| 区域模拟 | 内置 Locale Emulator Plus，以日文 locale 重启进程 |
| 字体 Hook | 替换字体、缩放、字重、字距、垂直度量、字符集伪装 |
| 文本替换 | 日文旧字体↔繁体、繁简互转，共三张映射表 |
| 代码页重定向 | 在 `MultiByteToWideChar` / `WideCharToMultiByte` 边界改码页 |
| 引擎适配 | 16 个引擎的专用字体处理（见下表） |
| 窗口标题 | 三种模式改写标题，支持 Unicode |
| VFS 与重定向 | 优先读外部文件夹或 `.chs` 封包 |

### 已适配引擎

BGI / Buriko、Artemis（含 Legacy）、KiriKiri / TVP、EntisGLS、Softpal、Escu:de、Mirai、Majiro、DxLib、TinkerBell、TopCat（x86）、CatSystem2、Unity（Mono / IL2CPP / TextMeshPro）、TyranoScript 5 / Electron、Ren'Py、YU-RIS。

引擎开关默认全关，需要哪个开哪个，且受 `[Font] Enable` 总开关约束。各引擎实现细节见 `Nepgear/hooks/internal/font/engines/*/README.md`。

## 构建

需要 Visual Studio 2022（v143 工具集）和 Windows SDK。

```bat
build.bat              两个架构 Release
build.bat x86          仅 32 位
build.bat x64          仅 64 位
build.bat x86 nopopup  不编译启动弹窗
build.bat all rebuild  全量重建
build.bat x86 debug    Debug 配置
```

产物固定为 `version.dll`，转发系统 `version.dll` 的 17 个 `Ver*` 导出；`TargetName`、代理头和 x64 汇编跳板都指向它。

### 启动弹窗（补丁作者信息）

启动时那个「游玩通知」有两个开关，编译期和运行期各一层：

| 层 | 开关 | 效果 |
| --- | --- | --- |
| 编译期 | `NEPGEAR_ENABLE_STARTUP_POPUP=0` | 整段提示**连同字符串一起编掉**，DLL 里搜不到作者信息，也不再有 `MessageBox` 调用 |
| 运行期 | `Nepgear.ini` → `[System] ShowStartupPopup=0` | 代码仍在，运行时直接跳过；改 ini 即可，不用重编 |

编译期默认仍为 1。关闭方式任选：

```bat
build.bat x86 nopopup                              :: build.bat 位置参数
python 编译\build_chs_embed.py Nepgear.chs x86 --no-popup
msbuild Nepgear.sln -p:Configuration=Release -p:Platform=Win32 -p:NepgearStartupPopup=0
```

切换这个开关会走独立的中间目录（`obj\<平台>\<配置>\popup0|1\`），所以不会出现改了宏却复用旧 obj、弹窗照样出现的情况。

`build_x86.bat` / `build_x64.bat` 是单架构的简写入口。也可以直接用 Visual Studio 打开 `Nepgear.sln`，三个工程（Nepgear、Packer、Unpacker）都在里面。

产物：

| 架构 | 输出目录 |
| --- | --- |
| x86 | `Release\version.dll`、`Packer.exe`、`Unpacker.exe` |
| x64 | `x64\Release\version.dll`、`Packer.exe`、`Unpacker.exe` |

Release 构建始终输出**未压缩**的原始 DLL，工程里没有 UPX 打包步骤，PE 里也就没有 UPX 段，可直接用常规工具查看。另外不生成 `.pdb` / `.lib` / `.exp`，输出目录只有可执行文件。

### 在 GitHub 上构建（不装 VS）

`.github/workflows/build.yml` 让整条流水线跑在 GitHub 的 Windows runner 上，本地什么都不用装：

- **推送到 `main` / 提 PR** → 自动构建一次 x86，跑完在这条运行记录的 **Artifacts** 里下载 `nepgear-version-dll-x86`（含 `x86\version.dll`、`Nepgear.ini`、`Packer.exe`、`Unpacker.exe`）。
- **手动触发** → Actions → `build` → *Run workflow*，可选目标架构（`x86` / `x64` / `all`）、是否编入启动弹窗、是否全量重建。
- **打 tag** → 推一个 `v*` 开头的 tag（如 `git tag v1.0.0 && git push origin v1.0.0`），构建通过后自动创建 Release 并附上 `Nepgear-<tag>.zip`。

CI 里执行的就是两条本地命令，没有另一套构建逻辑：

```bat
python 编译\build_chs_embed.py Nepgear.chs x86
python 编译\check_dll.py Release\version.dll
```

`编译\check_dll.py` 是产物校验：核对 PE 架构与路径相符、**不存在 UPX 段**、`RCDATA` 资源 `101` 存在且体积对得上。本地构建完也可以自己跑一遍，参数换成 `x64\Release\version.dll` 即可。

### 把 .chs 内嵌进 DLL

默认构建出的代理 DLL 在运行时读游戏目录里的 `Nepgear.chs`。想让 DLL 自带补丁数据（部署只留 `version.dll` + `Nepgear.ini`），再用一键脚本把 `.chs` 作为 RCDATA 资源打进去。**模拟器上推荐用这种形态**：容器里少一层文件就少一处出错的地方。

```bat
python 编译\build_chs_embed.py Nepgear.chs x86    :: 产物 Release\version.dll（预解压、未压缩）
python 编译\build_chs_embed.py Nepgear.chs all    :: 两个架构
python 编译\build_chs_embed.py --clean            :: 去掉内嵌，恢复普通构建
```

脚本把 `.chs` 里每个条目**预解压**后写进 `Nepgear\chs_payload.dat`，生成的 DLL 读取归档时不再做任何 LZMS 解压；输出保持未压缩。这是唯一的构建形态，没有更小体积的备选。

脚本随后生成 `Nepgear\chs_payload.rc` 并调用 `build.bat`。`Nepgear.vcxproj` 里这项带 `Condition`，所以没有资源的工程照旧能编。

运行时优先级：游戏目录（或重定向目录）里的 `Nepgear.chs` > DLL 内嵌数据；松散文件目录始终优先于归档。日志 `Nepgear.log` 里 `[VFS] Initialized ... (embedded archive)` 表示走的是内嵌数据。

细节、手动步骤和排查见 [编译\编译教程.txt](编译/编译教程.txt)。

### 不装 VS：图形化转换器（NepgearEmbed.exe）

上面两条路都要装 Visual Studio 或 Python。如果只是想把手上的 `.chs` 变成 `version.dll`，用这个独立小工具：它自己带着一份已经编译好的 DLL 当模板，只把里面的补丁数据换掉，**不需要 VS、不需要 Python，也不依赖系统解压组件**——拷贝到任何 Windows 机器、甚至直接放进 Winlator 容器里都能跑。

现成的可执行文件在 [Releases](https://github.com/isTurn/Nepgear-To-Version/releases) 里，下下来就能用。也可以自己用本机工具链编译一次：

```bat
编译\build_embed_tool.bat        :: 产物 Release\NepgearEmbed.exe
```

脚本会把 `Release\version.dll` 当作模板打进 exe（所以要先构建一次 x86 版本），然后编译出约 24 MB 的 `NepgearEmbed.exe`。

用法：

1. 双击打开，把 `.chs` 拖进窗口，或点「选择 .chs」；输出的 `version.dll` 默认写到 `.chs` 同目录，也可以自己改路径。
2. 也可以走命令行，静默转换、不弹窗，方便脚本调用：

```bat
Release\NepgearEmbed.exe Nepgear.chs
Release\NepgearEmbed.exe Nepgear.chs D:\out\version.dll
```

两点需要知道：

- **它只替换数据、不重新编译代码**。源码改了以后要重新跑一次 `build_embed_tool.bat`，否则 exe 里带的还是旧模板。
- **弹窗开关由模板决定**。想换成不弹窗的版本，先用 `build.bat x86 nopopup` 生成对应 DLL，再编译工具。
- 转换是纯字节操作：读 `.chs`、逐条目预解压、重建资源节，几秒完成，输出未压缩。

**为什么它在 Wine / Winlator 上也能用**：`.chs` 里的条目是 LZMS 压缩的，Windows 上靠 `cabinet.dll` 的压缩 API 解压，而 Wine 里那套 API 是个空壳——`CreateDecompressor` 直接返回成功，`Decompress` 只把输入原样拷回去，于是会看到「LZMS 解压失败」。所以工程里自带了一份 LZMS 解码器（`Common/lzms_decode.h`），系统 API 不可用或产出与预期大小不符时自动接管。同一个解码器也在 `version.dll` 里，容器中直接读压缩过的 `Nepgear.chs` 同样没问题。

排查时可以用环境变量强制走内置解码器，对比两条路径的结果：

```bat
set NEPGEAR_FORCE_NATIVE_LZMS=1
Release\NepgearEmbed.exe Nepgear.chs out\version.dll
```

## 部署

复制到游戏主 EXE 所在目录：

1. `version.dll` — 架构必须与游戏进程一致
2. `Nepgear.ini` — 配置文件，**必须存为 UTF-16 LE**，缺失会直接弹窗退出
3. 字体文件 — 文件名与 ini 里的 `FileName` 一致

游戏启动时会加载同目录下的 `version.dll`，Nepgear 把原有调用转发给系统真正的 `version.dll`，游戏原有功能不受影响。（Winlator 这类容器默认优先加载内建 DLL，需要额外一条覆盖设置，见下面「Winlator / 模拟器」。）

开启 `[LocaleEmulator] Enable=1` 时还需要把官方 LEP 2.x 的运行时 DLL 放进重定向文件夹（默认 `Nepgear/`）。同位数启动至少需要对应的 `LoaderDll` 与 `LocaleEmulatorPlus`；跨位数子进程需要同时提供同版本的 `LoaderDll_x86.dll`、`LocaleEmulatorPlus_x86.dll`、`LoaderDll_x64.dll`、`LocaleEmulatorPlus_x64.dll`。这些运行时不随仓库分发。

已验证官方 LEP **v2.0.6.0**，并兼容 v2.0.0.0 的早期 2.x 配置接口。接口说明和字体更新对照见 [兼容更新说明](docs/compatibility.md)。

### Winlator / 模拟器

部署方式与桌面完全一致，三个文件丢进游戏主 EXE 目录即可，容器里**不需要**再放原汉化的加载器或 `.chs`。额外注意两点：

1. **让容器加载目录里这一份。** Wine 优先加载内建的 `version.dll`，不设置覆盖时本地文件根本不会被读到（表现为补丁毫无反应、日志也没有输出）。在容器环境变量或该游戏的快捷方式里加一条 `WINEDLLOVERRIDES=version=n,b`，需要在启动游戏前生效。
2. **位数对齐。** 32 位游戏用 x86 产物，且容器的 Wine 位数要能跑 32 位进程；两者不匹配时 DLL 直接加载失败。

想确认到底加载了哪一份，给容器加 `WINEDEBUG=+loaddll`，日志里应能看到从**游戏目录**读取 `version.dll`；如果出现的是 Wine 内建路径，说明第 1 条没生效。

## 配置

完整带注释的配置见仓库根目录的 `Nepgear.ini`，这里只列关键项。

### `[System]`

```ini
Enable=1              ; 总开关，0 = 全部功能停用
ShowStartupPopup=1    ; 启动提示弹窗
```

### `[Font]`

```ini
Enable=1
FileName=galgame.ttf  ; 放在游戏根目录或重定向文件夹，留空用系统字体
FaceName=galgame      ; CreateFont 的 FaceName，建议与字体内部名一致
Charset=128           ; 128=Shift-JIS  134=GB2312  1=Default

EnableFaceNameReplace=1
EnableCharsetReplace=0

EnableHeightScale=1
HeightScale=1.0       ; 支持小数，修正汉化后字体过大过小
EnableWidthScale=1
WidthScale=1.0
EnableWeight=1
Weight=0              ; 0=沿用程序请求  400=标准  700=粗体
```

字距、垂直度量、行距：`EnableCharSpacing` / `CharSpacing`、`EnableVerticalMetrics` / `AscentPermille` / `DescentPermille`、`EnableLineSpacing` / `LineSpacing`。垂直度量按 unitsPerEm 千分比修补 hhea 和 OS/2 表。

字符集伪装与文本替换：

```ini
EnableCodepageSpoof=0   ; 把游戏请求的 SpoofFromCharset 伪装成 SpoofToCharset
SpoofFromCharset=128
SpoofToCharset=1

EnableTextSubstitution=0
TextSubstitutionMode=0  ; 0=日文旧字体/异体字  1=繁转简  2=简转繁
TextSubstitutionCodepage=932
```

### `[FontCompatibility]`

个别引擎对某些 GDI Hook 敏感，这里逐项关闭：`SkipDrawTextA`、`SkipFontDataQueries`、`SelectObjectTrackedOnly`、`HookCreateFontW`、`HookCreateFontIndirectW`、`HookGetTextFace`。字体不生效或渲染异常时优先调这一节。

### `[FontEngines]`

每个引擎一个 `Enable*` 开关加若干细项。例如：

```ini
EnableBgiHook=0
BgiPatchGdiImports=1              ; Win32 专用，x64 无效
BgiClearGlyphCacheOnSwitch=1

EnableKrkr=0
KrkrDisablePrerenderedFonts=1
KrkrNativeDispatchMode=0          ; 0=自动 TVP 1.4.x  1=跳过版本门控  2=禁用

EnableTopCatHook=0                ; x86 专用，验证 GDI 导入和调用点后启用

EnableArtemis=0
ArtemisPatchTables=1              ; 改 list_windows*.tbl / IET 文本里的字号、字距、行距
ArtemisRedirectFontFiles=1
ArtemisFontSize=0                 ; 0 = 保留游戏原值
ArtemisRubySize=-1                ; -1 = 保留游戏原值
```

BGI 会在 `GetOutlineTextMetricsA/W` 中保留原请求字体名，避免替换字体触发引擎递归重建；系统 USER32 字体请求跳过 BGI 专用度量归一化。KRKR 保留原生 `mapPrerenderedFont` 成员名与 TFT 文件，在接口兼容时把字体映射请求转到同一字体实例的原生 unmap；旧版或未知版本在自动模式下保留原生路径。TopCat 的度量保护仅作用于确认过的 x86 调用点。

其余引擎（EntisGLS、Softpal、Escu:de、Mirai、Majiro、DxLib、TinkerBell、Unity、Tyrano、Ren'Py、YU-RIS）的开关命名同理，逐项注释见 `Nepgear.ini`。

CatSystem2 和 YU-RIS 没有独立总开关：前者在 `Enable=1` 且 `EnableFaceNameReplace=1` 时自动检测，后者随字体替换启用；YU-RIS 另有 `YurisAtlasCodepage` 控制图形集码页（0 = 按资源结构自动判断）。

### `[Window]`

```ini
Enable=1
TitleMode=2   ; 0=进程内同步  1=仅 WinEvent 监听  2=混合（推荐）
Title=自定义标题
```

### `[FileRedirect]` 与 `[FileHook]`

```ini
Enable=1
Folder=Nepgear            ; 优先从这里找文件
ArchiveFile=Nepgear.chs   ; 封包模式的资源包

VFSMode=0                 ; 0=物理读取  1=内存读取
```

### `[LocaleEmulator]`

```ini
Enable=0
CodePage=932              ; 932=Shift-JIS  936=GBK
LocaleID=1041             ; 1041=日本  2052=中国
Charset=128
RegistryRedirectionMode=2 ; 0=核心  1=基础  2=高级
HookUILanguageMode=1     ; 0=关闭  1=Win32 API  2=Native API + MUI
Timezone=Tokyo Standard Time
```

按当前进程位数加载匹配的 `LoaderDll` 与核心 DLL，并暂存已提供的另一位数组以支持子进程。优先传入 LEPB v4/416，早期 2.x Loader 拒绝该结构时回退到 v2/204。运行时暂存到系统临时目录。`Timezone` 使用 Windows 时区 ID；未知 ID 会报告配置错误。

### `[CodePage]`

```ini
Enable=0
FromCodePage=932
ToCodePage=65001
```

用于引擎硬编码 CP932 解码 UTF-8 脚本的场合。字符集伪装发生得太晚时（文本在 GDI 之前就已成乱码），改这里。

### `[GLOBAL]`

KiriKiri XP3 补丁与 RioShiina 专用：`EnableKrkrz` / `KrkrzPatchFile`、`EnableRioShiina` / `RioShiinaMode`（1 = 翻译重定向，2 = 解包）等。

### `[Debug]`

```ini
Enable=0                  ; 弹出 CMD 控制台显示 Hook 日志
LogToFile=0               ; 输出 Nepgear.log
EnableFontTrace=0         ; 字体 Hook 细粒度采样
FontSlowMs=50
FontTraceSampleLimit=0
```

字体不生效时先开 `Enable=1` 看日志，能看到实际拦到哪些 API、字体是否加载成功。

## 配套工具

`Packer.exe` 把翻译资源打成单个 `.chs`：保持原目录结构放好文件，把文件夹拖到 `Packer.exe` 上，同级生成同名 `.chs`，再在 ini 里配 `ArchiveFile`。

`Unpacker.exe` 反向解包，用于检查封包内容。

`NepgearEmbed.exe` 把 `.chs` 直接转成 `version.dll`（补丁数据内嵌），双击开窗口、拖入文件即可，不需要编译环境。源码在 `tools\NepgearEmbed\`，用 `编译\build_embed_tool.bat` 构建，详见上面「不装 VS：图形化转换器」。

这些都是命令行工具，直接拖拽即可。

## 目录结构

```
Nepgear/            主 DLL 工程
  dllmain.cpp       入口与 Hook 安装顺序
  hooks/            各子系统聚合单元
    internal/       .cppinc 实现切片（不单独编译）
      font/engines/ 16 个引擎适配
      locale/       LEP 2.x 动态调用 ABI
  font/             字体表修补（name / cmap / OS/2 / hhea）
Common/             Packer 与 Unpacker 共用头文件
Packer/ Unpacker/   资源打包工具
tests/              原生区域、字体和 KRKR 接口回归探针
```

代码组织约定见 `Nepgear/hooks/README.md`。`.cppinc` 是实现切片，与所属 `.cpp` 共享私有状态和原始 API 指针，不能单独编译。

兼容测试的构建与执行方法见 [tests/README.md](tests/README.md)。
