# obs-psd —— OBS Studio 的 PSD 导入 / 导出插件

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

### 构建要求

* macOS arm64、Apple clang
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
* 未能做运行时验证：本机没有可交互的 OBS 会话，验证止于编译、链接与 PSD 编解码的单测/fuzz。
