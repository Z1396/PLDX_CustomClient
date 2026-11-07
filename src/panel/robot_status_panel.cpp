// robot_status_panel.cpp：己方机器人「本车卡」的实现。
// 2027-11-06 按「主界面定稿」：由堆叠的彩色文字+进度条，改为一张卡片
// （身份 + 32px 大血量数字 + 血条 + 热量/弹量/缓冲能量三栏）。
// 设备连接状态、42mm 装配权限、自瞄/小陀螺等原有信息全部保留，压缩到卡片底部两行。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
// [/pch-split]
#include "robot_status_panel.h"

#include <cstdio>
#include <string>

#include "app/robot_registry.h"
#include "ui/format_utils.h"
#include "ui/hud_primitives.h"
#include "ui/theme.h"

namespace {

using RoboGo::ui::WithAlpha;
using RoboGo::ui::hud::AddText;
using RoboGo::ui::hud::AddTextRightAligned;
using RoboGo::ui::hud::TextSize;

// 把过长的一行按 UTF-8 字符边界裁短，尾部加省略号，避免压出卡片。
std::string FitLine(const std::string &text, float max_width, float font_size) {
    ImGui::PushFont(ImGui::GetFont(), font_size);
    std::string result = text;
    bool trimmed = false;
    while (!result.empty() && ImGui::CalcTextSize(result.c_str()).x > max_width) {
        std::size_t i = result.size() - 1;
        while (i > 0 && (static_cast<unsigned char>(result[i]) & 0xC0u) == 0x80u) {
            --i;
        }
        result.erase(i);
        trimmed = true;
    }
    ImGui::PopFont();
    if (trimmed && !result.empty()) {
        std::size_t i = result.size() - 1;
        while (i > 0 && (static_cast<unsigned char>(result[i]) & 0xC0u) == 0x80u) {
            --i;
        }
        result.erase(i);
        result += "…";
    }
    return result;
}

// 10 个模块的连接状态（与协议 2.2.12 顺序一致）。
const char *const kModuleNames[] = {
        "主控", "电源管理", "图传", "RFID", "UWB",
        "装甲板", "小弹丸", "大弹丸", "超级电容", "灯条"};

std::string CollectOfflineModules(const State &state, bool &has_any) {
    has_any = false;
    std::string result;
    if (!state.robot_module_status.has_value()) {
        return "未知";
    }
    const uint32_t status[] = {
            state.robot_module_status->main_controller(),
            state.robot_module_status->power_manager(),
            state.robot_module_status->video_transmission(),
            state.robot_module_status->rfid(),
            state.robot_module_status->uwb(),
            state.robot_module_status->armor(),
            state.robot_module_status->small_shooter(),
            state.robot_module_status->big_shooter(),
            state.robot_module_status->capacitor(),
            state.robot_module_status->light_strip()};
    static_assert(std::size(kModuleNames) == std::size(status));
    for (size_t i = 0; i < std::size(status); ++i) {
        if (status[i] == 1) {
            continue;
        }
        if (!result.empty()) {
            result += ", ";
        }
        result += kModuleNames[i];
        if (status[i] == 2) {
            result += "(安装不规范)";
        }
    }
    has_any = !result.empty();
    return has_any ? result : "全部在线";
}

}  // namespace

// 功能：绘制本车卡（血量、热量、弹量、缓冲能量、等级、模式与设备状态）。
void ShowRobotStatusPanel(const State &state) {
    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration
                                          | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground
                                          | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs;
    if (!ImGui::Begin("VehicleStatusPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const float s = ImGui::GetScale();
    namespace ui = RoboGo::ui;

    const ImU32 text = ui::kTextPrimary;
    const ImU32 text2 = ui::kTextSecondary;
    const ImU32 dim = ui::kTextTertiary;
    const ImU32 red = ui::kTeamRed;
    const ImU32 border = ui::kBorder;
    const ImU32 slot = WithAlpha(0xFFFFFFFFu, 18);

    // 卡面
    draw_list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                             WithAlpha(ui::kSurface1, 215), 14.0f * s);
    draw_list->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), border, 14.0f * s, 0.5f);

    // 数据（缺状态时全部按 0 处理，不臆测）
    const bool has_static = state.robot_static_status.has_value();
    const bool has_dynamic = state.robot_dynamic_status.has_value();
    const uint32_t current_health = has_dynamic ? state.robot_dynamic_status->current_health() : 0;
    const uint32_t max_health = has_static ? state.robot_static_status->max_health() : 0;
    const uint32_t level = has_static ? state.robot_static_status->level() : 0;
    const uint32_t max_heat = has_static ? state.robot_static_status->max_heat() : 0;
    const uint32_t max_buffer = has_static ? state.robot_static_status->max_buffer_energy() : 0;
    const float current_heat = has_dynamic ? state.robot_dynamic_status->current_heat() : 0.0f;
    const uint32_t current_buffer = has_dynamic ? state.robot_dynamic_status->current_buffer_energy() : 0;
    const uint32_t ammo = has_dynamic ? state.robot_dynamic_status->remaining_ammo() : 0;
    const uint32_t exp = has_dynamic ? state.robot_dynamic_status->current_experience() : 0;
    const uint32_t exp_next = has_dynamic ? state.robot_dynamic_status->experience_for_upgrade() : 0;
    const int self_id = has_static ? static_cast<int>(state.robot_static_status->robot_id() % 100) : 0;

    const float pad = 16.0f * s;
    const float inner_w = size.x - pad * 2.0f;
    const float right = origin.x + size.x - pad;

    // 身份行
    const float head_y = origin.y + 14.0f * s;
    draw_list->AddRectFilled(ImVec2(origin.x + pad, head_y + 1.0f * s),
                             ImVec2(origin.x + pad + 17.0f * s, head_y + 18.0f * s), red, 4.0f * s);
    draw_list->AddRectFilled(ImVec2(origin.x + pad + 5.5f * s, head_y + 7.5f * s),
                             ImVec2(origin.x + pad + 11.5f * s, head_y + 13.5f * s),
                             IM_COL32(255, 255, 255, 235), 1.0f * s);
    std::string self_name = "未知机器人";
    if (self_id >= 1 && self_id <= 9) {
        self_name = RoboGo::app::robot_registry::kRobotTypes[self_id - 1].standalone;
    } else if (self_id == 0) {
        self_name = "本车";
    }
    AddText(draw_list, ImVec2(origin.x + pad + 25.0f * s, head_y + 3.0f * s), text, self_name.c_str(), 11.5f);

    char level_text[48];
    std::snprintf(level_text, sizeof(level_text), "Lv.%u · %u/%u", level, exp, exp_next);
    AddTextRightAligned(draw_list, right, head_y + 4.0f * s, dim, level_text, 9.5f);

    // 大血量数字
    char hp_text[32];
    std::snprintf(hp_text, sizeof(hp_text), "%u", current_health);
    char hp_max_text[32];
    std::snprintf(hp_max_text, sizeof(hp_max_text), "/ %u", max_health);
    const float hp_y = origin.y + 38.0f * s;
    const ImVec2 hp_size = TextSize(hp_text, 32);
    AddText(draw_list, ImVec2(origin.x + pad, hp_y), text, hp_text, 32);
    AddText(draw_list, ImVec2(origin.x + pad + hp_size.x + 7.0f * s, hp_y + 19.0f * s),
            dim, hp_max_text, 12);

    // 血条
    const float bar_y = origin.y + 78.0f * s;
    draw_list->AddRectFilled(ImVec2(origin.x + pad, bar_y), ImVec2(right, bar_y + 5.0f * s), slot, 3.0f * s);
    const float hp_ratio = RoboGo::ui::RatioOf(current_health, max_health);
    if (hp_ratio > 0.0f) {
        draw_list->AddRectFilled(ImVec2(origin.x + pad, bar_y),
                                 ImVec2(origin.x + pad + inner_w * hp_ratio, bar_y + 5.0f * s),
                                 red, 3.0f * s);
    }

    // 三栏指标：热量 / 弹量 / 缓冲能量
    const float metric_gap = 10.0f * s;
    const float metric_w = (inner_w - metric_gap * 2.0f) / 3.0f;
    const float metric_top = origin.y + 92.0f * s;
    char heat_text[32];
    std::snprintf(heat_text, sizeof(heat_text), "%.0f / %u", current_heat, max_heat);
    char ammo_text[32];
    std::snprintf(ammo_text, sizeof(ammo_text), "%u", ammo);
    char buffer_text[32];
    std::snprintf(buffer_text, sizeof(buffer_text), "%u / %u", current_buffer, max_buffer);

    struct Metric {
        const char *label;
        const char *value;
        float ratio;
        bool show_track;
    };
    const Metric metrics[] = {
            {"热量", heat_text, RoboGo::ui::RatioOf(static_cast<uint32_t>(current_heat), max_heat), true},
            {"弹量", ammo_text, 0.0f, false},
            {"缓冲能量", buffer_text, RoboGo::ui::RatioOf(current_buffer, max_buffer), true},
    };
    for (size_t i = 0; i < std::size(metrics); ++i) {
        const float x = origin.x + pad + static_cast<float>(i) * (metric_w + metric_gap);
        AddText(draw_list, ImVec2(x, metric_top), dim, metrics[i].label, 9.5f);
        AddText(draw_list, ImVec2(x, metric_top + 15.0f * s), text2, metrics[i].value, 12.5f);
        if (metrics[i].show_track) {
            const float track_y = metric_top + 34.0f * s;
            draw_list->AddRectFilled(ImVec2(x, track_y), ImVec2(x + metric_w, track_y + 2.0f * s),
                                     slot, 1.0f * s);
            if (metrics[i].ratio > 0.0f) {
                draw_list->AddRectFilled(ImVec2(x, track_y),
                                         ImVec2(x + metric_w * metrics[i].ratio, track_y + 2.0f * s),
                                         WithAlpha(0xFFFFFFFFu, 130), 1.0f * s);
            }
        }
    }

    // 底部第一行：模式开关 + 42mm 装配权限
    const float line1_y = origin.y + 136.0f * s;
    char mode_text[64];
    std::snprintf(mode_text, sizeof(mode_text), "自瞄 %s · 小陀螺 %s",
                  state.auto_aim ? "ON" : "OFF", state.gyro ? "ON" : "OFF");
    AddText(draw_list, ImVec2(origin.x + pad, line1_y), dim, mode_text, 9.5f);
    const char *perm_text = "42mm 未知";
    if (state.robot_module_status.has_value()) {
        perm_text = state.robot_module_status->modular_42mm() == 1 ? "42mm 已获得" : "42mm 未获得";
    }
    AddTextRightAligned(draw_list, right, line1_y, dim, perm_text, 9.5f);

    // 底部第二行：设备连接状态（有离线项时标红）
    const float line2_y = origin.y + 150.0f * s;
    bool has_offline = false;
    const std::string offline = CollectOfflineModules(state, has_offline);
    const std::string device_line = "设备: " + FitLine(offline, inner_w - 40.0f * s, 9.5f);
    AddText(draw_list, ImVec2(origin.x + pad, line2_y),
            has_offline ? WithAlpha(red, 220) : dim, device_line.c_str(), 9.5f);

    ImGui::End();
}
