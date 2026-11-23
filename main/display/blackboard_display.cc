#include "blackboard_display.h"

#include <esp_lvgl_port.h>
#include <esp_log.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "board.h"
#include "display.h"
#include "lvgl_display/lvgl_font.h"

// 原生界面用的内置字体（只有拉丁字符，中文靠服务器下发的字形）
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

const char* TAG = "Blackboard";

// 字幕历史最多保留的行数，超出后丢弃最早的行。
const size_t kMaxHistoryLines = 240;
// 版面尚未完成布局时的兜底行数。
const int kFallbackLinesPerPage = 8;

lv_obj_t* g_root = nullptr;        // 全屏黑板容器（挂在 lv_layer_top 上）
lv_obj_t* g_status_label = nullptr;  // 顶部状态行
lv_obj_t* g_chat_label = nullptr;    // 正文
lv_obj_t* g_shadow_a = nullptr;      // 正文的加粗描边层（右）
lv_obj_t* g_shadow_b = nullptr;      // 正文的加粗描边层（下）
lv_timer_t* g_rainbow_timer = nullptr;  // 文字颜色实时渐变
uint16_t g_rainbow_hue = 0;
bool g_inited = false;
bool g_blackboard_mode = false;
std::string g_status_text;                  // 额外状态（音乐、模式提示）
std::vector<std::string> g_lines;           // 已折行的字幕历史
size_t g_first_line = 0;                    // 当前页起始行

// 优先使用原生界面的运行时字体：它挂了“服务器下发字形”的 fallback，
// 用它才能把中文正常显示出来（内置字体只有拉丁字符，中文会变成乱码/方块）。
const lv_font_t* BlackboardFont() {
    // 屏幕的样式字体就是界面运行时字体（已挂服务器字形 fallback），
    // 直接取屏幕当前样式字体，中文才能正常显示。
    lv_obj_t* screen = lv_screen_active();
    if (screen != nullptr) {
        const lv_font_t* font = lv_obj_get_style_text_font(screen, LV_PART_MAIN);
        if (font != nullptr) {
            return font;
        }
    }
    return &BUILTIN_TEXT_FONT;
}

int TextLineHeight() { return lv_font_get_line_height(BlackboardFont()); }

// 版面里一页能放多少行。
int LinesPerPage() {
    if (g_chat_label == nullptr) {
        return kFallbackLinesPerPage;
    }
    int line_height = TextLineHeight();
    int height = lv_obj_get_content_height(g_chat_label);
    if (line_height <= 0 || height <= 0) {
        return kFallbackLinesPerPage;
    }
    int lines = height / line_height;
    return lines > 0 ? lines : 1;
}

// 估算每行能放多少“半角字符宽度”：中文按 2 个半角宽度计算。
int HalfWidthsPerLine() {
    if (g_chat_label == nullptr) {
        return 40;
    }
    int line_height = TextLineHeight();
    int width = lv_obj_get_content_width(g_chat_label);
    if (line_height <= 0 || width <= 0) {
        return 40;
    }
    int half_width = line_height / 2;
    if (half_width < 1) {
        half_width = 1;
    }
    int count = width / half_width;
    return count < 8 ? 8 : count;
}

// 按显示宽度折行，且绝不切断 UTF-8 多字节字符。
void WrapString(const std::string& in, std::vector<std::string>& out, int max_half_widths) {
    std::string line;
    int width = 0;
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
            width = 0;
            i += len;
            continue;
        }
        // 中文/全角字符按两个半角宽度计算。
        int char_width = (len >= 3) ? 2 : 1;
        if (width + char_width > max_half_widths && !line.empty()) {
            out.push_back(line);
            line.clear();
            width = 0;
        }
        line.append(in, i, len);
        width += char_width;
        i += len;
    }
    if (!line.empty()) {
        out.push_back(line);
    }
}

// 正文（含加粗描边层）统一改色。
void ApplyTextColor(lv_color_t color) {
    if (g_chat_label != nullptr) {
        lv_obj_set_style_text_color(g_chat_label, color, LV_PART_MAIN);
    }
    if (g_shadow_a != nullptr) {
        lv_obj_set_style_text_color(g_shadow_a, color, LV_PART_MAIN);
    }
    if (g_shadow_b != nullptr) {
        lv_obj_set_style_text_color(g_shadow_b, color, LV_PART_MAIN);
    }
}

// 定时器：色相缓慢推进，让黑板文字颜色实时流转。
void RainbowTimerCb(lv_timer_t* timer) {
    (void)timer;
    g_rainbow_hue = static_cast<uint16_t>((g_rainbow_hue + 3) % 360);
    ApplyTextColor(lv_color_hsv_to_rgb(g_rainbow_hue, 90, 100));
}

// 必须在持有 LVGL 锁的情况下调用。
void EnsureInit() {
    if (g_inited) {
        return;
    }
    lv_obj_t* top = lv_layer_top();
    if (top == nullptr) {
        return;
    }
    g_root = lv_obj_create(top);
    if (g_root == nullptr) {
        return;
    }
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_root, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(g_root, 0, 0);
    lv_obj_set_style_bg_color(g_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(g_root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(g_root, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    lv_obj_set_style_border_opa(g_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(g_root, 0, LV_PART_MAIN);
    lv_obj_set_style_opa(g_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_root, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(g_root, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(g_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    g_status_label = lv_label_create(g_root);
    lv_obj_set_width(g_status_label, lv_pct(100));
    lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_status_label, BlackboardFont(), LV_PART_MAIN);
    lv_label_set_text(g_status_label, "黑板字幕");

    // 加粗（近似黑体）效果：正文下方先铺两层同样文字、错开 1 像素的同色描边，
    // 真正的那一份再盖上去，笔画就会显粗。描边层不参与 flex 布局。
    g_shadow_a = lv_label_create(g_root);
    g_shadow_b = lv_label_create(g_root);
    lv_obj_t* shadows[2] = {g_shadow_a, g_shadow_b};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* s = shadows[i];
        lv_obj_add_flag(s, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(s, lv_pct(100));
        lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(s, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s, 0, LV_PART_MAIN);
        lv_obj_set_style_text_font(s, BlackboardFont(), LV_PART_MAIN);
        lv_obj_set_style_text_color(s, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_label_set_text(s, "");
    }

    g_chat_label = lv_label_create(g_root);
    lv_obj_set_width(g_chat_label, lv_pct(100));
    lv_obj_set_flex_grow(g_chat_label, 1);
    lv_obj_set_style_text_color(g_chat_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_chat_label, BlackboardFont(), LV_PART_MAIN);
    lv_obj_set_style_text_align(g_chat_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_long_mode(g_chat_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_chat_label, "");

    // 文字颜色实时流转。
    if (g_rainbow_timer == nullptr) {
        g_rainbow_timer = lv_timer_create(RainbowTimerCb, 120, nullptr);
    }
    ApplyTextColor(lv_color_hsv_to_rgb(g_rainbow_hue, 90, 100));

    // 先做一次布局，保证宽高可用（否则首屏行数会用到兜底值）。
    lv_obj_update_layout(g_root);

    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    g_inited = true;
}

// 同时更新正文与其描边层，并把描边层对准正文（错开 1 像素形成加粗）。
void SetBodyText(const std::string& body) {
    if (g_chat_label == nullptr) {
        return;
    }
    lv_label_set_text(g_chat_label, body.c_str());
    if (g_shadow_a != nullptr) {
        lv_label_set_text(g_shadow_a, body.c_str());
    }
    if (g_shadow_b != nullptr) {
        lv_label_set_text(g_shadow_b, body.c_str());
    }
    lv_obj_update_layout(g_chat_label);
    if (g_shadow_a != nullptr) {
        lv_obj_align_to(g_shadow_a, g_chat_label, LV_ALIGN_TOP_LEFT, 1, 0);
    }
    if (g_shadow_b != nullptr) {
        lv_obj_align_to(g_shadow_b, g_chat_label, LV_ALIGN_TOP_LEFT, 0, 1);
    }
}

// 必须在持有 LVGL 锁的情况下调用。
void Refresh() {
    if (g_chat_label == nullptr || g_status_label == nullptr) {
        return;
    }
    // 主题字体可能在资源加载后被替换过，每页刷新时重新挂一次，避免用旧字体渲染中文。
    lv_obj_set_style_text_font(g_status_label, BlackboardFont(), LV_PART_MAIN);
    lv_obj_set_style_text_font(g_chat_label, BlackboardFont(), LV_PART_MAIN);
    int per_page = LinesPerPage();
    size_t total = g_lines.size();
    if (total == 0) {
        SetBodyText("");
        std::string title = g_status_text.empty() ? "黑板字幕" : g_status_text;
        title += "  [0/0]";
        lv_label_set_text(g_status_label, title.c_str());
        return;
    }
    if (g_first_line + per_page > total) {
        g_first_line = total > static_cast<size_t>(per_page)
                           ? total - static_cast<size_t>(per_page)
                           : 0;
    }
    size_t pages = (total + per_page - 1) / per_page;
    if (pages == 0) {
        pages = 1;
    }
    size_t page_no = g_first_line / per_page + 1;

    std::string title = g_status_text.empty() ? "黑板字幕" : g_status_text;
    title += "  [";
    title += std::to_string(page_no);
    title += "/";
    title += std::to_string(pages);
    title += "]";
    lv_label_set_text(g_status_label, title.c_str());

    std::string body;
    for (size_t i = g_first_line; i < total && i < g_first_line + static_cast<size_t>(per_page); ++i) {
        body += g_lines[i];
        body += "\n";
    }
    SetBodyText(body);
}

// 必须在持有 LVGL 锁的情况下调用。follow_tail 为 true 时视图跟随最新内容。
void AppendLocked(const std::string& text, bool follow_tail, bool reset_to_start) {
    if (text.empty()) {
        return;
    }
    std::vector<std::string> wrapped;
    WrapString(text, wrapped, HalfWidthsPerLine());
    size_t message_start = g_lines.size();
    for (auto& line : wrapped) {
        g_lines.push_back(line);
    }
    if (g_lines.size() > kMaxHistoryLines) {
        size_t drop = g_lines.size() - kMaxHistoryLines;
        g_lines.erase(g_lines.begin(), g_lines.begin() + static_cast<std::ptrdiff_t>(drop));
        message_start = message_start > drop ? message_start - drop : 0;
    }
    if (reset_to_start) {
        // 新一轮对话：从这段内容的开头显示，长讲解可以按“下一页”往后读。
        g_first_line = message_start;
    } else if (follow_tail) {
        int per_page = LinesPerPage();
        g_first_line = g_lines.size() > static_cast<size_t>(per_page)
                           ? g_lines.size() - static_cast<size_t>(per_page)
                           : 0;
    }
    Refresh();
}

}  // namespace

void lv_display_init() {
    if (!lvgl_port_lock(1000)) {
        return;
    }
    EnsureInit();
    lvgl_port_unlock();
}

void lv_display_switch_blackboard(bool enable) {
    if (!lvgl_port_lock(1000)) {
        return;
    }
    EnsureInit();
    if (g_root != nullptr) {
        g_blackboard_mode = enable;
        if (enable) {
            // 原生界面整屏隐藏，保证整屏纯黑、不被上层 UI 抢焦点
            lv_obj_add_flag(lv_screen_active(), LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(g_root);
            lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
            Refresh();
        } else {
            lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_HIDDEN);
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
    EnsureInit();
    AppendLocked(std::string(line), true, false);
    lvgl_port_unlock();
}

void lv_display_append_message(const char* role, const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    std::string role_str = role == nullptr ? "" : role;
    // 黑板只显示小智说的内容：用户自己说的话不上屏，也不加任何前缀。
    if (role_str == "user") {
        return;
    }
    std::string body = text;
    if (!lvgl_port_lock(1000)) {
        return;
    }
    EnsureInit();
    // 字幕式滚动：始终跟到最新一行，需要回看时用“上一页”。
    AppendLocked(body, true, false);
    lvgl_port_unlock();
}

void lv_display_clear() {
    if (!lvgl_port_lock(1000)) {
        return;
    }
    EnsureInit();
    g_lines.clear();
    g_first_line = 0;
    Refresh();
    lvgl_port_unlock();
}

void lv_display_page_next() {
    if (!lvgl_port_lock(1000)) {
        return;
    }
    EnsureInit();
    int per_page = LinesPerPage();
    if (g_lines.empty()) {
        lvgl_port_unlock();
        return;
    }
    if (g_first_line + per_page < g_lines.size()) {
        g_first_line += per_page;
        if (g_first_line + per_page > g_lines.size()) {
            g_first_line = g_lines.size() > static_cast<size_t>(per_page)
                               ? g_lines.size() - static_cast<size_t>(per_page)
                               : 0;
        }
    }
    Refresh();
    lvgl_port_unlock();
}

void lv_display_page_prev() {
    if (!lvgl_port_lock(1000)) {
        return;
    }
    EnsureInit();
    int per_page = LinesPerPage();
    if (g_first_line >= static_cast<size_t>(per_page)) {
        g_first_line -= per_page;
    } else {
        g_first_line = 0;
    }
    Refresh();
    lvgl_port_unlock();
}

void lv_display_set_status(const char* status) {
    if (!lvgl_port_lock(1000)) {
        return;
    }
    EnsureInit();
    g_status_text = status == nullptr ? "" : status;
    Refresh();
    lvgl_port_unlock();
}

// 状态钩子在黑板上不做处理：原生表情界面继续按原有逻辑运行。
void lv_display_set_wake_up() {
    if (lv_display_is_blackboard()) {
        ESP_LOGD(TAG, "wake up");
    }
}
void lv_display_set_listening() {}
void lv_display_set_thinking() {}
void lv_display_set_speaking() {}
void lv_display_set_idle() {}
void lv_display_set_error(const char* msg) {
    (void)msg;
}
