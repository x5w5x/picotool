cpp = 'pico.cpp'
with open(cpp, 'rb') as f:
    data = f.read()

utf8_before = sum(1 for i in range(len(data)-2)
                  if 0xE0<=data[i]<=0xEF and 0x80<=data[i+1]<=0xBF and 0x80<=data[i+2]<=0xBF)
print(f'BEFORE: UTF-8={utf8_before}')

# Step 1: 删除之前错误位置的 I 块(在 busy_time_us_sum 之后)
# 它现在是: "busy_time_us_sum += loop_busy_us;  // legacy..." 后跟 I 块
old_i_block = (
    b'        busy_time_us_sum += loop_busy_us;  // legacy counter, kept for future stats\r\n'
    b'\r\n'
    b'        // ================= I. Per-frame CPU EMA (real-time) =================\r\n'
    b'        // frame_busy_frac = busy / budget, smoothed with alpha 0.1 (~150ms @60fps).\r\n'
    b'        // LCD sees this via g_cpu_usage_real on the NEXT loop tick (16ms lag, imperceptible).\r\n'
    b'        {\r\n'
    b'            float frame_budget = (float)frame_budget_us;\r\n'
    b'            float frame_busy_frac = frame_budget > 0.0f ? (float)loop_busy_us / frame_budget : 0.0f;\r\n'
    b'            if (frame_busy_frac > 1.0f) frame_busy_frac = 1.0f;\r\n'
    b'            g_cpu_ema = g_cpu_ema * 0.9f + frame_busy_frac * 0.1f;\r\n'
    b'            g_cpu_usage_real = (uint32_t)(g_cpu_ema * 100.0f + 0.5f);\r\n'
    b'            if (g_cpu_usage_real > 100) g_cpu_usage_real = 100;\r\n'
    b'        }\r\n'
)
assert data.find(old_i_block) != -1, 'I block not found at wrong position'
data = data.replace(old_i_block, b'        busy_time_us_sum += loop_busy_us;  // legacy counter, kept for future stats\r\n', 1)
print('1: I block removed from wrong position')

# Step 2: 在 frame_budget_us 声明之后, sleep 之前, 插入 I 块
anchor = b'        uint32_t frame_budget_us = (is_pressing || dirty_box.w > 0 || is_full_frame || (g_app_mgr.is_home && g_pic_dog.is_playing)) ? 16666 : 50000;\r\n'
assert data.find(anchor) != -1
repl = anchor + (
    b'\r\n'
    b'        // ================= I. Per-frame CPU EMA (real-time) =================\r\n'
    b'        // frame_busy_frac = busy / budget, smoothed with alpha 0.1 (~150ms @60fps).\r\n'
    b'        // LCD sees this via g_cpu_usage_real on the NEXT loop tick (16ms lag, imperceptible).\r\n'
    b'        {\r\n'
    b'            float frame_busy_frac = (float)loop_busy_us / (float)frame_budget_us;\r\n'
    b'            if (frame_busy_frac > 1.0f) frame_busy_frac = 1.0f;\r\n'
    b'            g_cpu_ema = g_cpu_ema * 0.9f + frame_busy_frac * 0.1f;\r\n'
    b'            g_cpu_usage_real = (uint32_t)(g_cpu_ema * 100.0f + 0.5f);\r\n'
    b'            if (g_cpu_usage_real > 100) g_cpu_usage_real = 100;\r\n'
    b'        }\r\n'
)
data = data.replace(anchor, repl, 1)
print('2: I block inserted after frame_budget_us declaration')

with open(cpp, 'wb') as f:
    f.write(data)

utf8_after = sum(1 for i in range(len(data)-2)
                 if 0xE0<=data[i]<=0xEF and 0x80<=data[i+1]<=0xBF and 0x80<=data[i+2]<=0xBF)
print(f'AFTER: UTF-8={utf8_after}, delta: {utf8_after - utf8_before}')

# 验证顺序: I 块应在 frame_budget_us 之后
i_pos = data.find(b'I. Per-frame CPU EMA')
fb_pos = data.find(b'uint32_t frame_budget_us = (is_pressing')
print(f'I block at: {i_pos}')
print(f'frame_budget_us at: {fb_pos}')
print(f'Order OK: {i_pos > fb_pos}')