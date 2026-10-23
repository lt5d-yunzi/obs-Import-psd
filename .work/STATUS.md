# 项目状态（最终）

## 已完成并通过验证
- PSD 编解码器 `src/psd/`（自写，无第三方依赖）
  - 往返测试 154 项断言全绿（`build/tests/psd_test`）
  - 独立 Python 解析器 `tests/verify_psd.py` 校验通过
  - 20000 次变异 fuzz，ASAN+UBSAN 下 0 崩溃 / 0 挂起 / 0 静默失败
- OBS 侧：
  - `src/obs/source_render.*` 任意源 → CPU RGBA8（走 obs_source_video_render + texrender/stagesurface，直通 alpha）
  - `src/obs/scene_build.*` PSD → OBS 场景（每层一个 image_source，保序，组/混合/可见性/等待纹理加载）
  - `src/obs/scene_export.*` OBS 场景 → psd::Document（按场景项实际变换渲染、组展开、隐藏项导出为隐藏图层）
  - `src/obs/psd_ignore.*` 忽略规则：浏览器/视频/采集类源跳过；图片与文本始终导出；`[keep]`/`[skip]` 可覆盖
- UI `src/ui/psd_ui.cpp`（Qt 对话框：导入、导出、导出报告）
- 构建：`build.sh` → `build/obs-psd.plugin`；`install.sh` 复制到 dist/plugins
- 插件二进制依赖核对：libobs + obs-frontend-api + libc++/libSystem；引用的 63 个 OBS 符号与全部 Qt 符号都能在 OBS.app 中找到

## 未能验证（环境限制）
- 没有可交互 OBS GUI 会话：运行时行为（场景外观、图像加载时序、真实采集源渲染）未实测
- 安装到 `~/Library/Application Support/obs-studio/plugins/` 被 macOS 隐私保护（TCC）拦截，需用户手动复制

## 环境注意
- 默认 SDK（MacOSX27）的 libSystem.tbd 损坏，必须 `-isysroot .../MacOSX15.4.sdk -Xlinker -ld_classic`
- Qt / OBS 头文件已 vendored 在 third_party/（无需 OBS 源码树）
