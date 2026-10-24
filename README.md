# obs-Import-psd

**OBS Studio 的 PSD 导入 / 导出插件** · PSD import/export plugin for OBS Studio

[![macOS](https://img.shields.io/badge/platform-macOS%20arm64-lightgrey)]()
[![OBS](https://img.shields.io/badge/OBS-32.x-blue)]()
[![License](https://img.shields.io/badge/license-GPL--2.0--or--later-green)]()

**[English](#english) · [中文](#中文) · [未修复的 Bug](#未修复的-bug截至最后一次提交)**

---

## 作者 / Author

**鲸鱼少女** (Whale Girl) — built with [DeepSeek Harness](https://github.com/deepseek-ai) as a coding agent,
together with **@lt5d-yunzi**.

> 一条会写 OBS 插件的鲸鱼。不是鱼，也不胖。

---

## 中文

### 作者

**鲸鱼少女** — 由 [DeepSeek Harness](https://github.com/deepseek-ai) 编码代理驱动，
与 **@lt5d-yunzi** 协作完成。

给 OBS 加两个菜单项：

* **工具 → 导入 PSD 为场景…**：把 `.psd` 的每个图层变成场景里的一个源，图层顺序与 Photoshop 的图层面板完全一致（最底层 = 最底下的源）。
* **工具 → 导出场景为 PSD…**：把当前场景按"源列表"顺序写成 PSD 图层，同样是**从下到上**一一对应。

导出时**会跳过**采集类源（浏览器、视频/媒体、屏幕/窗口/游戏采集、摄像头、虚拟摄像头），
**不会跳过**图片与文本图层。

---

## 规则细节

### 导出：哪些图层会被忽略

按源 type id 判断（`obs_source_get_id()`），命中即跳过：

| 类别 | type id |
|---|---|
| 浏览器 | `browser_source`、`obs_browser_source` … |
| 视频/媒体 | `ffmpeg_source`、`vlc_source`、`media_source`、`gstreamer-source` |
| 屏幕/窗口/设备采集 | `screen_capture`、`window_capture`、`game_capture`、`display_capture`、`monitor_capture`、`xcomposite_input`、`av_capture_input*`、`syphon-input`、`ndi_source`、`decklink-input`、`dshow_input` … |
| 虚拟摄像头 | `virtualcam`、`webcam` |

例外（名字里带标记，大小写不敏感）：

* `[keep]`：**强制导出**（即使它是采集/浏览器/视频源）
* `[skip]` / `[ignore]`：**强制跳过**（即使它是图片或文本）

漏掉的类型可以在导出对话框的 “Extra ignored ids” 里补，逗号分隔，按子串匹配。
对话框下方的报告会列出每个源的处置结果（导出 / 跳过 + 原因）。

### 导出：图层怎么摆

* 每个场景项按它在场景里的**实际变换**渲染：位置、缩放、旋转（按外接矩形近似）、裁剪。
* 图层矩形会被夹到画布范围内，完全在画布外的源会作为空图层保留。
* 场景组（Group）会被展开，组内图层按实际顺序落到父级。
* 隐藏的源会导出成**隐藏图层**（`visible = false`），保持文档忠实。
* PSD 的扁平化预览图（composite）由所有可见图层合成。

### 导入：PSD → 场景

* 每个 PSD 图层 → 一个 `image_source` 场景项，名字 = 图层名（含中文/emoji，走 UTF-8 `luni` 通道）。
* 顺序：PSD 第 0 层（最底）→ 场景最底下的源，和 Photoshop 面板顺序一致。
* 位置：默认按画布"等比适配并居中"（可选 1:1 文档像素，见 `SceneBuildOptions::Placement`）。
* 可见性：还原 PSD 的隐藏/显示状态；不透明度烘焙进 PNG 的 alpha。
* `mul`/`scrn` 混合模式映射到 OBS 的 Multiply/Screen。
* PSD 组（`lsct` 分隔符）会变成 OBS 的组（Group）。

---

## 构建与验证

```bash
./build.sh                 # 构建插件：build/obs-psd.plugin
./tests/run_all.sh         # PSD 编解码三层验证（往返 / 独立解析 / fuzz）
./install.sh [目标目录]    # 复制到 dist/plugins（默认）
```

`tests/run_all.sh` 会做：
1. C++ 往返测试（154 项断言：图层名 UTF-8、边界、透明度、可见性、像素、组、复合图、重写稳定性）；
2. `tests/verify_psd.py` —— 只用 Python 标准库的**独立** PSD 解析器，交叉校验写出的文件；
3. `tests/psd_fuzz` —— 20000 个随机变异/截断输入，ASAN + UBSAN 下要求零崩溃、零挂起、零静默失败。

`install.sh` 默认写 `./dist/plugins`；要把插件装进 OBS，请复制到：

```
~/Library/Application Support/obs-studio/plugins/obs-psd.plugin
```

（该目录可能受 macOS 隐私保护限制，必要时用 Finder 复制或给终端 "完全磁盘访问权限"。）

### 构建

```bash
./fetch-deps.sh    # 首次构建需要：下载第三方依赖（见下）
./build.sh         # 产物：build/obs-psd.plugin
./tests/run_all.sh # 跑 PSD 往返 + 独立校验 + fuzz
```

`third_party/` **不包含在本仓库里**——里面全是别人的代码，各有各的许可证：

| 依赖 | 来源 | 许可证 |
|---|---|---|
| libobs / obs-frontend-api 头文件 | [obsproject/obs-studio](https://github.com/obsproject/obs-studio) 32.2.2 | GPL-2.0-or-later |
| Qt 头文件（QtCore/QtGui/QtWidgets） | 随 OBS.app 分发的 Qt 框架 | LGPL-3.0 |
| simde | [simd-everywhere/simde](https://github.com/simd-everywhere/simde) 0.8.2 | MIT |
| miniz | [richgel999/miniz](https://github.com/richgel999/miniz) 3.0.2 | MIT / public domain |
| stb_image_write.h | [nothings/stb](https://github.com/nothings/stb) | public domain / MIT |

`fetch-deps.sh` 只下载构建必需的公共头文件；`build.sh` 在 `third_party/` 缺失时会自动调用它。

### 构建要求

* macOS arm64、Apple clang
* `curl`、`tar`（下载依赖用）
* **`-isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk`**：本机默认 SDK（MacOSX27）的 `libSystem.tbd` 损坏，链接会报 `tapi error: malformed file`；`build.sh` 已处理，可用 `SDK=... ./build.sh` 覆盖。
* OBS 32（默认 `/Applications/OBS.app`，可用 `OBS_APP=... ./build.sh` 覆盖）。
* 不依赖 OBS 源码树：libobs / frontend-api 头文件与 `simde`、Qt 头文件已 vendored 在 `third_party/`。

## 目录

```
src/
  plugin_main.cpp       插件入口、工具菜单、本地化
  ui/psd_ui.cpp         Qt 对话框（导入/导出）
  psd/                  自写 PSD 编解码器（无第三方依赖）
  obs/
    source_render.*     任意 obs_source → CPU RGBA8（走 OBS 自己的截图路径）
    scene_build.*       psd::Document → OBS 场景
    scene_export.*      OBS 场景 → psd::Document
    psd_ignore.*        采集/浏览器/视频源的忽略规则
tests/                  PSD 往返、独立解析、fuzz 测试
third_party/            stb、simde、OBS 头文件、Qt 头文件
```

## 已知限制

* 只支持 8 位/通道的 PSD（16/32 位文件会明确报错），PSB 大文件不支持。
* 旋转的源导出时用旋转后的外接矩形近似（PSD 图层不记录旋转）。
* 混合模式只往返 Normal / Multiply / Screen / Overlay / Dissolve 等基础项，其余落回 Normal。
* 不透明度在导入时烘焙进 PNG alpha（OBS 公共 API 没有场景项透明度）。


---

## English

**obs-Import-psd** is a macOS plugin for OBS Studio 32 that imports Photoshop documents
as layered scenes and exports scenes back to PSD.

### Features

* **Tools → Import PSD as Scene…** — every `.psd` layer becomes one source in a new scene.
  Layer order matches Photoshop's layer panel exactly (the bottom-most PSD layer is the
  bottom-most source).
* **Tools → Export Scene to PSD…** — writes the current scene's sources to PSD layers,
  bottom to top, one layer per scene item.
  Live-capture sources (browser, media/video, screen/window/game capture, camera,
  virtual camera) are **skipped**; image and text layers are always exported.
  A source name containing `[keep]` forces inclusion, `[skip]` / `[ignore]` forces exclusion.
* **Text layers** are rasterised with CoreText, using the PSD's own font, size, colour and
  stroke. If the font is missing, a system CJK face is used and the import dialog lists
  what to install. An optional switch keeps text layers as editable OBS text sources.
* **Groups** become OBS groups, visibility and opacity are preserved, and blend modes
  round-trip for the common ones.

### Requirements

* macOS arm64 (Apple silicon), Apple clang
* OBS Studio 32.x installed at `/Applications/OBS.app`
* `curl` and `tar` for downloading the build dependencies

### Build

```bash
./fetch-deps.sh    # downloads OBS headers, Qt headers, simde, miniz, stb
./build.sh         # produces build/obs-psd.plugin
./tests/run_all.sh # round-trip + independent verifier + fuzz
```

Install by copying `build/obs-psd.plugin` to:

```
~/Library/Application Support/obs-studio/plugins/
```

### Third-party code

Nothing under `third_party/` is committed — it all belongs to other projects:

| Dependency | Source | Licence |
|---|---|---|
| libobs / obs-frontend-api headers | [obs-studio](https://github.com/obsproject/obs-studio) 32.2.2 | GPL-2.0-or-later |
| Qt headers (Core/Gui/Widgets) | Qt framework bundled with OBS.app | LGPL-3.0 |
| simde | [simd-everywhere/simde](https://github.com/simd-everywhere/simde) 0.8.2 | MIT |
| miniz | [richgel999/miniz](https://github.com/richgel999/miniz) 3.0.2 | MIT / public domain |
| stb_image_write.h | [nothings/stb](https://github.com/nothings/stb) | public domain / MIT |

`fetch-deps.sh` downloads exactly the files the build needs and records provenance in
`SOURCE.txt`. `build.sh` calls it automatically when `third_party/` is missing.

### Test status

```
tests/run_all.sh
  ROUND TRIP OK (154 checks)    our own PSD round-trip
  VERIFY OK                     cross-checked by an independent Python parser
  FUZZ OK                       20000 mutated inputs under ASAN+UBSAN, zero crashes
```

Verified against a real Photoshop CS6 document (27 records, 23 visible layers, 6 groups):
layer names, order, group structure, bounds and the 15 pixel layers match Photoshop,
and the scene survives an OBS restart.

### Known broken

See the Chinese section below for the full list — in short:

1. `text_ft2_source_v2` renders as `0x0` (six different configurations tried; the
   rasteriser path is the working one).
2. A type layer's fill colour and stroke cannot be read back from the PSD automatically
   (they live in EngineData / the lfx2 descriptor); the import dialog asks for them.
3. The text size uses empirical factors (0.7 for the em ratio, 0.1 for tracking).
4. Vector-shape layers without rasterised pixels are skipped.
5. Layer positions are baked into full-canvas bitmaps to dodge OBS group coordinates,
   which makes the PNGs larger.

### Licence

GPL-2.0-or-later, matching OBS Studio (the plugin links against libobs).

---

# 未修复的 Bug（截至最后一次提交）

以下是**确认存在、尚未修好**的问题。按影响排序。

## 1. `text_ft2_source_v2` 导入后渲染为 0×0（未修复）

**现象**：勾选「始终使用 OBS 文本源」后，文字图层能创建出 `text_ft2_source_v2` 源，
但源尺寸恒为 `0x0`，画面上是空的。

**已尝试且失败**（日志确认每一步都生效）：

| 尝试 | 结果 |
|---|---|
| `obs_data_set_string("font", PostScript名)` | 0×0 |
| 显式设 `size`（键名从 `font_size` 更正为 `size`） | 0×0 |
| 设 `face`（字体家族名）+ `style` | 0×0 |
| 设 `use_color`、`text_file=""`、`custom_width` | 0×0 |
| 先 `obs_source_create()` 再 `obs_source_update()` | 0×0 |

**日志样例**：
```
[obs-psd] OBS text source '名人名言' '名人名言' face='ShanghaiFace-Zen-BoldGB2.0'
          size=70 -> 0x0
```

**已排除**：
* 字体文件本身正常（标准 TrueType，`CTFontManager` 可见，CoreText 能渲染中文）
* 插件二进制正确链接了 CoreText（`otool -L` 确认）
* 源码/构建产物/已安装文件 MD5 一致

**尚未排查**：OBS 文本源内部 FreeType 的字体查找路径、
它在 macOS 下遍历字体用的是 `CTFontManagerCopyAvailableFontFamilyNames`
还是别的来源。

**临时规避**：不勾选「始终使用 OBS 文本源」（默认），文字走 CoreText 渲染成图片，
这条路径已验证正常。

## 2. 文本图层的填充色与描边无法从 PSD 自动读取（未修复）

**现象**：文字颜色和描边只能靠导入对话框里手填，不能从 PSD 自动还原。

**原因**：这些值藏在两层自定义二进制格式里：

* **填充色**：`TySh` 块的 EngineData 树，路径为
  `/EngineDict/StyleRun/RunArray/<<.../StyleSheetData/.../FillColor`。
  已确认值是 `[alpha, R, G, B]` 顺序（样例文件是 `[1.0, .702, .596, .706]`
  = 淡紫 `#B398B4`），但遍历器无法稳定走到那个叶子节点。
* **描边**：`lfx2` 块的 descriptor（`FrFX`）。已能用 Python 逐字节读出
  `enab=1, Style=OutF, Sz=16px, Clr=RGB(255,255,255)`，
  但 `src/psd/psd_descriptor.{h,cpp}` 的 C++ 实现解析不到那么深。

**当前行为**：导入对话框提供「填充颜色 / 白色描边 / 描边宽度 / 描边颜色」四项手动设置，
默认值 `#CC99CC` + 白色 16px 对应样例文件。

## 3. 文本字号依赖经验系数（未修复，属于近似）

**现象**：PSD 里的 `FontSize` 是 em 大小，中文字形只占 em 的约 70%。
代码乘了 `0.7` 作为补偿，并对 tracking 乘 `0.1`（PSD 存 -300，原值应用会把
275px 宽的框缩到 215px）。

**验证结果**：样例文件里「名人名言」渲染后占 `x=[249,520]`，
图层框是 `x=[246,521]` —— 差 1~3px。

**风险**：这两个系数是针对样例文件调出来的，其他字体/字号可能偏。
更稳妥的做法是按图层框反算字号，尚未实现。

## 4. 矢量形状图层不导入（未修复）

**现象**：`vscg`（矢量形状）图层没有像素数据，导入时跳过并记一条日志。

**注意**：实际测试中发现，样例文件的「圆角矩形」等图层**是有像素数据的**
（走像素路径，导入正常）。真正没有像素数据的只有文本层。
所以这条影响面比预期小 —— 只在图层确实是纯矢量、未栅格化时触发。

## 5. 分组坐标系下的位置（已通过改为全画布贴图规避）

**历史问题**：早期版本用「图层尺寸的 PNG + 场景项 pos」定位，在 OBS 分组里位置会偏
（实测 `(63,136)` 变成 `(0,31)`）。曾尝试「修改组项变换」，但日志显示组本来就在
`(0,0)`，改不动。

**当前做法**：每个图层导出为**整幅画布尺寸**的 PNG，内容直接画在正确位置，
场景项恒为 `pos=(0,0), scale=fit, bounds=NONE`。位置因此不再依赖 OBS 的任何坐标计算。

**遗留风险**：画布很大时 PNG 会比较大（1920×1080 RGBA 未压缩约 8MB/层）。

---

## 验证状态

```
tests/run_all.sh
  ROUND TRIP OK (154 checks)    自写 PSD 往返
  VERIFY OK                     独立 Python 解析器交叉验证
  FUZZ OK                       20000 变异输入，ASAN+UBSAN 零崩溃
```

真实文件验证（`杂谈PSD022.psd`，Photoshop CS6 保存，27 条记录 / 23 个可见图层 / 6 个分组）：

* ✅ 图层名、顺序、分组结构、bounds 全部正确
* ✅ 15 个像素图层画面与 Photoshop 一致
* ✅ 位置正确（改为全画布贴图后）
* ✅ 重启 OBS 后图层仍存在（改用 `obs_source_create` 后可序列化）
* ⚠️ 文本图层的颜色/描边需手动在导入对话框设置
