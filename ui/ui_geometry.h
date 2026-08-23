#ifndef UI_GEOMETRY_H
#define UI_GEOMETRY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 基础矩形定义 (以左上角坐标 + 宽高表示) */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} ui_rect_t;

/* 基础二维点坐标 */
typedef struct {
    int16_t x;
    int16_t y;
} ui_point_t;

/* ================= 核心内联几何算法 ================= */

/**
 * @brief 构造并初始化矩形
 */
static inline ui_rect_t ui_rect_make(int16_t x, int16_t y, int16_t w, int16_t h) {
    ui_rect_t r = {x, y, w, h};
    return r;
}

/**
 * @brief 判定矩形是否为空或非法
 */
static inline bool ui_rect_is_empty(const ui_rect_t *r) {
    return (!r || r->w <= 0 || r->h <= 0);
}

/**
 * @brief 判断点是否落在矩形内 (含边界判定)
 */
static inline bool ui_rect_contains_point(const ui_rect_t *r, int16_t x, int16_t y) {
    if (!r || r->w <= 0 || r->h <= 0) return false;
    return (x >= r->x && x < (r->x + r->w) && y >= r->y && y < (r->y + r->h));
}

/**
 * @brief 判定两个矩形是否完全相等
 */
static inline bool ui_rect_equal(const ui_rect_t *r1, const ui_rect_t *r2) {
    if (!r1 || !r2) return false;
    return (r1->x == r2->x && r1->y == r2->y && r1->w == r2->w && r1->h == r2->h);
}

/**
 * @brief 矩形平移变换
 */
static inline ui_rect_t ui_rect_offset(const ui_rect_t *r, int16_t dx, int16_t dy) {
    if (!r) return ui_rect_make(0, 0, 0, 0);
    return ui_rect_make((int16_t)(r->x + dx), (int16_t)(r->y + dy), r->w, r->h);
}

/**
 * @brief 矩形向内收缩 / 向外扩展 (dx/dy 为正则内缩，为负则外扩)
 */
static inline ui_rect_t ui_rect_inset(const ui_rect_t *r, int16_t dx, int16_t dy) {
    if (!r) return ui_rect_make(0, 0, 0, 0);
    int16_t nw = (int16_t)(r->w - 2 * dx);
    int16_t nh = (int16_t)(r->h - 2 * dy);
    if (nw < 0) nw = 0;
    if (nh < 0) nh = 0;
    return ui_rect_make((int16_t)(r->x + dx), (int16_t)(r->y + dy), nw, nh);
}

/* ================= 几何重叠与包围盒运算 ================= */

/**
 * @brief 计算两个矩形的交集 (用于切片与控件区域的相交裁剪)
 * @param r1 输入矩形 1
 * @param r2 输入矩形 2
 * @param out_intersect 输出相交区域 (无交集返回 false，输出为 0 矩形)
 * @return true 存在有效交集，false 完全不相交
 */
bool ui_rect_intersect(const ui_rect_t *r1, const ui_rect_t *r2, ui_rect_t *out_intersect);

/**
 * @brief 计算包含两个矩形的最小外接矩形 (用于脏矩形 Dirty-Rect 合并)
 * @param r1 输入矩形 1
 * @param r2 输入矩形 2
 * @param out_union 输出合并后的外接矩形
 */
void ui_rect_union(const ui_rect_t *r1, const ui_rect_t *r2, ui_rect_t *out_union);

#ifdef __cplusplus
}
#endif

#endif /* UI_GEOMETRY_H */