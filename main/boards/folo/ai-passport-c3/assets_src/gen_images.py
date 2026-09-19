#!/usr/bin/env python3
"""Folo AI Passport 界面图片资源生成脚本。

把源 PNG 裁剪/缩放到目标尺寸后，调用仓库自带的 LVGLImage.py 转成
LVGL 可直接使用的 C 数组（编入固件 flash .rodata，无需运行时解码）。

目标板：folo/ai-passport-c3，240x320 ST7789P3，RGB565，无 PSRAM。

用法：
    python gen_images.py \
        --portrait <西施惠源图.png> \
        --bubble   <气泡源图.png>

生成物（输出到板目录根，供 CMake GLOB 自动编译）：
    ../bg_shizue.c   240x320  RGB565     全屏背景
    ../bubble_bg.c   气泡尺寸 RGB565A8   带透明的对话气泡

依赖：Pillow。LVGLImage.py 位于 scripts/Image_Converter/。
"""

import argparse
import os
import subprocess
import sys
import tempfile

from PIL import Image

# 目标屏幕尺寸
SCREEN_W = 240
SCREEN_H = 320

# 气泡目标尺寸（贴合对话文字区，宽度略窄于屏宽，扁平形状）
BUBBLE_W = 232
BUBBLE_H = 96

HERE = os.path.dirname(os.path.abspath(__file__))
# 板目录根（生成的 .c 放这里）
BOARD_DIR = os.path.dirname(HERE)
# 仓库根：main/boards/folo/ai-passport-c3 -> 上溯 4 级
REPO_ROOT = os.path.abspath(os.path.join(BOARD_DIR, "..", "..", "..", ".."))
LVGL_IMAGE_PY = os.path.join(REPO_ROOT, "scripts", "Image_Converter", "LVGLImage.py")


def cover_resize(img: Image.Image, target_w: int, target_h: int,
                 v_anchor: float = 0.5) -> Image.Image:
    """按 cover 方式缩放并裁剪，铺满目标尺寸不留边。

    v_anchor 控制垂直裁剪锚点：0.0 贴顶、0.5 居中、1.0 贴底。
    人物竖构图时略微上移锚点（如 0.42）可保住头顶造型不被裁掉。
    """
    src_w, src_h = img.size
    scale = max(target_w / src_w, target_h / src_h)
    new_w, new_h = round(src_w * scale), round(src_h * scale)
    img = img.resize((new_w, new_h), Image.LANCZOS)
    left = (new_w - target_w) // 2
    top = round((new_h - target_h) * v_anchor)
    top = max(0, min(top, new_h - target_h))
    return img.crop((left, top, left + target_w, top + target_h))


def contain_resize(img: Image.Image, target_w: int, target_h: int) -> Image.Image:
    """按 contain 方式缩放到目标尺寸内，保持完整（用于气泡，保留透明边）。"""
    src_w, src_h = img.size
    scale = min(target_w / src_w, target_h / src_h)
    new_w, new_h = round(src_w * scale), round(src_h * scale)
    img = img.resize((new_w, new_h), Image.LANCZOS)
    canvas = Image.new("RGBA", (target_w, target_h), (0, 0, 0, 0))
    canvas.paste(img, ((target_w - new_w) // 2, (target_h - new_h) // 2), img
                 if img.mode == "RGBA" else None)
    return canvas


def run_lvgl_convert(png_path: str, cf: str, out_dir: str):
    """调用 LVGLImage.py 生成 C 数组。输出文件名由输入 PNG 名决定。"""
    cmd = [
        sys.executable, LVGL_IMAGE_PY,
        "--ofmt", "C",
        "--cf", cf,
        "-o", out_dir,
        png_path,
    ]
    print("  运行:", " ".join(cmd))
    subprocess.run(cmd, check=True)
    # 规范化 LVGL 头包含：ESP-IDF 下统一用 <lvgl.h>，而非默认的 "lvgl/lvgl.h"。
    base = os.path.splitext(os.path.basename(png_path))[0]
    out_c = os.path.join(out_dir, base + ".c")
    _normalize_lvgl_include(out_c)


def _normalize_lvgl_include(c_file: str):
    """把生成文件顶部的 LVGL 头包含块替换为 ESP-IDF 通用的 <lvgl.h>。"""
    with open(c_file, "r") as f:
        text = f.read()
    block = (
        '#if defined(LV_LVGL_H_INCLUDE_SIMPLE)\n'
        '#include "lvgl.h"\n'
        '#elif defined(LV_BUILD_TEST)\n'
        '#include "../lvgl.h"\n'
        '#else\n'
        '#include "lvgl/lvgl.h"\n'
        '#endif\n'
    )
    if block in text:
        text = text.replace(block, "#include <lvgl.h>\n", 1)
        with open(c_file, "w") as f:
            f.write(text)
        print(f"  规范化 LVGL 头包含 -> <lvgl.h>: {os.path.basename(c_file)}")


def gen_portrait(src: str, v_anchor: float = 0.42):
    print(f"[背景] 处理 {src} -> {SCREEN_W}x{SCREEN_H} RGB565 (v_anchor={v_anchor})")
    img = Image.open(src).convert("RGB")
    img = cover_resize(img, SCREEN_W, SCREEN_H, v_anchor=v_anchor)
    with tempfile.TemporaryDirectory() as td:
        tmp_png = os.path.join(td, "bg_shizue.png")
        img.save(tmp_png)
        run_lvgl_convert(tmp_png, "RGB565", BOARD_DIR)
    print(f"  生成 {os.path.join(BOARD_DIR, 'bg_shizue.c')}")


def make_default_bubble(target_w: int, target_h: int) -> Image.Image:
    """程序化生成一个干净的浅色圆角气泡（带透明背景）。

    白色半透明填充 + 柔和描边，四角圆润；文字（近黑色）叠在上面清晰可读，
    底层背景图仍能透出一点。无需外部素材，也不涉及第三方版权。
    """
    from PIL import ImageDraw, ImageFilter

    # 用 4 倍超采样绘制再缩小，得到平滑抗锯齿的圆角。
    ss = 4
    w, h = target_w * ss, target_h * ss
    radius = 28 * ss
    margin = 3 * ss  # 四周留一点透明边，避免描边贴到图片边缘被裁

    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))

    # 柔和投影：先画一个稍大的深色圆角矩形并模糊，营造浮起感。
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sdraw = ImageDraw.Draw(shadow)
    sdraw.rounded_rectangle(
        [margin, margin + 4 * ss, w - margin, h - margin + 4 * ss],
        radius=radius, fill=(20, 40, 36, 90))
    shadow = shadow.filter(ImageFilter.GaussianBlur(6 * ss))
    canvas = Image.alpha_composite(canvas, shadow)

    # 气泡主体：接近白色、约 88% 不透明，让底层西施惠隐约透出。
    body = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bdraw = ImageDraw.Draw(body)
    bdraw.rounded_rectangle(
        [margin, margin, w - margin, h - margin],
        radius=radius, fill=(252, 253, 250, 224),
        outline=(255, 255, 255, 255), width=2 * ss)
    canvas = Image.alpha_composite(canvas, body)

    return canvas.resize((target_w, target_h), Image.LANCZOS)


def gen_bubble(src: str = None):
    if src:
        print(f"[气泡] 处理 {src} -> {BUBBLE_W}x{BUBBLE_H} RGB565A8")
        img = Image.open(src).convert("RGBA")
        img = contain_resize(img, BUBBLE_W, BUBBLE_H)
    else:
        print(f"[气泡] 程序化生成默认气泡 -> {BUBBLE_W}x{BUBBLE_H} RGB565A8")
        img = make_default_bubble(BUBBLE_W, BUBBLE_H)
    with tempfile.TemporaryDirectory() as td:
        tmp_png = os.path.join(td, "bubble_bg.png")
        img.save(tmp_png)
        run_lvgl_convert(tmp_png, "RGB565A8", BOARD_DIR)
    print(f"  生成 {os.path.join(BOARD_DIR, 'bubble_bg.c')}")


def main():
    parser = argparse.ArgumentParser(description="生成 Folo UI 图片 C 数组资源")
    parser.add_argument("--portrait", help="西施惠源图 PNG（全屏背景）")
    parser.add_argument("--v-anchor", type=float, default=0.42,
                        help="背景垂直裁剪锚点：0 贴顶 / 0.5 居中 / 1 贴底，默认 0.42")
    parser.add_argument("--bubble", nargs="?", const="__DEFAULT__",
                        help="气泡：给路径则用该 PNG（带透明）；不带值则程序化生成默认气泡")
    args = parser.parse_args()

    if not args.portrait and args.bubble is None:
        parser.error("至少提供 --portrait 或 --bubble 之一")

    if not os.path.exists(LVGL_IMAGE_PY):
        sys.exit(f"找不到 LVGLImage.py: {LVGL_IMAGE_PY}")

    if args.portrait:
        gen_portrait(args.portrait, v_anchor=args.v_anchor)
    if args.bubble is not None:
        gen_bubble(None if args.bubble == "__DEFAULT__" else args.bubble)

    print("完成。生成的 .c 已放入板目录，CMake 会自动编译。")


if __name__ == "__main__":
    main()
