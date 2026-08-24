import os

cpp = 'pico.cpp'
with open(cpp, 'rb') as f:
    data = f.read()

utf8_before = sum(1 for i in range(len(data)-2)
                  if 0xE0<=data[i]<=0xEF and 0x80<=data[i+1]<=0xBF and 0x80<=data[i+2]<=0xBF)
print(f'BEFORE: UTF-8={utf8_before}')

# ============================================================
# 1. 加 g_cpu_ema 静态变量
# ============================================================
anchor1 = b'static uint32_t        g_fps_smoothed    = 60;\r\n'
assert data.find(anchor1) != -1
repl1 = anchor1 + (
    b'// EMA-smoothed CPU utilization (fraction in [0,1]); alpha 0.1 -> ~10-frame time constant.\r\n'
    b'// Replaces 1-second busy-window so LCD reflects CPU changes within ~150ms.\r\n'
    b'static float            g_cpu_ema         = 0.0f;\r\n'
)
data = data.replace(anchor1, repl1, 1)

# ============================================================
# 2. B 区段 — 移除 1 秒窗口 CPU 计算
# ============================================================
anchor2 = (
    b'        if (now - last_sec_tick >= 1000) {\r\n'
    b'            // Float + round-to-nearest so idle 0.6% shows 1 (not 0 like integer int)\r\n'
    b'            float cpu_pct = (float)busy_time_us_sum * 100.0f / 1000000.0f;\r\n'
    b'            g_cpu_usage_real = (uint32_t)(cpu_pct + 0.5f);\r\n'
    b'            if (g_cpu_usage_real > 100) g_cpu_usage_real = 100;\r\n'
    b'            busy_time_us_sum = 0;\r\n'
    b'            last_sec_tick    = now;\r\n'
)
assert data.find(anchor2) != -1
repl2 = (
    b'        if (now - last_sec_tick >= 1000) {\r\n'
    b'            last_sec_tick = now;\r\n'
)
data = data.replace(anchor2, repl2, 1)

# ============================================================
# 3. 在 frame_budget_us 声明之后插入 I 块(关键顺序!)
# ============================================================
anchor3 = b'        uint32_t frame_budget_us = (is_pressing || dirty_box.w > 0 || is_full_frame || (g_app_mgr.is_home && g_pic_dog.is_playing)) ? 16666 : 50000;\r\n'
assert data.find(anchor3) != -1
repl3 = anchor3 + (
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
data = data.replace(anchor3, repl3, 1)
print('All 3 edits applied')

# 写回
with open(cpp, 'wb') as f:
    f.write(data)

utf8_after = sum(1 for i in range(len(data)-2)
                 if 0xE0<=data[i]<=0xEF and 0x80<=data[i+1]<=0xBF and 0x80<=data[i+2]<=0xBF)
print(f'AFTER: UTF-8={utf8_after}, delta: {utf8_after - utf8_before}')

# 验证顺序: I 块在 frame_budget_us 之后
i_pos = data.find(b'I. Per-frame CPU EMA')
fb_pos = data.find(b'uint32_t frame_budget_us = (is_pressing')
print(f'I block at: {i_pos}, frame_budget_us at: {fb_pos}')
print(f'Order OK: {i_pos > fb_pos}')

# 关键代码检查
checks = [
    b'static float            g_cpu_ema',
    b'g_cpu_ema = g_cpu_ema * 0.9f',
    b'last_sec_tick = now;',
]
for c in checks:
    n = data.count(c)
    print(f'  [{"OK" if n > 0 else "MISS"}] x{n}: {c.decode()[:50]}')

gone = [b'float cpu_pct = (float)busy_time_us_sum']
for c in gone:
    n = data.count(c)
    print(f'  [{"OK gone" if n == 0 else "STILL"}] x{n}: {c.decode()[:50]}')