/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 屏幕下方触摸按键区的命中。全屏应用菜单与底栏把手已按用户要求移除。
 *
 * Hit-test for the touch-key strip below the display. The full-screen app
 * menu and the bar handle have been removed at the user's request.
 */

#include "ui_menu.h"

// 按 UI_KEY_PITCH 均分，三个按键的中心（80/240/400）正好落在各段中点，
// 手指偏一点也还在同一段里。
// Split by UI_KEY_PITCH so the three key centers (80/240/400) sit at mid-segment;
// a slight finger miss still lands in the same segment.
int ui_key_hit_test(uint16_t x, uint16_t y) {
#if KIIKO_BOARD_METALIO47
    if (y < 1570 || y > 1630) return -1;
    if (x >= 170 && x <= 230) return UI_KEY_1;
    if (x >= 70 && x <= 130) return UI_KEY_2;
    if (x >= 270 && x <= 330) return UI_KEY_3;
    return -1;
#else
    if (y < UI_KEY_AREA_TOP) return -1;
    int index = (int)x / UI_KEY_PITCH;
    if (index >= UI_KEY_COUNT) return -1;
    return index;
#endif
}
