# convert_jpg.py
# 依赖库: pip install pillow
import os
from PIL import Image, ImageEnhance

def convert_jpg_to_c(input_jpg, output_header="dog_data.h", target_w=240, target_h=240, sharpen_factor=1.2):
    # 1. 打开 JPG 图像并转为 RGB
    im = Image.open(input_jpg).convert('RGB')

    # 2. 保持最高画质缩放 (LANCZOS 重采样)
    im_resized = im.resize((target_w, target_h), Image.Resampling.LANCZOS)

    # 3. 适度锐化增强 (嵌入式小屏幕上像素密度高，微调锐度可以让毛发边缘更清晰)
    if sharpen_factor > 1.0:
        enhancer = ImageEnhance.Sharpness(im_resized)
        im_final = enhancer.enhance(sharpen_factor)
    else:
        im_final = im_resized

    pixels = list(im_final.getdata())

    # 4. 生成兼容现有 ui_picture 组件的 C 头文件
    with open(output_header, 'w', encoding='utf-8') as f:
        f.write('#pragma once\n#include <stdint.h>\n\n')
        f.write(f'#define DOG_IMG_W {target_w}\n')
        f.write(f'#define DOG_IMG_H {target_h}\n')
        f.write(f'#define DOG_IMG_FRAMES 1\n')
        f.write(f'#define DOG_IS_ANIM 0\n\n')

        f.write(f'static const uint16_t g_dog_frame_0[{target_w * target_h}] = {{\n')
        
        line_cnt = 0
        for r, g, b in pixels:
            # ⚡ 高保真带舍入补偿的 24bit -> 16bit 转换
            r5 = ((r * 31) + 127) // 255
            g6 = ((g * 63) + 127) // 255
            b5 = ((b * 31) + 127) // 255
            rgb565 = (r5 << 11) | (g6 << 5) | b5

            f.write(f'0x{rgb565:04X}, ')
            line_cnt += 1
            if line_cnt % 12 == 0:
                f.write('\n')

        f.write('\n};\n\n')
        f.write('static const uint16_t* g_dog_frames[] = {\n    g_dog_frame_0\n};\n')

    print(f"JPG 高清转换完成 -> {output_header} (单图模式, 尺寸 {target_w}x{target_h})")

if __name__ == '__main__':
    # 将你的 jpg 图片重命名为 dog.jpg 放在同一目录下
    img_file = "dog.jpg"
    if os.path.exists(img_file):
        # 尺寸可设为 240x240，或者满屏 480x320
        convert_jpg_to_c(img_file, "dog_data.h", target_w=240, target_h=240, sharpen_factor=1.2)
    else:
        print(f"未找到 {img_file}，请将 JPG 图片放入 tool 目录并命名为 dog.jpg！")