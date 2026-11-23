#include "blackboard_display.h"

#include <esp_lvgl_port.h>

#include <string>
#include <vector>

// Use the same built-in text font as the rest of the UI so Chinese renders correctly.
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

const int kMaxLine = 12;
const int kMaxCharPerLine = 42;

lv_obj_t* g_blackboard = nullptr;
lv_obj_t* g_label_chat = nullptr;
bool g_blackboard_mode = false;
bool g_inited = false;
std::vector<std::string> g_chat_lines;

// UTF-8 aware line wrapping: never split a multi-byte character.
void wrap_string(const std::string& in, std::vector<std::string>& out) {
    std::string line;
    int count = 0;
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = 1;
        if ((c & 0x80) == 0) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > in.size()) {
            len = in.size() - i;
        }
        if (c == '\n') {
            out.push_back(line);
            line.clear();
            count = 0;
            i += len;
            continue;
        }
        line.append(in, i, len);
        i += len;
        count++;
        if (count >= kMaxCharPerLine) {
            out.push_back(line);
            line.clear();
            count = 0;
        }
    }
    if (!line.empty()) {
        out.push_back(line);
    }
}

// Must be called while the LVGL lock is held.
void ensure_init() {
    if (g_inited) {
        return;
    }
    lv_obj_t* top = lv_layer_top();
    if (top == nullptr) {
        return;
    }
    g_blackboard = lv_obj_create(top);
    if (g_blackboard == nullptr) {
        return;
    }
    lv_obj_remove_flag(g_blackboard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_blackboard, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(g_blackboard, 0, 0);
    lv_obj_set_style_bg_color(g_blackboard, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_blackboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_blackboard, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_blackboard, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_blackboard, 4, LV_PART_MAIN);

    g_label_chat = lv_label_create(g_blackboard);
    lv_obj_set_style_text_color(g_label_chat, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_label_chat, &BUILTIN_TEXT_FONT, LV_PART_MAIN);
    lv_label_set_long_mode(g_label_chat, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_label_chat, lv_pct(100));
    lv_obj_align(g_label_chat, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(g_label_chat, "");

    lv_obj_add_flag(g_blackboard, LV_OBJ_FLAG_HIDDEN);
    g_inited = true;
}

void refresh_blackboard_text() {
    if (g_label_chat == nullptr) {
        return;
    }
    std::string all;
    for (auto& l : g_chat_lines) {
        all += l;
        all += "\n";
    }
    lv_label_set_text(g_label_chat, all.c_str());
}

}  // namespace

void lv_display_init() {
    if (!lvgl_port_lock(1000)) {
        return;
    }
    ensure_init();
    lvgl_port_unlock();
}

void lv_display_switch_blackboard(bool enable) {
    if (!lvgl_port_lock(1000)) {
        return;
    }
    ensure_init();
    if (g_blackboard != nullptr) {
        g_blackboard_mode = enable;
        if (enable) {
            lv_obj_move_foreground(g_blackboard);
            lv_obj_remove_flag(g_blackboard, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_blackboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_port_unlock();
}

bool lv_display_is_blackboard() { return g_blackboard_mode; }

void lv_display_append_chat(const char* line) {
    if (line == nullptr || line[0] == '\0') {
        return;
    }
    if (!lvgl_port_lock(1000)) {
        return;
    }
    ensure_init();
    std::vector<std::string> wrapped;
    wrap_string(std::string(line), wrapped);
    for (auto& s : wrapped) {
        g_chat_lines.push_back(s);
        if (static_cast<int>(g_chat_lines.size()) > kMaxLine) {
            g_chat_lines.erase(g_chat_lines.begin());
        }
    }
    refresh_blackboard_text();
    lvgl_port_unlock();
}

// Status hooks are no-ops on purpose: the original emoji UI keeps running under the overlay.
void lv_display_set_wake_up() {}
void lv_display_set_listening() {}
void lv_display_set_thinking() {}
void lv_display_set_speaking() {}
void lv_display_set_idle() {}
void lv_display_set_error(const char* msg) { (void)msg; }
