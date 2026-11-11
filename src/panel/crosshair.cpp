// crosshair.cpp：准星 + 单圈枪管热量环。
//
// 画四段式十字准星、一圈固定白色底环，以及唯一一条从 12 点逆时针增长的枪管热量弧；
// 环下方显示「当前热量 / 上限」，超热量时额外显示 OVERHEAT。
// 数据流：只读 State::robot_dynamic_status.current_heat（10Hz 推送）与
// robot_static_status.max_heat；两者任一缺失即按「无数据」显示 `-- / --`。
// 设计要点：
// · 热量按 1/2、3/4、满上限分档换色（淡青白 → 淡黄 → 红 → 亮红），与官方说明一致；
// · g_shown_heat 是文件内静态平滑值：每帧按 dt*8 向目标比例靠拢，避免 10Hz 数据
// 直接驱动弧长造成肉眼可见的跳动；dt 被 clamp 到 0.1s，防止切窗口回来时猛跳；
// · 弧用 72 段折线近似（kArcSegments），AddPolyline 走 ImGui 1.92.8 的新签名
// (col, thickness, flags)，旧签名参数顺序已废弃；
// · 数值颜色与弧颜色解耦：只在 ≥3/4 或超热量时提高对比度，常态保持低干扰。
// 依赖：ImGui（imgui_internal.h 提供 GetScale）、ui/hud_primitives.h（居中文字）、ui/theme.h。
// 注意：本文件只读 State，不发送任何上行指令；窗口带 NoBackground/NoDecoration，
// 绘制坐标取窗口中心而非视口中心，由主窗口负责把该窗口摆到屏幕中央。
// 2027-11-06 按 DJI 官方《RoboMaster 青少年对抗赛 选手端界面说明》重做热量环；
// 2027-11-08 按要求收敛成「一圈」：只保留枪管热量，从 12 点逆时针增长，
// 按 1/2、3/4 上限分档换色，超热量整圈变红。
// 内层血量环、中层缓冲环、棕色余辉、血量火花线、事件涟漪全部移除。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
// [/pch-split]
#include "crosshair.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/hud_primitives.h"
#include "ui/theme.h"

namespace {

using RoboGo::ui::hud::AddTextCenteredIn;

constexpr float kHeatRingRadius = 76.0f;
constexpr int kArcSegments = 72;
constexpr float kPi = 3.14159265358979323846f;

// 平滑后的热量比例：协议按 10Hz 推送，直接画会让弧长一跳一跳。
float g_shown_heat = 0.0f;

// 一次热量读取的只读快照：当前值、上限、以及「数据是否可信」。
// valid 由 maximum>0 推出，避免把「尚未收到 RobotStaticStatus」误判成超热量。
struct HeatReading {
    float current = 0.0f;
    uint32_t maximum = 0;
    bool valid = false;
};

/**
 * @brief 从 State 读当前枪管热量与上限。
 * @note 思路：dynamic / static 两个 optional 任一缺失都返回 valid=false，调用方据此显示 `-- / --`，不对 0 上限做除法。
 * @return HeatReading 值类型。
 */
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

// 官方分档配色：正常淡青白 → 1/2 上限淡黄 → 3/4 上限红 → 超热量整圈红。
// 思路：四档边界为 0.5 / 0.75 / 1.0，分别对应官方说明中的 1/2、3/4 上限与超热量；
//       超热量单独用更亮的红，让操作手一眼能区分「接近上限」与「已经超了」。
ImU32 HeatColor(float ratio) {
    if (ratio >= 1.0f) return IM_COL32(255, 96, 84, 255);
    if (ratio >= 0.75f) return IM_COL32(245, 96, 84, 245);
    if (ratio >= 0.5f) return IM_COL32(240, 226, 140, 245);
    return IM_COL32(186, 222, 242, 235);
}

}  // namespace

/**
 * @brief 在屏幕中心绘制十字准星 + 单圈热量环 + 热量数值。
 * @param state 只读全局状态。
 * @note 思路：窗口中心取 ImGui 的窗口矩形中心；热量目标值经 g_shown_heat 平滑后驱动弧长，弧从 12 点(-π/2)逆时针延展，用 72 段折线近似整圈；底环恒显，
 *        保证无数据时控件位置仍可见。数值行与 OVERHEAT 文案用居中绘制，不随弧长变化。
 * @note 注意：不发送任何上行指令；ImGui::PushFont/PopFont 只在 hud 原语内部成对出现。
 */
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
    const float radius = kHeatRingRadius * s;

    // 热量比例平滑：每帧向目标值靠拢，避免 10Hz 数据造成弧长抖动。
    const HeatReading heat = ReadHeat(state);
    const float heat_target = heat.valid
                                      ? std::clamp(heat.current / static_cast<float>(heat.maximum), 0.0f, 1.0f)
                                      : 0.0f;
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    g_shown_heat += (heat_target - g_shown_heat) * std::min(1.0f, dt * 8.0f);

    // 四段式十字准星（中心留空，位于环内）。
    const float arm = 10.0f * s;
    const float gap = 3.0f * s;
    const ImU32 cross_color = IM_COL32(225, 235, 248, 235);
    draw_list->AddLine(ImVec2(center.x - arm, center.y), ImVec2(center.x - gap, center.y), cross_color, 1.0f);
    draw_list->AddLine(ImVec2(center.x + gap, center.y), ImVec2(center.x + arm, center.y), cross_color, 1.0f);
    draw_list->AddLine(ImVec2(center.x, center.y - arm), ImVec2(center.x, center.y - gap), cross_color, 1.0f);
    draw_list->AddLine(ImVec2(center.x, center.y + gap), ImVec2(center.x, center.y + arm), cross_color, 1.0f);

    // 固定的白色底环：不随数据变化，数据缺失时也能看到控件位置。
    draw_list->AddCircle(center, radius, IM_COL32(255, 255, 255, 30), kArcSegments, 7.0f * s);

    // 12 点小刻度：标定弧的起点，也提示逆时针方向。
    draw_list->AddLine(ImVec2(center.x, center.y - radius - 9.0f * s),
                       ImVec2(center.x, center.y - radius - 3.0f * s),
                       IM_COL32(238, 242, 247, 110), 1.2f * s);

    // 全环唯一一条热量弧：从 12 点逆时针延展。
    if (g_shown_heat > 0.002f) {
        const float top = -kPi * 0.5f;
        ImVec2 points[kArcSegments + 1];
        for (int i = 0; i <= kArcSegments; ++i) {
            const float step = static_cast<float>(i) / static_cast<float>(kArcSegments);
            const float angle = top - g_shown_heat * 2.0f * kPi * (1.0f - step);
            points[i] = ImVec2(center.x + std::cos(angle) * radius,
                               center.y + std::sin(angle) * radius);
        }
        // ImGui 1.92.8 起签名是 (col, thickness, flags)，旧的 (col, flags, thickness) 已废弃。
        draw_list->AddPolyline(points, kArcSegments + 1, HeatColor(g_shown_heat),
                               7.0f * s, ImDrawFlags_None);
    }

    // 环下方数值行：常态淡灰，超过 3/4 上限或超热量时才提高对比度。
    char value_text[48];
    if (heat.valid) {
        std::snprintf(value_text, sizeof(value_text), "热量 %.0f / %u", heat.current, heat.maximum);
    } else {
        std::snprintf(value_text, sizeof(value_text), "热量 -- / --");
    }
    const bool overheat = heat.valid && heat.current >= static_cast<float>(heat.maximum);
    const ImU32 value_color = overheat ? IM_COL32(255, 120, 108, 255)
                                       : (g_shown_heat >= 0.75f ? IM_COL32(245, 150, 110, 255)
                                                                : RoboGo::ui::kTextTertiary);
    const float label_y = center.y + radius + 10.0f * s;
    AddTextCenteredIn(draw_list, center.x - 90.0f * s, center.x + 90.0f * s, label_y,
                      value_color, value_text, 11);

    if (overheat) {
        AddTextCenteredIn(draw_list, center.x - 70.0f * s, center.x + 70.0f * s,
                          center.y + radius * 0.45f, IM_COL32(255, 120, 108, 255),
                          "OVERHEAT", 10);
    }

    ImGui::End();
}
