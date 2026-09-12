#ifndef UI_PIN_CONFIG_H
#define UI_PIN_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui_geometry.h"
#include "ui_tile.h"
#include "pin_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_PIN_CONFIG_INVALID_GPIO UINT8_MAX

typedef struct ui_pin_config ui_pin_config_t;

typedef void (*ui_pin_config_changed_cb_t)(
    ui_pin_config_t *config,
    uint8_t gpio,
    void *user_data
);

struct ui_pin_config
{
    /* rect使用父容器的内容坐标；绘制滚动视图时可复制后平移。 */
    ui_rect_t rect;
    pin_manager_t *manager;

    pin_owner_t owner;
    pin_source_t source;
    pin_fun_t function;
    uint8_t instance;

    const char *title;
    uint8_t selected_gpio;
    uint8_t pressed_gpio;
    uint32_t allowed_gpio_mask;
    uint32_t excluded_gpio_mask;

    bool enabled;
    bool is_touch_down;
    bool is_dragged;
    int16_t down_x;
    int16_t down_y;

    uint16_t color_bg;
    uint16_t color_border;
    uint16_t color_text;
    uint16_t color_selected;
    uint16_t color_busy;

    ui_pin_config_changed_cb_t on_changed;
    void *user_data;
};

void ui_pin_config_init(
    ui_pin_config_t *config,
    int16_t x,
    int16_t y,
    int16_t w,
    const char *title,
    pin_manager_t *manager,
    pin_owner_t owner,
    pin_source_t source,
    pin_fun_t function,
    uint8_t instance,
    uint8_t selected_gpio,
    ui_pin_config_changed_cb_t on_changed
);

void ui_pin_config_set_target(
    ui_pin_config_t *config,
    pin_source_t source,
    pin_fun_t function
);

void ui_pin_config_set_enabled(
    ui_pin_config_t *config,
    bool enabled
);

void ui_pin_config_set_excluded_mask(
    ui_pin_config_t *config,
    uint32_t gpio_mask
);

void ui_pin_config_set_allowed_mask(
    ui_pin_config_t *config,
    uint32_t gpio_mask
);

void ui_pin_config_set_color(
    ui_pin_config_t *config,
    uint16_t background,
    uint16_t border,
    uint16_t text,
    uint16_t selected,
    uint16_t busy
);

bool ui_pin_config_set_selected(
    ui_pin_config_t *config,
    uint8_t gpio
);

uint8_t ui_pin_config_get_selected(const ui_pin_config_t *config);
size_t ui_pin_config_get_candidate_count(const ui_pin_config_t *config);
const pin_init_t *ui_pin_config_get_candidate(
    const ui_pin_config_t *config,
    size_t candidate_index
);
int16_t ui_pin_config_get_preferred_height(const ui_pin_config_t *config);

bool ui_pin_config_process_touch(
    ui_pin_config_t *config,
    bool is_touching,
    int16_t tx,
    int16_t ty,
    uint32_t now_tick
);

void ui_pin_config_draw(
    ui_tile_t *tile,
    const ui_pin_config_t *config
);

#ifdef __cplusplus
}
#endif

#endif /* UI_PIN_CONFIG_H */
