// UI 数值与文本工具：比例、时间格式、居中文本。
// 这几件事以前在 commander / header / robot_status 各写一遍，边界处理（除零、夹紧、负数）
// 稍有出入就会让同一份数据在两个界面显示得不一样，因此统一收在这里。
#pragma once

#include <pch.h>

namespace RoboGo::ui {

// 求比例并夹到 [0,1]；上限为 0 时返回 0（防除零）。
inline float RatioOf(uint32_t value, uint32_t max_value) {
    if (max_value == 0) {
        return 0.0f;
    }
    const float ratio = static_cast<float>(value) / static_cast<float>(max_value);
    return ratio > 1.0f ? 1.0f : ratio;
}

// 秒 -> "mm:ss"，写入调用方缓冲区（零分配，可在逐帧路径里用）。
// 负数按 0 处理；超过 99 分钟时分钟位自然溢出两位数，与原有实现保持一致。
inline void FormatClockTo(char (&buffer)[16], int32_t seconds) {
    const int32_t safe = seconds < 0 ? 0 : seconds;
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", safe / 60, safe % 60);
}

// 秒 -> "mm:ss"，返回 std::string（需要拼接或长期保存时用；逐帧路径请用 FormatClockTo）。
inline std::string FormatClock(int32_t seconds) {
    char buffer[16];
    FormatClockTo(buffer, seconds);
    return std::string(buffer);
}

// 把一段文本绘制到给定矩形的正中央（血条数值、图标文字等）。
inline void AddTextCentered(ImDrawList *draw_list, const char *text_begin, const char *text_end,
                            float left, float top, float right, float bottom, ImU32 col) {
    const ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end);
    draw_list->AddText(
            ImVec2(left + (right - left) / 2 - text_size.x / 2, top + (bottom - top) / 2 - text_size.y / 2),
            col, text_begin, text_end);
}

}  // namespace RoboGo::ui
