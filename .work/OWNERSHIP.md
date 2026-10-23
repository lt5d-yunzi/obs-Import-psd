# PSD 编解码器当前状态（03:10）

## 结论
`src/psd/**` 的读写器**仍未通过往返测试**。当前工作树里的版本是 Lead 的 WIP（含调试残留，已清理一部分）。
已有两份互不兼容的实现：

1. Lead 的 WIP（工作树 + `.work/lead_wip/`）
   - 写入器结构性修复：`layer&mask` 长度包含 global layer mask info；composite 平面不再重复写压缩标签；composite 通道数 = max(ri,gi,bi,ai)+1。
   - 读取器：int16 层计数（负号允许）、`layout_end = layer_start + layer_len` 主路径 + 合法压缩标签探测回退。
   - 状态：`wrote /tmp/psd_rt.psd` 后 `READ FAILED: layer bounds too large (layer record 0)`。

2. 分身 742e 的版本（`.scratch/psd_final/`）
   - 更完整的加固（长度校验、RLE 上限、PSB/深度检查、luni、lsct 分组、premul/unpremul）
   - 但它在本地也停在：`READ FAILED: truncated channel compression tag`。

## 已验证正确的部分（用独立 Python 解析 /tmp/tiny3.psd 确认）
写入器产物的**段布局**是对的：
- header → colour mode(0) → image resources(0) → layer&mask(164) → layer info(156) → composite(tag 0x0001)

## 关键待办
1. 层记录第一个字段的读取仍错位（Lead 版报 bounds too large；分身版报 channel compression tag 截断）。
   两者都说明 `parse_layers` 在进入第一条记录时读到的不是 `t,l,b,r`。
2. 修复后必须：往返测试通过 → tests/verify_psd.py 独立校验通过 → ≥20000 变异输入的 ASAN/UBSAN fuzz 通过。

## 所有权
- `src/psd/**`、`tests/**`：只允许一个写入方。
- Lead 已停止编辑 `src/psd/**`（03:10 起）。
