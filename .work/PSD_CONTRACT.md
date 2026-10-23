# PSD 段布局契约（Lead 实测确认，供编解码器实现使用）

## 文件结构
```
header(26) → colour mode data(len) → image resources(len) → layer&mask(len) → composite
```

## layer & mask 段
```
layer&mask_len 覆盖：[ layer_info_len(4) + layer_info 内容 ] + [ global_mask_len(4) + 内容 ]
layer_info 内容：[ int16 count ][ record_0 .. record_{n-1} ][ channel data ]
```
- `layer_info_len` **只**覆盖 count + records + channel data。
- `global layer mask info` 在 layer_info 之后，**不计入** layer_info_len，但计入 layer&mask_len。
- composite 起点 = `layer_start + layer_len`（layer_start 指向 layer&mask 长度字段之后）。

## 每个 layer record
```
int32 top, left, bottom, right
uint16 channel_count
[ int16 channel_id, uint32 channel_length ] * channel_count   (channel_length 含 2 字节压缩标签)
char blend_key[4]  (前面 4 字节 "8BIM")  → 实为: "8BIM" + blend_key
uint8 opacity, clipping, flags, filler
uint32 extra_len
  uint32 mask_len + data
  uint32 blending_ranges_len + data
  pascal string name (padded to 4)
  8BIM/luni (uint32 charcount, UTF-16BE chars)
  8BIM/lsct (uint32 divider type)  - 组
channel data:
  uint16 compression (0=raw,1=RLE)
  若 RLE: uint16 row_count * height，再逐行数据
```

## 已确认的 bug（造成 lead 版本读取失败）
- writer 把 global layer mask info 算进了 layer_info_len（多 4 字节），导致读侧从 +4 处开始读 record。
- 实测 tiny 文档：layer&mask=164, layer info=156，而 layer info 实际内容 152 字节。
