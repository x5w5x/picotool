#!/usr/bin/env python3
"""Small Tk helper for App generation, build/deploy, and RGB565 colors."""

from __future__ import annotations

import queue
import re
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import colorchooser, messagebox, ttk

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "generate_app.py"
FLASHER = ROOT / "flash_pico.py"
CMK = Path(r"C:\Users\xwx\.pico-sdk\cmake\v3.31.5\bin\cmake.exe")


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


class PicoTool(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Pico 开发工具")
        self.geometry("760x560")
        self.minsize(680, 480)
        self.log_queue: queue.Queue[str] = queue.Queue()
        self._build_ui()
        self.after(100, self._drain_log)

    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.pack(fill="both", expand=True)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(1, weight=1)

        notebook = ttk.Notebook(root)
        notebook.grid(row=0, column=0, sticky="ew")
        self._build_project_tab(notebook)
        self._build_palette_tab(notebook)
        self._build_design_tab(notebook)

        log_frame = ttk.LabelFrame(root, text="日志")
        log_frame.grid(row=1, column=0, sticky="nsew", pady=(10, 0))
        log_frame.rowconfigure(0, weight=1)
        log_frame.columnconfigure(0, weight=1)
        self.log = tk.Text(log_frame, height=12, state="disabled", wrap="none")
        self.log.grid(row=0, column=0, sticky="nsew")
        ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview).grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=lambda *args: None)

    def _build_project_tab(self, notebook: ttk.Notebook) -> None:
        tab = ttk.Frame(notebook, padding=14)
        notebook.add(tab, text="项目工具")
        tab.columnconfigure(1, weight=1)

        ttk.Label(tab, text="功能名").grid(row=0, column=0, sticky="w", pady=4)
        self.app_name = tk.StringVar(value="settings")
        ttk.Entry(tab, textvariable=self.app_name, width=30).grid(row=0, column=1, sticky="ew", pady=4)
        ttk.Label(tab, text="例如 led、uart、settings").grid(row=0, column=2, sticky="w", padx=8)

        ttk.Label(tab, text="显示名称").grid(row=1, column=0, sticky="w", pady=4)
        self.display_name = tk.StringVar(value="系统设置")
        ttk.Entry(tab, textvariable=self.display_name).grid(row=1, column=1, sticky="ew", pady=4)

        self.register = tk.BooleanVar(value=True)
        ttk.Checkbutton(tab, text="自动加入 app/CMakeLists.txt", variable=self.register).grid(row=2, column=1, sticky="w", pady=4)

        buttons = ttk.Frame(tab)
        buttons.grid(row=3, column=0, columnspan=3, sticky="w", pady=(12, 4))
        ttk.Button(buttons, text="生成 App 模板", command=self.generate).pack(side="left", padx=(0, 8))
        ttk.Button(buttons, text="构建固件", command=self.build).pack(side="left", padx=8)
        ttk.Button(buttons, text="构建并更新 Pico", command=self.deploy).pack(side="left", padx=8)
        ttk.Button(buttons, text="仅复制 UF2", command=self.flash).pack(side="left", padx=8)

        ttk.Label(tab, text=f"工程目录: {ROOT}").grid(row=4, column=0, columnspan=3, sticky="w", pady=(20, 0))

    def _build_palette_tab(self, notebook: ttk.Notebook) -> None:
        tab = ttk.Frame(notebook, padding=14)
        notebook.add(tab, text="调色盘")
        tab.columnconfigure(1, weight=1)
        self.color_preview = tk.Label(tab, text="点击选择颜色", width=24, height=5, relief="solid")
        self.color_preview.grid(row=0, column=0, rowspan=4, padx=(0, 18))
        ttk.Button(tab, text="选择颜色", command=self.pick_color).grid(row=0, column=1, sticky="w", pady=4)
        self.hex_value = tk.StringVar(value="#FFFFFF")
        self.rgb_value = tk.StringVar(value="RGB(255, 255, 255)")
        self.rgb565_value = tk.StringVar(value="0xFFFF")
        ttk.Label(tab, textvariable=self.hex_value, font=("Consolas", 12)).grid(row=1, column=1, sticky="w", pady=4)
        ttk.Label(tab, textvariable=self.rgb_value).grid(row=2, column=1, sticky="w", pady=4)
        ttk.Entry(tab, textvariable=self.rgb565_value, width=16).grid(row=3, column=1, sticky="w", pady=4)
        ttk.Button(tab, text="复制 RGB565", command=self.copy_rgb565).grid(row=4, column=1, sticky="w", pady=(8, 4))

    def _build_design_tab(self, notebook: ttk.Notebook) -> None:
        tab = ttk.Frame(notebook, padding=8)
        notebook.add(tab, text="UI 设计")
        tab.columnconfigure(0, weight=1); tab.columnconfigure(1, weight=0)
        tab.rowconfigure(0, weight=1)
        self.design_items: list[dict[str, str]] = []
        self.design_selected = -1
        self.design_drag_offset = (0, 0)
        self.design_canvas = tk.Canvas(tab, width=480, height=320, bg="#FFFFFF", highlightthickness=1)
        self.design_canvas.grid(row=0, column=0, sticky="nw", padx=(0, 10))
        self.design_canvas.bind("<Button-1>", self._design_mouse_down)
        self.design_canvas.bind("<B1-Motion>", self._design_mouse_move)
        self.design_canvas.bind("<ButtonRelease-1>", self._design_mouse_up)
        self.design_canvas.bind("<Delete>", self.delete_design_item)
        self.design_canvas.focus_set()
        panel = ttk.Frame(tab, padding=4); panel.grid(row=0, column=1, sticky="ns")
        fields = ("类型", "名称/文本", "X", "Y", "宽", "高", "颜色")
        self.design_vars: dict[str, tk.StringVar] = {}
        for i, label in enumerate(fields):
            ttk.Label(panel, text=label).grid(row=i, column=0, sticky="w", pady=3)
            var = tk.StringVar(value={"类型":"text", "名称/文本":"标题", "X":"20", "Y":"50", "宽":"160", "高":"32", "颜色":"0x4A69"}.get(label, ""))
            self.design_vars[label] = var
            if label == "类型":
                ttk.Combobox(panel, textvariable=var, values=("text", "rect", "button", "switch"), width=14, state="readonly").grid(row=i, column=1)
            else:
                ttk.Entry(panel, textvariable=var, width=16).grid(row=i, column=1)
        ttk.Button(panel, text="添加组件", command=self.add_design_item).grid(row=7, column=0, sticky="ew", pady=(10, 3))
        ttk.Button(panel, text="更新选中", command=self.update_design_item).grid(row=7, column=1, sticky="ew", pady=(10, 3), padx=(4, 0))
        ttk.Button(panel, text="删除选中", command=self.delete_design_item).grid(row=8, column=0, sticky="ew", pady=3)
        ttk.Button(panel, text="清空画布", command=self.clear_design).grid(row=8, column=1, sticky="ew", pady=3, padx=(4, 0))
        ttk.Button(panel, text="生成 C 代码", command=self.generate_design_code).grid(row=9, column=0, columnspan=2, sticky="ew", pady=3)
        ttk.Button(panel, text="复制 C 代码", command=self.copy_design_code).grid(row=10, column=0, columnspan=2, sticky="ew", pady=3)
        self.design_code = tk.Text(panel, width=34, height=13, wrap="none")
        self.design_code.grid(row=11, column=0, columnspan=2, pady=(10, 0))
        self._redraw_design()

    def _design_canvas_click(self, event: tk.Event) -> None:
        self.design_vars["X"].set(str(event.x)); self.design_vars["Y"].set(str(event.y))

    def _design_mouse_down(self, event: tk.Event) -> None:
        # 逆序命中，后添加的组件视为位于上层。
        self.design_selected = -1
        for index in range(len(self.design_items) - 1, -1, -1):
            item = self.design_items[index]
            x, y, w, h = (int(item[k]) for k in ("X", "Y", "宽", "高"))
            if x <= event.x <= x + w and y <= event.y <= y + h:
                self.design_selected = index
                self.design_drag_offset = (event.x - x, event.y - y)
                self._load_design_item(item)
                break
        if self.design_selected < 0:
            self._design_canvas_click(event)
        self._redraw_design()

    def _design_mouse_move(self, event: tk.Event) -> None:
        if self.design_selected < 0:
            return
        item = self.design_items[self.design_selected]
        ox, oy = self.design_drag_offset
        x = max(0, min(480 - int(item["宽"]), event.x - ox))
        y = max(0, min(320 - int(item["高"]), event.y - oy))
        item["X"], item["Y"] = str(x), str(y)
        self._load_design_item(item)
        self._redraw_design()

    def _design_mouse_up(self, event: tk.Event) -> None:
        self._design_mouse_move(event)

    def _load_design_item(self, item: dict[str, str]) -> None:
        for key, value in item.items():
            if key in self.design_vars:
                self.design_vars[key].set(value)

    def add_design_item(self) -> None:
        try:
            item = {k: v.get().strip() for k, v in self.design_vars.items()}
            for key in ("X", "Y", "宽", "高"):
                int(item[key])
        except ValueError:
            messagebox.showerror("参数错误", "X/Y/宽/高必须是整数")
            return
        self.design_items.append(item); self.design_selected = len(self.design_items) - 1
        self._redraw_design()

    def update_design_item(self) -> None:
        if self.design_selected < 0:
            messagebox.showinfo("提示", "请先在画布上选择组件")
            return
        try:
            item = {k: v.get().strip() for k, v in self.design_vars.items()}
            for key in ("X", "Y", "宽", "高"):
                int(item[key])
        except ValueError:
            messagebox.showerror("参数错误", "X/Y/宽/高必须是整数")
            return
        self.design_items[self.design_selected] = item
        self._redraw_design()

    def delete_design_item(self, event: tk.Event | None = None) -> None:
        if self.design_selected < 0:
            return
        del self.design_items[self.design_selected]
        self.design_selected = min(self.design_selected, len(self.design_items) - 1)
        self._redraw_design()

    def copy_design_code(self) -> None:
        self.generate_design_code()
        self.clipboard_clear()
        self.clipboard_append(self.design_code.get("1.0", "end-1c"))
        self.update()

    def clear_design(self) -> None:
        self.design_items.clear(); self.design_selected = -1
        self._redraw_design(); self.design_code.delete("1.0", "end")

    def _redraw_design(self) -> None:
        c = self.design_canvas; c.delete("all")
        for x in range(0, 481, 40): c.create_line(x, 0, x, 320, fill="#E8EDF2")
        for y in range(0, 321, 40): c.create_line(0, y, 480, y, fill="#E8EDF2")
        c.create_line(0, 32, 480, 32, fill="#D09030", dash=(4, 2))
        c.create_text(6, 8, anchor="nw", text="状态栏 32px", fill="#A06020", font=("Arial", 8))
        # Match the application's left TabView navigation reference area.
        c.create_rectangle(0, 32, 110, 320, fill="#F1F5F8", outline="#AAB8C4")
        c.create_line(110, 32, 110, 320, fill="#AAB8C4", width=2)
        c.create_text(8, 48, anchor="nw", text="TabView 侧栏 110px", fill="#607890", font=("Arial", 8))
        for index, item in enumerate(self.design_items):
            x, y, w, h = (int(item[k]) for k in ("X", "Y", "宽", "高"))
            typ = item["类型"]; label = item["名称/文本"]
            if typ == "text":
                c.create_text(x, y, anchor="nw", text=label, fill="#304A69", font=("Microsoft YaHei UI", 10))
            elif typ == "switch":
                radius = h // 2
                c.create_oval(x, y, x+h, y+h, fill="#B8C6D6", outline="#607890")
                c.create_rectangle(x+radius, y, x+w-radius, y+h, fill="#B8C6D6", outline="#B8C6D6")
                c.create_oval(x+w-h, y, x+w, y+h, fill="#B8C6D6", outline="#607890")
                c.create_oval(x+w-h+3, y+3, x+w-3, y+h-3, fill="#FFFFFF", outline="#607890")
            else:
                if typ == "button":
                    c.create_rectangle(x+4, y+4, x+w-4, y+h-2, fill="#B8DCCF", outline="")
                    c.create_rectangle(x, y, x+w, y+h, outline="#4A6988", fill="#DCECF0")
                else:
                    c.create_rectangle(x, y, x+w, y+h, outline="#4A6988", fill="#EEF3F7")
            
            if typ in ("button", "rect"): c.create_text(x+w//2, y+h//2, text=label, fill="#304A69")
            if index == self.design_selected:
                c.create_rectangle(max(0, x - 3), max(0, y - 3), min(480, x + w + 3), min(320, y + h + 3), outline="#D07020", width=2, dash=(4, 2))

    def generate_design_code(self) -> None:
        lines = []
        for item in self.design_items:
            typ, label = item["类型"], item["名称/文本"]
            x, y, w, h, color = item["X"], item["Y"], item["宽"], item["高"], item["颜色"]
            if typ == "text":
                lines.append(f'ui_draw_string(tile, {x}, {y}, "{label}", FONT_16, {color}, 0xFFFF);')
            elif typ == "rect":
                lines.append(f'ui_rect_t rect = ui_rect_make({x}, {y}, {w}, {h});\nui_draw_rect(tile, &rect, {color});')
            elif typ == "button":
                name = re.sub(r"[^a-zA-Z0-9]+", "_", label).strip("_").lower() or "button"
                lines.append(f'ui_btn_init(&data->btn_{name}, {x}, {y}, {w}, {h}, "{label}", on_{name}_click);')
            else:
                name = re.sub(r"[^a-zA-Z0-9]+", "_", label).strip("_").lower() or "switch"
                lines.append(f'ui_switch_init(&data->sw_{name}, {x}, {y}, {w}, {h}, false, on_{name}_changed);')
        self.design_code.delete("1.0", "end"); self.design_code.insert("1.0", "\n\n".join(lines))

    def _run(self, args: list[str]) -> None:
        def worker() -> None:
            self.log_queue.put("> " + " ".join(args) + "\n")
            try:
                process = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, text=True,
                                           encoding="utf-8", errors="replace")
                assert process.stdout is not None
                for line in process.stdout:
                    self.log_queue.put(line)
                code = process.wait()
                self.log_queue.put(f"[完成] exit={code}\n")
            except OSError as exc:
                self.log_queue.put(f"[错误] {exc}\n")
        threading.Thread(target=worker, daemon=True).start()

    def generate(self) -> None:
        name = self.app_name.get().strip()
        if not name:
            messagebox.showerror("参数错误", "请输入功能名")
            return
        args = [sys.executable, str(GENERATOR), name, "--display-name", self.display_name.get().strip() or name]
        if self.register.get():
            args.append("--register")
        self._run(args)

    def build(self) -> None:
        self._run([str(CMK), "--build", str(ROOT / "build"), "--target", "pico", "--parallel", "1"])

    def flash(self) -> None:
        self._run([sys.executable, str(FLASHER), str(ROOT / "build" / "pico.uf2")])

    def deploy(self) -> None:
        self._run([sys.executable, str(Path(__file__).resolve()), "--deploy-worker"])

    def pick_color(self) -> None:
        result = colorchooser.askcolor(title="选择 RGB565 颜色")
        if not result or result[0] is None or result[1] is None:
            return
        (r, g, b), hx = result
        value = rgb565(int(r), int(g), int(b))
        self.color_preview.configure(bg=hx)
        self.hex_value.set(hx.upper())
        self.rgb_value.set(f"RGB({int(r)}, {int(g)}, {int(b)})")
        self.rgb565_value.set(f"0x{value:04X}")

    def copy_rgb565(self) -> None:
        self.clipboard_clear()
        self.clipboard_append(self.rgb565_value.get())
        self.update()

    def _drain_log(self) -> None:
        while True:
            try:
                line = self.log_queue.get_nowait()
            except queue.Empty:
                break
            self.log.configure(state="normal")
            self.log.insert("end", line)
            self.log.see("end")
            self.log.configure(state="disabled")
        self.after(100, self._drain_log)


if __name__ == "__main__" and "--deploy-worker" not in sys.argv:
    PicoTool().mainloop()
elif "--deploy-worker" in sys.argv:
    subprocess.run([str(CMK), "--build", str(ROOT / "build"), "--target", "pico", "--parallel", "1"], cwd=ROOT, check=False)
    subprocess.run([sys.executable, str(FLASHER), str(ROOT / "build" / "pico.uf2")], cwd=ROOT, check=False)
