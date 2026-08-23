/*
 * @Author: 轩
 * @Date: 2026-08-19 16:39:51
 * @LastEditTime: 2026-08-21 10:56:54
 * @FilePath: \pico\ui\ui_geometry.c
 */
#include "ui_geometry.h"

#define UI_MIN(a, b) ((a) < (b) ? (a) : (b))
#define UI_MAX(a, b) ((a) > (b) ? (a) : (b))

bool ui_rect_intersect(const ui_rect_t *r1, const ui_rect_t *r2, ui_rect_t *out_intersect)
{
    if (!out_intersect) return false;

    if (!r1 || !r2 || r1->w <= 0 || r1->h <= 0 || r2->w <= 0 || r2->h <= 0) {
        *out_intersect = ui_rect_make(0, 0, 0, 0);
        return false;
    }

    // 1. 计算重叠边界四边
    int16_t left   = UI_MAX(r1->x, r2->x);
    int16_t top    = UI_MAX(r1->y, r2->y);
    int16_t right  = UI_MIN((int16_t)(r1->x + r1->w), (int16_t)(r2->x + r2->w));
    int16_t bottom = UI_MIN((int16_t)(r1->y + r1->h), (int16_t)(r2->y + r2->h));

    // 2. 判断是否存在有效重叠面积
    if (right > left && bottom > top) {
        out_intersect->x = left;
        out_intersect->y = top;
        out_intersect->w = (int16_t)(right - left);
        out_intersect->h = (int16_t)(bottom - top);
        return true;
    }

    *out_intersect = ui_rect_make(0, 0, 0, 0);
    return false;
}

void ui_rect_union(const ui_rect_t *r1, const ui_rect_t *r2, ui_rect_t *out_union)
{
    if (!out_union) return;

    bool r1_empty = (!r1 || r1->w <= 0 || r1->h <= 0);
    bool r2_empty = (!r2 || r2->w <= 0 || r2->h <= 0);

    if (r1_empty && r2_empty) {
        *out_union = ui_rect_make(0, 0, 0, 0);
        return;
    }
    if (r1_empty) {
        *out_union = *r2;
        return;
    }
    if (r2_empty) {
        *out_union = *r1;
        return;
    }

    int16_t left   = UI_MIN(r1->x, r2->x);
    int16_t top    = UI_MIN(r1->y, r2->y);
    int16_t right  = UI_MAX((int16_t)(r1->x + r1->w), (int16_t)(r2->x + r2->w));
    int16_t bottom = UI_MAX((int16_t)(r1->y + r1->h), (int16_t)(r2->y + r2->h));

    out_union->x = left;
    out_union->y = top;
    out_union->w = (int16_t)(right - left);
    out_union->h = (int16_t)(bottom - top);
}