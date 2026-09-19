# Folo AI Passport 界面资源生成

本目录存放界面图片资源的**生成脚本与源图说明**。生成的 `.c` 文件会输出到
板目录根（`../bg_shizue.c`、`../bubble_bg.c`），由 CMake `file(GLOB ... *.c)`
自动编译进固件。

## 资源清单

| 资源 | 生成物 | 尺寸 | 色彩格式 | 用途 |
| --- | --- | --- | --- | --- |
| 西施惠形象 | `../bg_shizue.c` | 240×320 | RGB565 | 全屏背景，铺满屏幕 |
| 对话气泡 | `../bubble_bg.c` | 232×96 | RGB565A8（带透明） | 文字下方常显气泡（默认程序化生成的浅色圆角气泡） |
| 背景音乐 | `../../../../assets/common/resident.ogg` | — | Opus 16kHz 单声道 | 50% 音量循环背景音 |

## 重新生成图片（替换为真实源图）

当前仓库内的 `bg_shizue.c` / `bubble_bg.c` 若为占位图，替换为真实素材步骤：

1. 准备源 PNG：
   - 西施惠形象图（建议竖构图，人物居中；脚本按 cover 方式缩放铺满 240×320）。
   - 气泡图（**带透明背景**的 PNG；脚本按 contain 方式缩放，保留四周透明）。
2. 运行（需 Pillow、pypng、lz4）：
   ```sh
   # 背景用真实源图；气泡用自带素材
   python gen_images.py --portrait 西施惠.png --bubble 气泡.png

   # 气泡不带值 = 程序化生成一个干净的浅色圆角气泡（无需素材、无版权）
   python gen_images.py --portrait 西施惠.png --bubble
   ```
3. 生成的 `.c` 会覆盖板目录根的同名文件。

### 可选参数

- `--v-anchor`：背景垂直裁剪锚点，`0` 贴顶 / `0.5` 居中 / `1` 贴底，默认 `0.42`
  （竖构图人物略微上移，保住头顶造型不被 cover 裁掉）。
- `--bubble`：给路径则用该 PNG（带透明）；不带值则程序化生成默认气泡
  （白色半透明圆角 + 柔和描边和投影，文字叠加清晰可读）。

依赖安装（若环境缺失）：
```sh
python -m pip install Pillow pypng lz4
```

## 重新生成背景音乐

从 `main/audio/Resident.mp3` 转出 50% 音量的 Opus（增益烘焙进文件，不依赖运行时音量）：
```sh
ffmpeg -i ../../../audio/Resident.mp3 -af volume=0.5 \
    -c:a libopus -b:a 16k -ac 1 -ar 16000 -frame_duration 60 \
    ../../../assets/common/resident.ogg
```
参数与现有音效管线一致（`OPUS_FRAME_DURATION_MS=60`）。
