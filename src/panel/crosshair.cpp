// crosshair.cpp：准星 + 枪管热量环。
//
// 2027-11-06 按 DJI 官方《RoboMaster 青少年对抗赛 选手端界面说明》重做热量环：
//   · 辅助射击区 = 准星 + 热量环；空环 Q1=0、热量正常、1/2 Q0 黄色警示、3/4 Q0 红色警示、
//     Q1>=Q0 超热量（整圈红 + Overheat）；
//   · 环从 12 点方向逆时针增长（用户评审后改向），热量冷却时弧长随时间倒退（数据 10Hz，再叠一层平滑）；
//   · 环下方补上「热量 当前 / 上限」数值，操作手不用去左下角本车卡找。
// 旧实现只在热量 >1% 时画一段从 3 点方向起、绿→黄→红渐变的弧，且没有底环与数值。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
// [/pch-split]
#include "crosshair.h"

#include <algorithm>
#include <cstdio>

#include "ui/hud_primitives.h"
#include "ui/theme.h"

namespace {

using RoboGo::ui::hud::AddTextCenteredIn;

constexpr float kRingRadius = 36.0f;
constexpr float kBaseThickness = 2.5f;
constexpr float kFillThickness = 3.5f;
constexpr int kArcSegments = 72;
constexpr float kPi = 3.14159265358979323846f;

struct HeatReading {
    float current = 0.0f;
    uint32_t maximum = 0;
    bool valid = false;
};

HeatReading ReadHeat(const State &state) {
    HeatReading reading;
    if (!state.robot_dynamic_status.has_value() || !state.robot_static_status.has_value()) {
        return reading;
    }
    reading.current = state.robot_dynamic_status->current_heat();
    reading.maximum = state.robot_static_status->max_heat();
    reading.valid = reading.maximum > 0;
    return reading;
}

// 固定步长逼近：数据 10Hz，硬切会让弧跳；冷却时也是逐帧平滑倒退。
float MoveTowards(float current, float target, float max_delta) {
    return current < target ? std::min(current + max_delta, target)
                            : std::max(current - max_delta, target);
}

// 官方分档配色：正常淡青白 → 1/2 Q0 淡黄 → 3/4 Q0 红 → 超热量整圈红。
ImU32 HeatArcColor(float ratio) {
    if (ratio >= 1.0f) {
        return IM_COL32(255, 96, 84, 255);
    }
    if (ratio >= 0.75f) {
        return IM_COL32(245, 96, 84, 245);
    }
    if (ratio >= 0.5f) {
        return IM_COL32(240, 226, 140, 245);
    }
    return IM_COL32(186, 222, 242, 235);
}

}  // namespace

// 功能：在屏幕中心绘制十字准星 + 环形热量读条 + 热量数值。
void ShowCrosshair(const State &state) {
    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration
                                          | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground
                                          | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("CrosshairWindow", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const ImVec2 center(window_pos.x + window_size.x * 0.5f, window_pos.y + window_size.y * 0.5f);
    const float s = ImGui::GetScale();
    const float radius = kRingRadius * s;

    // 四段式十字准星（中心留空，与官方辅助射击区一致）。
    const float arm = 10.0f * s;
    const float gap = 3.0f * s;
    const ImU32 cross_color = IM_COL32(225, 235, 248, 235);
    draw_list->AddLine(ImVec2(center.x - arm, center.y), ImVec2(center.x - gap, center.y), cross_color, 1.0f);
    draw_list->AddLine(ImVec2(center.x + gap, center.y), ImVec2(center.x + arm, center.y), cross_color, 1.0f);
    draw_list->AddLine(ImVec2(center.x, center.y - arm), ImVec2(center.x, center.y - gap), cross_color, 1.0f);
    draw_list->AddLine(ImVec2(center.x, center.y + gap), ImVec2(center.x, center.y + arm), cross_color, 1.0f);

    // 底环：空热量时也保持一个细灰圈，让操作手知道这个控件存在。
    draw_list->AddCircle(center, radius, IM_COL32(255, 255, 255, 48), kArcSegments, kBaseThickness * s);

    const HeatReading heat = ReadHeat(state);
    const float target_ratio = heat.valid
                                       ? std::clamp(heat.current / static_cast<float>(heat.maximum), 0.0f, 1.0f)
                                       : 0.0f;
    static float shown_ratio = 0.0f;
    shown_ratio = MoveTowards(shown_ratio, target_ratio, ImGui::GetIO().DeltaTime * 3.0f);

    // 从 12 点（-90°）逆时针填充：热量越高，弧沿 11 点 / 10 点方向往左长；
    // 冷却时末端逐帧退回 12 点。ImGui 要求 a_min <= a_max，所以把区间写成
    // [ -π/2 - ratio·2π, -π/2 ]，路径仍按角度递增生成，覆盖的正是逆时针那一段。
    if (shown_ratio > 0.002f) {
        const float top_angle = -kPi * 0.5f;
        const float start_angle = top_angle - shown_ratio * 2.0f * kPi;
        const ImU32 arc_color = HeatArcColor(shown_ratio);
        draw_list->PathArcTo(center, radius, start_angle, top_angle, kArcSegments);
        draw_list->PathStroke(arc_color, kFillThickness * s, ImDrawFlags_None);
    }

    // 环下方数值行：热量 当前 / 上限；超热量时整行转红并补英文提示。
    char value_text[48];
    if (heat.valid) {
        std::snprintf(value_text, sizeof(value_text), "热量 %.0f / %u", heat.current, heat.maximum);
    } else {
        std::snprintf(value_text, sizeof(value_text), "热量 -- / --");
    }
    const bool overheat = heat.valid && heat.current >= static_cast<float>(heat.maximum);
    const ImU32 value_color = overheat ? IM_COL32(255, 120, 108, 255)
                                       : (shown_ratio >= 0.75f ? IM_COL32(245, 150, 110, 255)
                                                                : RoboGo::ui::kTextTertiary);
    const float label_y = center.y + radius + 10.0f * s;
    AddTextCenteredIn(draw_list, center.x - 90.0f * s, center.x + 90.0f * s, label_y,
                      value_color, value_text, 11);

    // 超热量：整圈已红，再在环内下方压一行 Overheat，和官方一致。
    if (overheat) {
        AddTextCenteredIn(draw_list, center.x - 70.0f * s, center.x + 70.0f * s,
                          center.y + radius * 0.45f, IM_COL32(255, 120, 108, 255),
                          "OVERHEAT", 10);
    }

    ImGui::End();
}
