#ifndef BLACKBOARD_DISPLAY_H
#define BLACKBOARD_DISPLAY_H

#include <lvgl.h>

#include <string>

void lv_display_init();
void lv_display_set_wake_up();
void lv_display_set_listening();
void lv_display_set_thinking();
void lv_display_set_speaking();
void lv_display_set_idle();
void lv_display_set_error(const char* msg);
void lv_display_append_chat(const char* line);
void lv_display_switch_blackboard(bool enable);
bool lv_display_is_blackboard();

#endif  // BLACKBOARD_DISPLAY_H
