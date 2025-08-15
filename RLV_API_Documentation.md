# ST7789 RLV API 文档

## 概述

RLV (Run Length Video) 是一种专为微控制器设计的双色视频格式，使用行程编码压缩技术。ST7789 驱动提供了三个主要的 RLV 相关函数，用于播放、解码和获取 RLV 视频信息。

## API 函数列表

### 1. `display.rlv_play(rlv_data, x, y, [fg_color], [bg_color], [loop_count], [cache_size], [fps_print])`

播放 RLV 视频文件或数据。

#### 参数

**必须的位置参数：**
- `rlv_data` (str/bytes): RLV 文件路径或字节数据
- `x` (int): 显示起始 X 坐标
- `y` (int): 显示起始 Y 坐标

**可选的关键字参数：**
- `fg_color` (int): 前景色，默认为 `st7789.WHITE` (0xFFFF)
- `bg_color` (int): 背景色，默认为 `st7789.BLACK` (0x0000)
- `loop_count` (int): 循环播放次数，默认为 1
- `cache_size` (int): 数据缓存大小（字节），默认为 1024
- `fps_print` (bool): 是否打印 FPS 统计信息，默认为 False

#### 使用示例

```python
import st7789

# 基本播放
display.rlv_play("video.rlv", 0, 0)

# 指定颜色
display.rlv_play("video.rlv", 10, 20, st7789.RED, st7789.BLUE)

# 循环播放 3 次
display.rlv_play("video.rlv", 0, 0, loop_count=3)

# 使用更大的缓存提高性能
display.rlv_play("video.rlv", 0, 0, cache_size=4096)

# 启用 FPS 统计
display.rlv_play("video.rlv", 0, 0, fps_print=True)

# 混合使用位置参数和关键字参数
display.rlv_play("video.rlv", 0, 0, st7789.GREEN, cache_size=2048, fps_print=True)

# 使用字节数据
with open("video.rlv", "rb") as f:
    data = f.read()
display.rlv_play(data, 0, 0)
```

#### 返回值
无返回值

#### 注意事项
- 支持文件路径和字节数据两种输入方式
- 缓存大小影响播放性能，特别是对于复杂帧
- FPS 统计会在控制台输出性能信息

---

### 2. `display.rlv_decode_frame(rlv_data, frame_index, [fg_color], [bg_color])`

解码 RLV 视频的单个帧到内存缓冲区。

#### 参数

**必须的位置参数：**
- `rlv_data` (str/bytes): RLV 文件路径或字节数据
- `frame_index` (int): 要解码的帧索引（从 0 开始）

**可选的位置参数：**
- `fg_color` (int): 前景色，默认为 `st7789.WHITE` (0xFFFF)
- `bg_color` (int): 背景色，默认为 `st7789.BLACK` (0x0000)

#### 使用示例

```python
# 解码第一帧
frame_buffer = display.rlv_decode_frame("video.rlv", 0)

# 解码第 5 帧并指定颜色
frame_buffer = display.rlv_decode_frame("video.rlv", 4, st7789.RED, st7789.YELLOW)

# 使用字节数据
with open("video.rlv", "rb") as f:
    data = f.read()
frame_buffer = display.rlv_decode_frame(data, 0)
```

#### 返回值
- `memoryview`: 包含解码后帧数据的内存视图对象，每个像素为 16 位颜色值

#### 注意事项
- 返回的缓冲区大小为 `width * height * 2` 字节
- 帧索引必须在有效范围内（0 到 frame_count-1）
- 返回的内存视图可以直接用于其他显示函数

---

### 3. `display.rlv_info(rlv_data)`

获取 RLV 视频文件的基本信息。

#### 参数

**必须的位置参数：**
- `rlv_data` (str/bytes): RLV 文件路径或字节数据

#### 使用示例

```python
# 获取文件信息
info = display.rlv_info("video.rlv")
print(f"分辨率: {info['width']}x{info['height']}")
print(f"帧数: {info['frame_count']}")

# 使用字节数据
with open("video.rlv", "rb") as f:
    data = f.read()
info = display.rlv_info(data)
```

#### 返回值
- `dict`: 包含视频信息的字典，包含以下键：
  - `width` (int): 视频宽度
  - `height` (int): 视频高度
  - `frame_count` (int): 总帧数
  - `unit_bits` (int): 单位编码位数
  - `frame_table_bits` (int): 帧表编码位数

#### 注意事项
- 这是一个轻量级函数，只读取文件头部信息
- 可用于在播放前验证视频格式和获取基本参数

---

## 性能优化建议

### 1. 缓存大小调优
```python
# 对于简单视频，使用较小缓存
display.rlv_play("simple.rlv", 0, 0, cache_size=512)

# 对于复杂视频，使用较大缓存
display.rlv_play("complex.rlv", 0, 0, cache_size=4096)
```

### 2. FPS 监控
```python
# 启用 FPS 统计来监控性能
display.rlv_play("video.rlv", 0, 0, fps_print=True)
```

### 3. 预加载检查
```python
# 播放前检查视频信息
info = display.rlv_info("video.rlv")
if info['frame_count'] > 100:
    # 对于长视频使用更大缓存
    display.rlv_play("video.rlv", 0, 0, cache_size=8192)
else:
    display.rlv_play("video.rlv", 0, 0)
```

## 错误处理

所有函数在遇到错误时会抛出相应的 MicroPython 异常：

```python
try:
    display.rlv_play("nonexistent.rlv", 0, 0)
except OSError as e:
    print(f"文件错误: {e}")
except ValueError as e:
    print(f"参数错误: {e}")
except Exception as e:
    print(f"其他错误: {e}")
```

## 常见用例

### 1. 简单动画播放
```python
# 播放简单的 logo 动画
display.rlv_play("logo.rlv", 50, 50, st7789.BLUE, st7789.WHITE)
```

### 2. 循环背景动画
```python
# 无限循环播放背景动画
display.rlv_play("background.rlv", 0, 0, loop_count=0)  # 0 表示无限循环
```

### 3. 帧缓冲操作
```python
# 解码帧并进行后处理
frame = display.rlv_decode_frame("video.rlv", 10)
# 可以对 frame 进行进一步处理
# 然后使用其他显示函数显示
```

### 4. 视频信息显示
```python
# 显示视频详细信息
info = display.rlv_info("video.rlv")
print(f"视频: {info['width']}x{info['height']}, {info['frame_count']} 帧")
```

## 技术细节

- **RLV 格式**: 使用行程编码压缩的双色视频格式
- **内存管理**: 自动管理内部缓冲区，用户无需手动释放
- **文件支持**: 支持从文件系统读取和内存数据两种方式
- **颜色格式**: 使用 16 位 RGB565 颜色格式
- **性能**: 针对微控制器优化，支持实时播放

## 版本历史

- **v1.0**: 基础 RLV 播放功能
- **v1.1**: 添加运行时缓存配置和 FPS 统计
- **v1.2**: 改进参数处理，支持关键字参数