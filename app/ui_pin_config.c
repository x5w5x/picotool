#include "ui_pin_config.h"

#include <stdlib.h>
#include <string.h>

#include "ui_draw.h"

#define UI_PIN_CONFIG_COLUMNS       3
#define UI_PIN_CONFIG_PADDING       8
#define UI_PIN_CONFIG_TITLE_H       24
#define UI_PIN_CONFIG_CELL_H        32
#define UI_PIN_CONFIG_CELL_GAP      6
#define UI_PIN_CONFIG_TOUCH_SLOP    8

static bool ui_pin_config_supports_target(
    const ui_pin_config_t *config,
    uint8_t gpio
)
{
    if (config == NULL || config->function == PIN_FUNC_NONE) {
        return false;
    }

    if (gpio >= 32u ||
        (config->allowed_gpio_mask & (1u << gpio)) == 0u) {
        return false;
    }

    if (config->source == PIN_SOURCE_PIO) {
        return pin_manager_supports(gpio, PIN_FUNC_PIO);
    }

    if (config->source == PIN_SOURCE_HARDWARE) {
        return pin_manager_supports(gpio, config->function);
    }

    return false;
}

static bool ui_pin_config_is_available(
    const ui_pin_config_t *config,
    uint8_t gpio
)
{
    if (config == NULL || config->manager == NULL ||
        !ui_pin_config_supports_target(config, gpio)) {
        return false;
    }

    if (gpio < 32u &&
        (config->excluded_gpio_mask & (1u << gpio)) != 0u) {
        return false;
    }

    const pin_runtime_t *runtime =
        pin_manager_get_runtime(config->manager, gpio);

    if (runtime == NULL) {
        return false;
    }

    if (!runtime->allocated) {
        return true;
    }

    /* 已经属于本功能实例的引脚仍允许显示和选中。 */
    return runtime->owner == config->owner &&
           runtime->instance == config->instance;
}

size_t ui_pin_config_get_candidate_count(const ui_pin_config_t *config)
{
    if (config == NULL) {
        return 0;
    }

    size_t count = 0;
    size_t pin_count = pin_manager_get_count();

    for (size_t i = 0; i < pin_count; ++i) {
        const pin_init_t *pin = pin_manager_get_pin_at(i);
        if (pin != NULL &&
            ui_pin_config_supports_target(config, pin->gpio)) {
            ++count;
        }
    }

    return count;
}

const pin_init_t *ui_pin_config_get_candidate(
    const ui_pin_config_t *config,
    size_t candidate_index
)
{
    if (config == NULL) {
        return NULL;
    }

    size_t matched = 0;
    size_t pin_count = pin_manager_get_count();

    for (size_t i = 0; i < pin_count; ++i) {
        const pin_init_t *pin = pin_manager_get_pin_at(i);
        if (pin == NULL ||
            !ui_pin_config_supports_target(config, pin->gpio)) {
            continue;
        }

        if (matched == candidate_index) {
            return pin;
        }
        ++matched;
    }

    return NULL;
}

int16_t ui_pin_config_get_preferred_height(const ui_pin_config_t *config)
{
    size_t count = ui_pin_config_get_candidate_count(config);
    size_t rows = (count + UI_PIN_CONFIG_COLUMNS - 1u) /
                  UI_PIN_CONFIG_COLUMNS;

    return (int16_t)(
        UI_PIN_CONFIG_PADDING +
        UI_PIN_CONFIG_TITLE_H +
        rows * UI_PIN_CONFIG_CELL_H +
        (rows > 0 ? (rows - 1u) * UI_PIN_CONFIG_CELL_GAP : 0u) +
        UI_PIN_CONFIG_PADDING
    );
}

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
)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));

    config->manager = manager;
    config->owner = owner;
    config->source = source;
    config->function = function;
    config->instance = instance;
    config->title = title;
    config->selected_gpio = UI_PIN_CONFIG_INVALID_GPIO;
    config->pressed_gpio = UI_PIN_CONFIG_INVALID_GPIO;
    config->allowed_gpio_mask = UINT32_MAX;
    config->enabled = true;
    config->on_changed = on_changed;

    /* 与系统设置页面一致的浅灰、银灰配色。 */
    config->color_bg = 0xF7BE;
    config->color_border = 0xC618;
    config->color_text = 0x4208;
    config->color_selected = 0xBDF7;
    config->color_busy = 0xDEFB;

    config->rect = ui_rect_make(x, y, w, 0);
    config->rect.h = ui_pin_config_get_preferred_height(config);

    (void)ui_pin_config_set_selected(config, selected_gpio);
}

void ui_pin_config_set_target(
    ui_pin_config_t *config,
    pin_source_t source,
    pin_fun_t function
)
{
    if (config == NULL) {
        return;
    }

    config->source = source;
    config->function = function;
    config->rect.h = ui_pin_config_get_preferred_height(config);

    if (!ui_pin_config_supports_target(config, config->selected_gpio)) {
        config->selected_gpio = UI_PIN_CONFIG_INVALID_GPIO;
    }

    config->pressed_gpio = UI_PIN_CONFIG_INVALID_GPIO;
    config->is_touch_down = false;
    config->is_dragged = false;
}

void ui_pin_config_set_enabled(
    ui_pin_config_t *config,
    bool enabled
)
{
    if (config == NULL) {
        return;
    }

    config->enabled = enabled;

    if (!enabled) {
        config->pressed_gpio = UI_PIN_CONFIG_INVALID_GPIO;
        config->is_touch_down = false;
        config->is_dragged = false;
    }
}

void ui_pin_config_set_excluded_mask(
    ui_pin_config_t *config,
    uint32_t gpio_mask
)
{
    if (config == NULL) {
        return;
    }

    config->excluded_gpio_mask = gpio_mask;

    if (config->selected_gpio < 32u &&
        (gpio_mask & (1u << config->selected_gpio)) != 0u) {
        config->selected_gpio = UI_PIN_CONFIG_INVALID_GPIO;
    }
}

void ui_pin_config_set_allowed_mask(
    ui_pin_config_t *config,
    uint32_t gpio_mask
)
{
    if (config == NULL) {
        return;
    }

    config->allowed_gpio_mask = gpio_mask;
    config->rect.h = ui_pin_config_get_preferred_height(config);

    if (!ui_pin_config_supports_target(config, config->selected_gpio)) {
        config->selected_gpio = UI_PIN_CONFIG_INVALID_GPIO;
    }
}

void ui_pin_config_set_color(
    ui_pin_config_t *config,
    uint16_t background,
    uint16_t border,
    uint16_t text,
    uint16_t selected,
    uint16_t busy
)
{
    if (config == NULL) {
        return;
    }

    config->color_bg = background;
    config->color_border = border;
    config->color_text = text;
    config->color_selected = selected;
    config->color_busy = busy;
}

bool ui_pin_config_set_selected(
    ui_pin_config_t *config,
    uint8_t gpio
)
{
    if (config == NULL) {
        return false;
    }

    if (gpio == UI_PIN_CONFIG_INVALID_GPIO) {
        config->selected_gpio = gpio;
        return true;
    }

    if (!ui_pin_config_is_available(config, gpio)) {
        return false;
    }

    config->selected_gpio = gpio;
    return true;
}

uint8_t ui_pin_config_get_selected(const ui_pin_config_t *config)
{
    return config != NULL
        ? config->selected_gpio
        : UI_PIN_CONFIG_INVALID_GPIO;
}

static bool ui_pin_config_get_cell_rect(
    const ui_pin_config_t *config,
    size_t candidate_index,
    ui_rect_t *out_rect
)
{
    if (config == NULL || out_rect == NULL || config->rect.w <= 0) {
        return false;
    }

    int16_t inner_w = (int16_t)(
        config->rect.w - 2 * UI_PIN_CONFIG_PADDING
    );
    int16_t total_gap = (UI_PIN_CONFIG_COLUMNS - 1) *
                        UI_PIN_CONFIG_CELL_GAP;
    int16_t cell_w = (int16_t)(
        (inner_w - total_gap) / UI_PIN_CONFIG_COLUMNS
    );

    if (cell_w <= 0) {
        return false;
    }

    size_t row = candidate_index / UI_PIN_CONFIG_COLUMNS;
    size_t column = candidate_index % UI_PIN_CONFIG_COLUMNS;

    *out_rect = ui_rect_make(
        (int16_t)(config->rect.x + UI_PIN_CONFIG_PADDING +
                  column * (cell_w + UI_PIN_CONFIG_CELL_GAP)),
        (int16_t)(config->rect.y + UI_PIN_CONFIG_PADDING +
                  UI_PIN_CONFIG_TITLE_H +
                  row * (UI_PIN_CONFIG_CELL_H + UI_PIN_CONFIG_CELL_GAP)),
        cell_w,
        UI_PIN_CONFIG_CELL_H
    );
    return true;
}

static uint8_t ui_pin_config_hit_gpio(
    const ui_pin_config_t *config,
    int16_t x,
    int16_t y
)
{
    size_t count = ui_pin_config_get_candidate_count(config);

    for (size_t i = 0; i < count; ++i) {
        const pin_init_t *pin = ui_pin_config_get_candidate(config, i);
        ui_rect_t cell;

        if (pin != NULL &&
            ui_pin_config_get_cell_rect(config, i, &cell) &&
            ui_rect_contains_point(&cell, x, y)) {
            return pin->gpio;
        }
    }

    return UI_PIN_CONFIG_INVALID_GPIO;
}

bool ui_pin_config_process_touch(
    ui_pin_config_t *config,
    bool is_touching,
    int16_t tx,
    int16_t ty,
    uint32_t now_tick
)
{
    (void)now_tick;

    if (config == NULL || !config->enabled) {
        return false;
    }

    if (is_touching) {
        if (!config->is_touch_down) {
            uint8_t gpio = ui_pin_config_hit_gpio(config, tx, ty);
            if (gpio != UI_PIN_CONFIG_INVALID_GPIO &&
                ui_pin_config_is_available(config, gpio)) {
                config->is_touch_down = true;
                config->is_dragged = false;
                config->down_x = tx;
                config->down_y = ty;
                config->pressed_gpio = gpio;
                return true;
            }
        }
        else {
            int16_t dx = (int16_t)abs(tx - config->down_x);
            int16_t dy = (int16_t)abs(ty - config->down_y);
            if (dx >= UI_PIN_CONFIG_TOUCH_SLOP ||
                dy >= UI_PIN_CONFIG_TOUCH_SLOP) {
                config->is_dragged = true;
                config->pressed_gpio = UI_PIN_CONFIG_INVALID_GPIO;
                return true;
            }
        }
        return false;
    }

    if (!config->is_touch_down) {
        return false;
    }

    uint8_t pressed_gpio = config->pressed_gpio;
    bool choose = !config->is_dragged &&
                  pressed_gpio != UI_PIN_CONFIG_INVALID_GPIO &&
                  ui_pin_config_is_available(config, pressed_gpio);

    config->is_touch_down = false;
    config->is_dragged = false;
    config->pressed_gpio = UI_PIN_CONFIG_INVALID_GPIO;

    if (!choose) {
        return true;
    }

    bool changed = config->selected_gpio != pressed_gpio;
    config->selected_gpio = pressed_gpio;

    if (changed && config->on_changed != NULL) {
        config->on_changed(config, pressed_gpio, config->user_data);
    }

    return true;
}

void ui_pin_config_draw(
    ui_tile_t *tile,
    const ui_pin_config_t *config
)
{
    if (tile == NULL || config == NULL) {
        return;
    }

    ui_draw_fill_rect(tile, &config->rect, config->color_bg);
    ui_draw_rect(tile, &config->rect, config->color_border);

    if (config->title != NULL) {
        ui_draw_string(
            tile,
            (int16_t)(config->rect.x + UI_PIN_CONFIG_PADDING),
            (int16_t)(config->rect.y + UI_PIN_CONFIG_PADDING),
            config->title,
            FONT_16,
            config->color_text,
            config->color_bg
        );
    }

    size_t count = ui_pin_config_get_candidate_count(config);

    for (size_t i = 0; i < count; ++i) {
        const pin_init_t *pin = ui_pin_config_get_candidate(config, i);
        ui_rect_t cell;

        if (pin == NULL ||
            !ui_pin_config_get_cell_rect(config, i, &cell)) {
            continue;
        }

        bool available = ui_pin_config_is_available(config, pin->gpio);
        bool selected = pin->gpio == config->selected_gpio;
        bool pressed = pin->gpio == config->pressed_gpio;

        uint16_t cell_bg = !available
            ? config->color_busy
            : ((selected || pressed)
                ? config->color_selected
                : 0xFFFF);

        ui_draw_fill_rect(tile, &cell, cell_bg);
        ui_draw_rect(tile, &cell, config->color_border);

        /* GPxx均为ASCII，FONT_16下每个字符宽约8像素。 */
        int16_t text_w = (int16_t)(strlen(pin->label) * 8u);
        int16_t text_x = (int16_t)(cell.x + (cell.w - text_w) / 2);
        int16_t text_y = (int16_t)(cell.y + (cell.h - 16) / 2);

        ui_draw_string(
            tile,
            text_x,
            text_y,
            pin->label,
            FONT_16,
            available ? config->color_text : 0x8410,
            cell_bg
        );
    }
}
