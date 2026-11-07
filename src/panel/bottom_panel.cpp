// bottom_panel.cpp：底部「控制度带」的实现。
// 2027-11-06 按「主界面定稿」：把两组长条 + 构成行合成一条居中的双色带，
// 控制度用大数字，总伤害 / 存活 / 图传清晰度压到一行；复活与装配状态仍在下方居中显示。
// 数据来源与缓动策略不变，只改呈现。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
#include <messages.pb.h>
// [/pch-split]
#include "bottom_panel.h"

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <string>

#include "app/robot_registry.h"
#include "app/team_util.h"
#include "ui/hud_primitives.h"
#include "ui/theme.h"

namespace {

using RoboGo::ui::WithAlpha;
using RoboGo::ui::hud::AddText;
using RoboGo::ui::hud::AddTextCenteredIn;
using RoboGo::ui::hud::AddTextRightAligned;
using RoboGo::ui::hud::TextSize;

// 与顶部队伍条保持同一组「有血量」单位。
constexpr std::array<int, 4> kHudRobotIds = {1, 3, 4, 7};

// 上一帧底部条实际占用的高度：左下角本车卡靠它让位。
float g_last_panel_height = 0.0f;

// 己方在总量中的占比；双方都为 0 时按对半，避免开局显示成一条纯对方色的空带。
float CalcAllyRatio(uint32_t ally, uint32_t enemy) {
    const float total = static_cast<float>(ally) + static_cast<float>(enemy);
    return total <= 0.0f ? 0.5f : static_cast<float>(ally) / total;
}

// 固定步长逼近目标比例：数据 1Hz，硬切会闪。
float MoveTowards(float current, float target, float max_delta) {
    return current < target ? std::min(current + max_delta, target)
                            : std::max(current - max_delta, target);
}

// 复活 / 装配等状态文本；没有内容就不产生行，让底部条自然收缩。
struct StatusLine {
    std::string text;
    ImVec4 color;
};

std::vector<StatusLine> CollectStatusLines(const State &state) {
    std::vector<StatusLine> lines;
    if (state.robot_respawn_status.has_value() && state.robot_respawn_status->is_pending_respawn()) {
        const auto &respawn = *state.robot_respawn_status;
        lines.push_back({"复活进度: " + std::to_string(respawn.current_respawn_progress())
                                 + "/" + std::to_string(respawn.total_respawn_progress()),
                         ImVec4(1.0f, 1.0f, 0.3f, 1.0f)});
    }
    if (state.tech_core_motion_state_sync.has_value()) {
        const auto &sync = *state.tech_core_motion_state_sync;
        if (sync.basic_state() >= 2 && sync.basic_state() <= 3) {
            std::string status_text;
            if (sync.basic_state() == 2) {
                status_text = "科技核心移动中";
            } else {
                std::vector<std::string> steps;
                if (sync.putin_state() == 1) steps.push_back("已放入");
                if (sync.move_state() == 1) steps.push_back("已平移");
                if (sync.rotate_state() == 1) steps.push_back("已旋转");
                status_text = steps.empty() ? "等待装配"
                                            : "装配中: " + std::accumulate(
                                                      steps.begin(), steps.end(), std::string(),
                                                      [](const std::string &a, const std::string &b) {
                                                          return a.empty() ? b : a + " -> " + b;
                                                      });
            }
            if (sync.remain_time_step() > 0) {
                status_text += " (" + std::to_string(sync.remain_time_step()) + "s)";
            }
            lines.push_back({"重装装配状态: " + status_text, ImVec4(0.3f, 0.8f, 1.0f, 1.0f)});
        }
    }
    return lines;
}

// 图传清晰度提示：2027 规则 5.6.1，己方基地掉血会让图传按 λ² 降采样。
void ComputeQualityHint(const GlobalUnitStatus &unit_status, std::string &text, ImVec4 &color) {
    color = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    if (unit_status.base_health() >= 10000) {
        return;
    }
    const float lambda = 1.0f - static_cast<float>(10000 - unit_status.base_health()) / 20000.0f;
    if (lambda > 0.98f) {
        return;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "图传 %.2f", lambda);
    text = buffer;
    if (lambda < 0.5f) {
        color = ImVec4(0.95f, 0.30f, 0.30f, 1.0f);
    } else if (lambda < 0.75f) {
        color = ImVec4(0.95f, 0.60f, 0.15f, 1.0f);
    }
}

int CountAlive(const State &state, bool we_are_red) {
    const RobotStatus *robots = we_are_red ? state.red_robots : state.blue_robots;
    int alive = 0;
    for (int id : kHudRobotIds) {
        if (robots[id - 1].health > 0) {
            ++alive;
        }
    }
    return alive;
}

// 双色带：己方从左、对方从右，中间一根 50% 刻度。
void DrawControlBand(ImDrawList *draw_list, const ImVec2 &top_left, float width, float height,
                     float ally_ratio, ImU32 ally_color, ImU32 enemy_color, const RoboGo::ui::hud::Colors &colors) {
    const float rounding = height * 0.5f;
    const float split_x = top_left.x + width * std::clamp(ally_ratio, 0.0f, 1.0f);
    const float right_x = top_left.x + width;
    const float bottom_y = top_left.y + height;
    draw_list->AddRectFilled(top_left, ImVec2(right_x, bottom_y), colors.slot, rounding);
    if (split_x - top_left.x > 1.0f) {
        draw_list->AddRectFilled(top_left, ImVec2(split_x, bottom_y), ally_color, rounding,
                                 ImDrawFlags_RoundCornersLeft);
    }
    if (right_x - split_x > 1.0f) {
        draw_list->AddRectFilled(ImVec2(split_x, top_left.y), ImVec2(right_x, bottom_y), enemy_color,
                                 rounding, ImDrawFlags_RoundCornersRight);
    }
    const float mid_x = top_left.x + width * 0.5f;
    draw_list->AddLine(ImVec2(mid_x, top_left.y + 1.0f), ImVec2(mid_x, bottom_y - 1.0f),
                       IM_COL32(0, 0, 0, 150), 1.0f);
    draw_list->AddRect(top_left, ImVec2(right_x, bottom_y), colors.border, rounding, 0.5f);
}

}  // namespace

// 功能：在屏幕底部居中绘制控制度带与状态行。
void ShowBottomPanel(const State &state) {
    const ImVec2 viewport_size = ImGui::GetMainViewport()->Size;
    const float s = ImGui::GetScale();
    const RoboGo::ui::hud::Colors colors = RoboGo::ui::hud::MakeColors();
    const std::vector<StatusLine> status_lines = CollectStatusLines(state);

    const float bottom_margin = 28.0f * s;
    const float row1_h = 20.0f * s;
    const float gap1 = 9.0f * s;
    const float band_h = 6.0f * s;
    const float gap2 = 9.0f * s;
    const float row2_h = 12.0f * s;
    const float status_line_h = 14.0f * s;
    const float panel_height = 4.0f * s + static_cast<float>(status_lines.size()) * status_line_h
                               + row1_h + gap1 + band_h + gap2 + row2_h + bottom_margin;
    const float panel_top = viewport_size.y - panel_height;

    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration
                                          | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                          | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowPos(ImVec2(0, panel_top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport_size.x, panel_height), ImGuiCond_Always);
    if (!ImGui::Begin("BottomPanel", nullptr, window_flags)) {
        ImGui::End();
        g_last_panel_height = panel_height;
        return;
    }

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const float band_w = 400.0f * s;
    const float band_left = (viewport_size.x - band_w) * 0.5f;
    const float band_right = band_left + band_w;
    float y = panel_top + 4.0f * s;

    for (const auto &line : status_lines) {
        AddTextCenteredIn(draw_list, 0.0f, viewport_size.x, y,
                          ImGui::ColorConvertFloat4ToU32(line.color), line.text.c_str(), 10);
        y += status_line_h;
    }

    const bool we_are_red = RoboGo::app::team_util::WeAreRed(state);
    const ImU32 ally_color = we_are_red ? colors.red : colors.blue;
    const ImU32 enemy_color = we_are_red ? colors.blue : colors.red;
    const ImU32 ally_number_color = RoboGo::ui::Tint(ally_color, 0.0f, 0.30f);
    const ImU32 enemy_number_color = RoboGo::ui::Tint(enemy_color, 0.0f, 0.30f);

    uint32_t cp_ally = 0, cp_enemy = 0, dmg_ally = 0, dmg_enemy = 0;
    std::string quality_text;
    ImVec4 quality_color(0.70f, 0.70f, 0.70f, 1.0f);
    if (state.global_unit_status.has_value()) {
        const GlobalUnitStatus &unit = *state.global_unit_status;
        cp_ally = unit.total_cp_ally();
        cp_enemy = unit.total_cp_enemy();
        dmg_ally = unit.total_damage_ally();
        dmg_enemy = unit.total_damage_enemy();
        ComputeQualityHint(unit, quality_text, quality_color);
    }

    static float cp_ratio_shown = 0.5f;
    cp_ratio_shown = MoveTowards(cp_ratio_shown, CalcAllyRatio(cp_ally, cp_enemy),
                                 ImGui::GetIO().DeltaTime * 1.2f);

    // 第一行：标签 + 己方/对方大数字
    AddText(draw_list, ImVec2(band_left, y + 3.0f * s), colors.dim, "控制度", 10);
    const std::string cp_ally_text = std::to_string(cp_ally);
    const std::string cp_enemy_text = std::to_string(cp_enemy);
    const float cp_enemy_w = TextSize(cp_enemy_text.c_str(), 17).x;
    AddTextRightAligned(draw_list, band_right, y, enemy_number_color, cp_enemy_text.c_str(), 17);
    AddTextRightAligned(draw_list, band_right - cp_enemy_w - 12.0f * s, y + 3.0f * s,
                        colors.dim, ":", 10);
    AddTextRightAligned(draw_list, band_right - cp_enemy_w - 24.0f * s, y,
                        ally_number_color, cp_ally_text.c_str(), 17);
    y += row1_h + gap1;

    // 双色带
    DrawControlBand(draw_list, ImVec2(band_left, y), band_w, band_h, cp_ratio_shown,
                    ally_color, enemy_color, colors);
    y += band_h + gap2;

    // 第二行：总伤害 / 存活 / 图传清晰度
    char second_text[96];
    std::snprintf(second_text, sizeof(second_text), "总伤害 %u : %u", dmg_ally, dmg_enemy);
    AddText(draw_list, ImVec2(band_left, y), colors.dim, second_text, 9.5f);
    float second_x = band_left + TextSize(second_text, 9.5f).x + 12.0f * s;
    if (!quality_text.empty()) {
        AddText(draw_list, ImVec2(second_x, y), ImGui::ColorConvertFloat4ToU32(quality_color),
                quality_text.c_str(), 9.5f);
    }
    char alive_text[32];
    std::snprintf(alive_text, sizeof(alive_text), "存活 %d / 4", CountAlive(state, we_are_red));
    AddTextRightAligned(draw_list, band_right, y, colors.text2, alive_text, 9.5f);

    ImGui::End();
    g_last_panel_height = panel_height;
}

// 功能：返回底部条上一帧占用的高度（供其它贴底面板让位）。
float GetBottomPanelHeight() {
    return g_last_panel_height;
}
