#pragma once

#include "app.h"

extern const app_desc_t app_weread;

/* Reader opening is shared by local shelf, Home, Files and WeRead. */
bool app_weread_confirm_open(app_ctx_t *ctx, const char *path, bool from_home);
bool app_weread_take_offline(const char *path);
bool app_weread_take_return(void);

int app_book_shelf_slots(void);
EpdRect app_book_shelf_slot(int row);
EpdRect app_book_shelf_cover(int row);
void app_book_shelf_furniture(uint8_t *fb);
/* 右上角两个控件的矩形由书架自己给出，两个书架共用同一份布局常量。 */
EpdRect app_book_shelf_action_rect(bool second);
void app_book_shelf_header(uint8_t *fb, bool second, const char *label, bool pressed);
