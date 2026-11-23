#ifndef BLACKBOARD_DISPLAY_H
#define BLACKBOARD_DISPLAY_H

#include <lvgl.h>

#include <string>

// 黑板字幕层：在原生表情界面之上叠加一层可选的“黑底白字”显示面。
// 只负责显示，不参与语音识别、对话请求与音频播放逻辑。

void lv_display_init();
void lv_display_set_wake_up();
void lv_display_set_listening();
void lv_display_set_thinking();
void lv_display_set_speaking();
void lv_display_set_idle();
void lv_display_set_error(const char* msg);

// 追加一行纯文本（不自动加说话人前缀）。
void lv_display_append_chat(const char* line);

// 追加一条消息，role 为 "user" / "assistant"，会自动加“我：”“智脑：”前缀。
void lv_display_append_message(const char* role, const char* text);

// 清空黑板上的全部字幕。
void lv_display_clear();

// 字幕翻页：上一页 / 下一页。
void lv_display_page_prev();
void lv_display_page_next();

// 顶部状态行文字（例如当前音乐、模式提示），传空字符串表示恢复默认标题。
void lv_display_set_status(const char* status);

void lv_display_switch_blackboard(bool enable);
bool lv_display_is_blackboard();

#endif  // BLACKBOARD_DISPLAY_H
