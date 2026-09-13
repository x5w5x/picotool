#include "ui_pin_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_draw.h"

#define ROW_H 44
#define LIST_ROW_H 40
#define MARGIN 8
#define LIST_GAP 8
#define LIST_COLUMNS 2u
#define SLOP 8

static bool supports(const ui_pin_config_t *c,uint8_t g){
 if(!c||c->function==PIN_FUNC_NONE||g>=32||!(c->allowed_gpio_mask&(1u<<g)))return false;
 if(c->source==PIN_SOURCE_HARDWARE)return pin_manager_supports(g,c->function);
 if(c->source==PIN_SOURCE_PIO)return pin_manager_supports(g,PIN_FUNC_PIO);
 return pin_manager_supports(g,c->function)||pin_manager_supports(g,PIN_FUNC_PIO);
}
static bool available(const ui_pin_config_t *c,uint8_t g){
 if(!c||!c->manager||!supports(c,g)||(c->excluded_gpio_mask&(1u<<g)))return false;
 const pin_runtime_t *r=pin_manager_get_runtime(c->manager,g);
 return r&&(!r->allocated||(r->owner==c->owner&&r->instance==c->instance));
}
size_t ui_pin_config_get_candidate_count(const ui_pin_config_t *c){
 size_t n=0;if(!c)return 0;for(size_t i=0;i<pin_manager_get_count();i++){const pin_init_t*p=pin_manager_get_pin_at(i);if(p&&supports(c,p->gpio))n++;}return n;
}
const pin_init_t *ui_pin_config_get_candidate(const ui_pin_config_t*c,size_t wanted){
 if(!c)return NULL;
 size_t n=0;
 for(size_t i=0;i<pin_manager_get_count();i++){
  const pin_init_t*p=pin_manager_get_pin_at(i);
  if(!p||!supports(c,p->gpio))continue;
  if(n++==wanted)return p;
 }
 return NULL;
}
static ui_rect_t list_row(const ui_modal_t *m, size_t i)
{
    int16_t cell_w = (int16_t)((m->sv_body.view_rect.w - 2 * MARGIN - LIST_GAP) / 2);
    size_t row = i / LIST_COLUMNS;
    size_t col = i % LIST_COLUMNS;
    return ui_rect_make(
        (int16_t)(m->sv_body.view_rect.x + MARGIN + col * (cell_w + LIST_GAP)),
        (int16_t)(m->sv_body.view_rect.y + MARGIN + row * LIST_ROW_H - m->sv_body.scroll_y),
        cell_w, LIST_ROW_H - 6);
}
static void modal_draw(ui_tile_t *t,const ui_rect_t *v,int16_t sy,void *ud)
{
    (void)v; (void)sy;
    ui_pin_config_t *c=ud;
    if(!c)return;
    for(size_t i=0;i<ui_pin_config_get_candidate_count(c);i++){
        const pin_init_t *p=ui_pin_config_get_candidate(c,i);
        if(!p)continue;
        ui_rect_t r=list_row(&c->modal,i);
        bool a=available(c,p->gpio);
        uint16_t bg=!a?c->color_busy:(p->gpio==c->pending_gpio?c->color_selected:0xFFFF);
        ui_draw_fill_rect(t,&r,bg);
        ui_draw_rect(t,&r,c->color_border);
        ui_draw_string(t,r.x+8,r.y+9,p->label,FONT_16,a?c->color_text:0x8410,bg);
        const char *k=pin_manager_supports(p->gpio,c->function)
            ?(c->function==PIN_FUNC_UART_TX?"UART1 TX":"UART1 RX"):"PIO";
        ui_draw_string(t,r.x+52,r.y+9,k,FONT_16,a?0x632C:0x8410,bg);
    }
}
static bool modal_touch(ui_modal_t*m,bool down,int16_t x,int16_t y,uint32_t tick,void*ud){
 (void)tick;
 ui_pin_config_t*c=ud;
 if(!c||!m||!down||m->sv_body.is_dragged)return false;

 /* 底部操作栏不属于列表，防止“应用生效”同时点中被遮住的候选项。 */
 if(!ui_rect_contains_point(&m->sv_body.view_rect,x,y))return false;

 for(size_t i=0;i<ui_pin_config_get_candidate_count(c);i++){
  const pin_init_t*p=ui_pin_config_get_candidate(c,i);
  ui_rect_t r=list_row(m,i);
  if(p&&ui_rect_contains_point(&r,x,y)&&available(c,p->gpio)){
   c->pending_gpio=p->gpio;
   return true;
  }
 }
 return false;
}
static void modal_action(ui_modal_t*m,bool ok,void*ud){
 (void)m;
 ui_pin_config_t*c=ud;
 if(!c)return;

 if(!ok){
  /* 取消时丢弃本次临时选择。 */
  c->pending_gpio=c->selected_gpio;
  return;
 }

 if(c->pending_gpio==UINT8_MAX||!available(c,c->pending_gpio))return;

 bool changed=c->selected_gpio!=c->pending_gpio;
 c->selected_gpio=c->pending_gpio;

 /* 通知所属页面更新互斥掩码并重绘外层选择按钮。 */
 if(changed&&c->on_changed){
  c->on_changed(c,c->selected_gpio,c->user_data);
 }
}

void ui_pin_config_init(ui_pin_config_t*c,int16_t x,int16_t y,int16_t w,const char*title,pin_manager_t*m,pin_owner_t owner,pin_source_t source,pin_fun_t function,uint8_t instance,uint8_t selected,ui_pin_config_changed_cb_t cb){
 if(!c)return;
 memset(c,0,sizeof(*c));
 c->rect=ui_rect_make(x,y,w,ROW_H);
 c->manager=m;
 c->owner=owner;
 c->source=source;
 c->function=function;
 c->instance=instance;
 c->title=title;
 c->selected_gpio=UINT8_MAX;
 c->pending_gpio=UINT8_MAX;
 c->pressed_gpio=UINT8_MAX;
 c->allowed_gpio_mask=UINT32_MAX;
 c->enabled=true;
 c->on_changed=cb;
 c->color_bg=0xF7BE;
 c->color_border=0xC618;
 c->color_text=0x4208;
 c->color_selected=0xBDF7;
 c->color_busy=0xDEFB;
 ui_modal_init(&c->modal,480,320,360,250,250,title,UI_MODAL_BTN_OK_CANCEL,modal_draw,modal_touch,modal_action);
 c->modal.color_bg=0xF7BE;
 c->modal.color_border=0xC618;
 c->modal.color_title=0x4208;
 c->modal.sv_body.color_bg=0xFFFF;
 c->modal.sv_body.color_scrollbar=0xA514;
 c->modal.btn_close.rect=ui_rect_make(0,0,0,0);
 ui_btn_set_color(&c->modal.btn_cancel,0xD69A,0xBDF7,0x4208);
 ui_btn_set_color(&c->modal.btn_confirm,0xBDF7,0xA514,0x4208);
 (void)ui_pin_config_set_selected(c,selected);
}
void ui_pin_config_set_target(ui_pin_config_t*c,pin_source_t s,pin_fun_t f){if(c){c->source=s;c->function=f;if(!supports(c,c->selected_gpio))c->selected_gpio=UINT8_MAX;}}
void ui_pin_config_set_enabled(ui_pin_config_t*c,bool e){if(c)c->enabled=e;}
void ui_pin_config_set_excluded_mask(ui_pin_config_t*c,uint32_t m){if(c)c->excluded_gpio_mask=m;}
void ui_pin_config_set_allowed_mask(ui_pin_config_t*c,uint32_t m){if(c)c->allowed_gpio_mask=m;}
void ui_pin_config_set_color(ui_pin_config_t*c,uint16_t bg,uint16_t border,uint16_t text,uint16_t sel,uint16_t busy){if(c){c->color_bg=bg;c->color_border=border;c->color_text=text;c->color_selected=sel;c->color_busy=busy;}}
bool ui_pin_config_set_selected(ui_pin_config_t*c,uint8_t g){if(!c)return false;if(g==UINT8_MAX){c->selected_gpio=g;return true;}if(!available(c,g))return false;c->selected_gpio=g;return true;}
uint8_t ui_pin_config_get_selected(const ui_pin_config_t*c){return c?c->selected_gpio:UINT8_MAX;}
int16_t ui_pin_config_get_preferred_height(const ui_pin_config_t*c){(void)c;return ROW_H;}
bool ui_pin_config_process_touch(ui_pin_config_t*c,bool down,int16_t x,int16_t y,uint32_t tick){
 (void)tick;if(!c||!c->enabled)return false;if(down){if(!c->is_touch_down&&ui_rect_contains_point(&c->rect,x,y)){c->is_touch_down=true;c->is_dragged=false;c->down_x=x;c->down_y=y;return true;}if(c->is_touch_down&&(abs(x-c->down_x)>=SLOP||abs(y-c->down_y)>=SLOP))c->is_dragged=true;return c->is_touch_down;}if(!c->is_touch_down)return false;bool open=!c->is_dragged&&ui_rect_contains_point(&c->rect,x,y);c->is_touch_down=false;c->is_dragged=false;if(open){c->pending_gpio=c->selected_gpio;size_t n=ui_pin_config_get_candidate_count(c);size_t rows=(n+LIST_COLUMNS-1u)/LIST_COLUMNS;int16_t h=(int16_t)(2*MARGIN+rows*LIST_ROW_H);if(h<c->modal.sv_body.view_rect.h)h=c->modal.sv_body.view_rect.h;c->modal.sv_body.content_h=h;ui_modal_show(&c->modal,c->title,c);}return true;
}
void ui_pin_config_draw(ui_tile_t*t,const ui_pin_config_t*c){if(!t||!c)return;ui_draw_fill_rect(t,&c->rect,c->color_bg);ui_draw_rect(t,&c->rect,c->color_border);if(c->title)ui_draw_string(t,c->rect.x+10,c->rect.y+14,c->title,FONT_16,c->color_text,c->color_bg);char s[20];if(c->selected_gpio==UINT8_MAX)strcpy(s,"Not selected >");else snprintf(s,sizeof(s),"GP%u >",c->selected_gpio);ui_draw_string(t,c->rect.x+c->rect.w-112,c->rect.y+14,s,FONT_16,c->color_text,c->color_bg);}
bool ui_pin_config_process_modal_touch(ui_pin_config_t*c,bool d,int16_t x,int16_t y,uint32_t tick){return c&&c->modal.is_open?ui_modal_process_touch(&c->modal,d,x,y,tick):false;}
void ui_pin_config_draw_modal(ui_tile_t*t,ui_pin_config_t*c){if(c)ui_modal_draw(t,&c->modal);}
