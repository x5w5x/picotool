#!/usr/bin/env python3
"""Generate a minimal TabView-based application skeleton."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


C_TEMPLATE = r'''#include "{header}"
#include "app_shared_ram.h"
#include "ui_tabview.h"
#include "ui_draw.h"
#include "lcd.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *s_{symbol}_tabs[] = {{
    "主页",
    "设置",
    "关于",
}};

#define {macro}_TAB_COUNT ((uint8_t)(sizeof(s_{symbol}_tabs) / sizeof(s_{symbol}_tabs[0])))

typedef struct {{
    // 必须放在第一个字段，app_manager 依赖它获取脏矩形
    ui_rect_t dirty_rect;
    ui_tabview_t tabview;
    uint8_t last_tab;
}} {symbol}_data_t;

static app_t s_{symbol}_app;

static void mark_full_redraw({symbol}_data_t *data) {{
    data->dirty_rect = ui_rect_make(0, 0, LCD_WIDTH, LCD_HEIGHT);
}}

static void {symbol}_tab_changed(ui_tabview_t *tv, uint8_t old_tab,
                                 uint8_t new_tab, void *user_data) {{
    (void)old_tab;
    (void)new_tab;
    {symbol}_data_t *data = ({symbol}_data_t *)user_data;
    if (data) data->dirty_rect = tv->content_rect;
}}

static void {symbol}_init(app_t *self) {{
    self->user_data = g_static_app_ram;
}}

static void {symbol}_enter(app_t *self) {{
    {symbol}_data_t *data = ({symbol}_data_t *)g_static_app_ram;
    memset(data, 0, sizeof(*data));
    self->user_data = data;
    mark_full_redraw(data);
    ui_tabview_init(&data->tabview, 0, 32, LCD_WIDTH, LCD_HEIGHT - 32,
                    110, 48, s_{symbol}_tabs, {macro}_TAB_COUNT, 0,
                    {symbol}_tab_changed);
    data->tabview.user_data = data;
}}

static void {symbol}_exit(app_t *self) {{
    self->user_data = NULL;
}}

static void {symbol}_update(app_t *self, uint32_t now_tick) {{
    (void)self;
    (void)now_tick;
}}

static bool {symbol}_process_touch(app_t *self, bool is_touching,
                                   int16_t x, int16_t y, uint32_t now_tick) {{
    {symbol}_data_t *data = ({symbol}_data_t *)self->user_data;
    if (!data) return false;
    if (ui_tabview_process_touch(&data->tabview, is_touching, x, y, now_tick)) {{
        if (data->tabview.active_tab_idx != data->last_tab) {{
            data->last_tab = data->tabview.active_tab_idx;
            data->dirty_rect = data->tabview.rect;
        }} else {{
            data->dirty_rect = ui_tabview_get_bar_dirty_rect(&data->tabview);
        }}
        return true;
    }}
    return false;
}}

static void {symbol}_draw(app_t *self, ui_tile_t *tile) {{
    {symbol}_data_t *data = ({symbol}_data_t *)self->user_data;
    if (!data) return;
    ui_rect_t body = ui_rect_make(0, 32, LCD_WIDTH, LCD_HEIGHT - 32);
    ui_rect_t overlap;
    if (ui_rect_intersect(&tile->rect, &body, &overlap))
        ui_draw_fill_rect(tile, &overlap, 0xFFFF);
    ui_tabview_draw(tile, &data->tabview);
    if (!ui_rect_intersect(&tile->rect, &data->tabview.content_rect, &overlap)) return;
    const char *title = "主页";
    switch (data->tabview.active_tab_idx) {{
    case 1: title = "设置"; break;
    case 2: title = "关于"; break;
    default: break;
    }}
    ui_draw_string(tile,
                   data->tabview.content_rect.x + 20,
                   data->tabview.content_rect.y + 20,
                   title, FONT_20, 0x0000, 0xFFFF);
}}

app_t *app_{symbol}_get_instance(void) {{
    s_{symbol}_app.id = APP_ID_CUSTOM_0;
    s_{symbol}_app.name = "{display_name}";
    s_{symbol}_app.init = {symbol}_init;
    s_{symbol}_app.enter = {symbol}_enter;
    s_{symbol}_app.exit = {symbol}_exit;
    s_{symbol}_app.update = {symbol}_update;
    s_{symbol}_app.process_touch = {symbol}_process_touch;
    s_{symbol}_app.draw = {symbol}_draw;
    return &s_{symbol}_app;
}}
'''

H_TEMPLATE = '''#ifndef {guard}
#define {guard}

#include "app_base.h"

#ifdef __cplusplus
extern "C" {{
#endif

app_t *app_{symbol}_get_instance(void);

#ifdef __cplusplus
}}
#endif

#endif
'''


def normalize(raw: str) -> str:
    value = re.sub(r"[^a-zA-Z0-9]+", "_", raw.strip()).strip("_").lower()
    if not value or value[0].isdigit():
        raise ValueError("功能名必须以字母开头，例如 led 或 uart")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate a TabView app skeleton")
    parser.add_argument("name", help="功能名，例如 led、uart")
    parser.add_argument("--display-name", help="桌面显示名称，默认使用功能名")
    parser.add_argument("--out-dir", default="app", help="输出目录，默认 app")
    parser.add_argument("--register", action="store_true",
                        help="将生成的 .c 自动加入 app/CMakeLists.txt")
    args = parser.parse_args()

    symbol = normalize(args.name)
    display_name = args.display_name or symbol
    macro = "APP_" + symbol.upper()
    guard = macro + "_H"
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    c_path = out / f"app_{symbol}.c"
    h_path = out / f"app_{symbol}.h"
    if c_path.exists() or h_path.exists():
        raise SystemExit(f"输出文件已存在: {c_path} 或 {h_path}")
    values = dict(header=h_path.name, symbol=symbol, macro=macro,
                  guard=guard, display_name=display_name)
    # Keep app/app_template.c as the single source of truth for comments,
    # lifecycle conventions, TabView flow, and component examples.
    template_path = Path(__file__).resolve().parents[1] / "app" / "app_template.c"
    if not template_path.exists():
        raise SystemExit(f"找不到模板文件: {template_path}")
    source = template_path.read_text(encoding="utf-8")
    source = source.replace('"app_template.h"', f'"{h_path.name}"')
    source = source.replace('"app_settings.h"', f'"{h_path.name}"')
    # Support both the original app_template.c and the current settings-based
    # mother template without touching Chinese comments or user-facing text.
    source = source.replace("app_template", f"app_{symbol}")
    source = source.replace("app_settings", f"app_{symbol}")
    source = source.replace("settings_data_t", f"{symbol}_data_t")
    source = source.replace("settings_init", f"{symbol}_init")
    source = source.replace("settings_enter", f"{symbol}_enter")
    source = source.replace("settings_exit", f"{symbol}_exit")
    source = source.replace("settings_update", f"{symbol}_update")
    source = source.replace("settings_process_touch", f"{symbol}_process_touch")
    source = source.replace("settings_draw", f"{symbol}_draw")
    source = source.replace("s_settings", f"s_{symbol}")
    source = source.replace('"系统设置"', f'"{display_name}"')
    c_path.write_text(source, encoding="utf-8")
    h_path.write_text(H_TEMPLATE.format(**values), encoding="utf-8")
    print(f"generated {c_path}")
    print(f"generated {h_path}")
    if args.register:
        cmake_path = out / "CMakeLists.txt"
        if not cmake_path.exists():
            raise SystemExit(f"找不到 {cmake_path}，无法自动注册")
        cmake = cmake_path.read_text(encoding="utf-8")
        source = c_path.name
        if source not in cmake:
            marker = "add_library(appmgr STATIC"
            pos = cmake.find(marker)
            if pos < 0:
                raise SystemExit(f"{cmake_path} 中找不到 add_library(appmgr STATIC)")
            line_end = cmake.find("\n", pos)
            cmake = cmake[:line_end + 1] + f"    {source}\n" + cmake[line_end + 1:]
            cmake_path.write_text(cmake, encoding="utf-8")
            print(f"registered {source} in {cmake_path}")
        else:
            print(f"already registered: {source}")
    print("仍需在 app_manager/pico.cpp 中注册 app_<name>_get_instance()，并分配唯一 App ID。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
