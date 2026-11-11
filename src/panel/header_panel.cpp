// header_panel.cpp：主 HUD 顶部区（状态条 + 双方队伍条）与判罚/事件浮层的实现。
//
// 每帧绘制四块互不重叠的内容：
// 1) ShowHeaderPanel：居中状态胶囊（比分 / 倒计时 / 阶段 / 金币 / 自瞄·小陀螺）
// + 左右各 300px 的队伍条（基地、前哨、2×2 单位块）；
// 2) ShowPenaltyBanner：判罚横幅（按类型配色 + 本地倒计时）；
// 3) ShowEventToasts：最近重要事件的堆叠提示（6 秒淡出、最多 6 条）；
// 4) GetPenaltyTypeName：判罚类型码 → 中文，供日志/横幅复用。
// 数据流：只读 State。状态胶囊取 GameStatus / GlobalLogisticsStatus；
// 队伍条取 GlobalUnitStatus + RobotStaticStatus + State::red_robots/blue_robots；
// 浮层取 State::penalty_info / event_toasts（都由 MQTT handler 写入）。
// 设计要点：
// · 全部 ImDrawList 自绘且窗口带 NoInputs，不与下方本车卡、日志面板抢交互；
// · 只显示 1/3/4/7 四台有血量的单位，2027 协议里 2/6/8/9 无血量故不占位；
// · 敌方栏做整体镜像（队头靠右、建筑标签靠右、数值靠左），保证两侧视觉对称；
// · 己方/对方建筑血量按 WeAreRed 换算到红/蓝槽位，前哨站字段本身是绝对红蓝，不参与交换；
// · 浮层用 steady_clock 算本地剩余时间，不受系统时间调整影响，也不依赖服务器持续推送。
// 依赖：ImGui（imgui_internal.h 提供 GetScale）、messages.pb.h、ui/theme.h、
// ui/hud_primitives.h、ui/format_utils.h、app/match_display.h、app/robot_registry.h、
// app/team_util.h。
// 注意：本文件不接收任何输入，所有按钮态都来自 State 里已解析好的字段。
// 2027-11-06 按「主界面定稿」实装：
// · 顶部由横跨全屏的 9 条细血条，改为居中状态胶囊 + 左右各 300px 的队伍条；
// · 队伍条为「基地 / 前哨 + 2×2 大数值单位块」，只显示 1/3/4/7 四台有血量的单位；
// · 2 保留位、6 空中、8 飞镖、9 雷达在 2027 协议里没有血量，不再占位；
// · 兵种名、弹药行、角标等重复/无用文字全部去掉。
// 全部使用 ImDrawList 直接绘制（不接收交互），由主窗口在每帧开头调用。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
#include <messages.pb.h>
// [/pch-split]
#include "header_panel.h"

#include <cstdio>
#include <string>

#include "app/match_display.h"
#include "app/robot_registry.h"
#include "app/team_util.h"
#include "ui/format_utils.h"
#include "ui/hud_primitives.h"
#include "ui/theme.h"

namespace {

using RoboGo::ui::WithAlpha;
using RoboGo::ui::hud::AddText;
using RoboGo::ui::hud::AddTextCenteredIn;
using RoboGo::ui::hud::AddTextRightAligned;
using RoboGo::ui::hud::Colors;
using RoboGo::ui::hud::TextSize;

// 定稿只显示四台有血量的单位，编号与兵种名一一对应。
// 用户要求去掉纯数字编号、直接写清兵种：1 重装 / 3 步兵3 / 4 步兵4 / 7 哨兵。
constexpr std::array<int, 4> kHudRobotIds = {1, 3, 4, 7};
constexpr std::array<const char *, 4> kHudRobotLabels = {"重装", "步兵3", "步兵4", "哨兵"};
static_assert(kHudRobotIds.size() == kHudRobotLabels.size());

constexpr uint32_t kOutpostMaxHealth = 1000;
constexpr uint32_t kBaseMaxHealth = 10000;

/**
 * @brief 取主 HUD 统一色板（转发 hud_primitives.h 的 MakeColors）。
 * @return Colors 值类型，供本文件所有 Draw* 共享，避免各处重复取色。
 */
Colors MakeHudColors() {
    return RoboGo::ui::hud::MakeColors();
}

// 比赛阶段文案：技术暂停优先；结算阶段带结果码时拼成「结算 · 红方胜利」。
// 思路：优先级为「未连接 → 技术暂停中 → StageName(阶段)」；结算阶段(stage==5)且带
//       game_result 时追加「· 红方胜利/蓝方胜利/平局」，未知结果码则只显示阶段名。
// 参数：state —— 只读全局状态。
// 返回值：可直接交给 AddText 的 std::string。
std::string ResolveStageText(const State &state) {
    if (!state.game_status.has_value()) {
        return "未连接";
    }
    const GameStatus &status = *state.game_status;
    if (status.is_paused()) {
        return "技术暂停中";
    }
    const char *base = RoboGo::app::match_display::StageName(status.current_stage());
    if (status.current_stage() == 5 && status.has_game_result()) {
        const char *result = nullptr;
        switch (status.game_result()) {
            case 0: result = "平局"; break;
            case 1: result = "红方胜利"; break;
            case 2: result = "蓝方胜利"; break;
            default: break;
        }
        if (result != nullptr) {
            return std::string(base) + " · " + result;
        }
    }
    return base;
}

// 顶部胶囊：红方比分 / 倒计时 + 阶段 / 蓝方比分 / 金币 / 自瞄与小陀螺。
// 全部是裁判系统真实字段，没有装饰性假数据。
// 思路：按 52/142/52/92/132 五段划分宽度，依次是红方比分、倒计时+阶段、蓝方比分、
//       金币、自瞄/小陀螺两个状态 chip；段间用 1px 竖线分隔。所有数值缺数据时按 0，
//       阶段 0 强制把倒计时归零，避免显示上一局残留时间。
// 参数：viewport_size —— 主视口尺寸，用于水平居中；colors —— 统一色板。
void DrawTopBar(const State &state, ImDrawList *draw_list, const ImVec2 &viewport_size,
                const Colors &colors) {
    const float s = ImGui::GetScale();
    const float bar_w = 470.0f * s;
    const float bar_h = 48.0f * s;
    const float left = (viewport_size.x - bar_w) * 0.5f;
    const float top = 18.0f * s;
    const float bottom = top + bar_h;

    draw_list->AddRectFilled(ImVec2(left, top), ImVec2(left + bar_w, bottom), colors.surface, 13.0f * s);
    draw_list->AddRect(ImVec2(left, top), ImVec2(left + bar_w, bottom), colors.border, 13.0f * s, 0.5f);

    uint32_t red_score = 0;
    uint32_t blue_score = 0;
    int32_t countdown = 0;
    if (state.game_status.has_value()) {
        red_score = state.game_status->red_score();
        blue_score = state.game_status->blue_score();
        countdown = state.game_status->stage_countdown_sec();
        if (state.game_status->current_stage() == 0) {
            countdown = 0;
        }
    }
    const uint32_t gold = state.global_logistics_status.has_value()
                                  ? state.global_logistics_status->remaining_economy()
                                  : 0;

    char clock_text[16];
    RoboGo::ui::FormatClockTo(clock_text, countdown);
    const std::string stage_text = ResolveStageText(state);
    const std::string red_text = std::to_string(red_score);
    const std::string blue_text = std::to_string(blue_score);
    const std::string gold_text = std::to_string(gold);

    // 五段的横向边界：52 / 142 / 52 / 92 / 132（合计 470）。
    const float seg1 = left + 52.0f * s;
    const float seg2 = seg1 + 142.0f * s;
    const float seg3 = seg2 + 52.0f * s;
    const float seg4 = seg3 + 92.0f * s;
    const auto separator = [&](float x) {
        draw_list->AddLine(ImVec2(x, top + 10.0f * s), ImVec2(x, bottom - 10.0f * s), colors.border, 1.0f);
    };
    const float middle_y = top + (bar_h - TextSize("0", 20).y) * 0.5f;

    // 红方比分
    draw_list->AddCircleFilled(ImVec2(left + 14.0f * s, top + bar_h * 0.5f), 3.0f * s, colors.red);
    AddText(draw_list, ImVec2(left + 25.0f * s, middle_y), colors.text, red_text.c_str(), 20);
    separator(seg1);

    // 倒计时 + 阶段
    AddTextCenteredIn(draw_list, seg1, seg2, top + 7.0f * s, colors.text, clock_text, 21);
    AddTextCenteredIn(draw_list, seg1, seg2, top + 29.0f * s, colors.dim, stage_text.c_str(), 9.5f);
    separator(seg2);

    // 蓝方比分
    const ImVec2 blue_size = TextSize(blue_text.c_str(), 20);
    AddText(draw_list, ImVec2(seg2 + 8.0f * s, middle_y), colors.text, blue_text.c_str(), 20);
    draw_list->AddCircleFilled(ImVec2(seg2 + 8.0f * s + blue_size.x + 10.0f * s, top + bar_h * 0.5f),
                               3.0f * s, colors.blue);
    separator(seg3);

    // 金币
    const float econ_y = top + (bar_h - TextSize(gold_text.c_str(), 15).y - 12.0f * s) * 0.5f;
    AddText(draw_list, ImVec2(seg3 + 12.0f * s, econ_y), colors.gold, gold_text.c_str(), 15);
    AddText(draw_list, ImVec2(seg3 + 12.0f * s, econ_y + 15.0f * s), colors.dim, "金币", 9.5f);
    separator(seg4);

    // 自瞄 / 小陀螺
    const float chip_y = top + (bar_h - 25.0f * s) * 0.5f;
    const float chip_w = 52.0f * s;
    const auto draw_chip = [&](float x, const char *label, bool on) {
        draw_list->AddRectFilled(ImVec2(x, chip_y), ImVec2(x + chip_w, chip_y + 25.0f * s),
                                 on ? WithAlpha(colors.green, 24) : colors.chip, 7.0f * s);
        draw_list->AddRect(ImVec2(x, chip_y), ImVec2(x + chip_w, chip_y + 25.0f * s),
                           on ? WithAlpha(colors.green, 70) : colors.border, 7.0f * s, 0.5f);
        draw_list->AddCircleFilled(ImVec2(x + 9.0f * s, chip_y + 12.5f * s), 2.5f * s,
                                   on ? colors.green : colors.dim);
        const ImVec2 text_size = TextSize(label, 10);
        AddText(draw_list, ImVec2(x + 16.0f * s, chip_y + (25.0f * s - text_size.y) * 0.5f),
                on ? colors.text2 : colors.dim, label, 10);
    };
    draw_chip(seg4 + 9.0f * s, "自瞄", state.auto_aim);
    draw_chip(seg4 + 9.0f * s + chip_w + 5.0f * s, "小陀螺", state.gyro);
}

// 前哨站 / 基地血量，已按我方阵营换算到红/蓝槽位。
// base_health / enemy_base_* 是「己方 / 对方」相对语义，必须按阵营交换；
// outpost_R_* / outpost_B_* 是绝对红/蓝前哨，不能交换。
// 思路：把「相对语义」的协议字段在读取时一次性转成绝对红蓝，后续绘制不再判断阵营。
struct BuildingHealth {
    uint32_t red_outpost = 0;
    uint32_t red_base = 0;
    uint32_t blue_outpost = 0;
    uint32_t blue_base = 0;
};

/**
 * @brief 从 GlobalUnitStatus 读出四个建筑血量并按己方阵营归位。
 * @param we_are_red 由 team_util::WeAreRed(state) 得到。
 * @note 思路：base_health / enemy_base_health 是「己方/对方」相对语义，必须按 we_are_red 交换；outpost_r_health / outpost_b_health
 *        是绝对红蓝前哨，直接取值即可。缺少 GlobalUnitStatus 或 RobotStaticStatus 时返回全 0（界面显示 0 而不崩）。
 * @return BuildingHealth 值类型。
 */
BuildingHealth ReadBuildingHealth(const State &state, bool we_are_red) {
    BuildingHealth health;
    if (!state.global_unit_status.has_value() || !state.robot_static_status.has_value()) {
        return health;
    }
    const GlobalUnitStatus &unit = *state.global_unit_status;
    if (we_are_red) {
        health.red_base = unit.base_health();
        health.blue_base = unit.enemy_base_health();
    } else {
        health.red_base = unit.enemy_base_health();
        health.blue_base = unit.base_health();
    }
    health.red_outpost = unit.outpost_r_health();
    health.blue_outpost = unit.outpost_b_health();
    return health;
}

// 一侧队伍条：队头 + 基地/前哨两行 + 2×2 大数值单位块。
// mirror=true 时整体镜像（右侧蓝方：队头靠右、建筑标签靠右、数值靠左）。
// 思路：mirror=true 为右侧蓝方做整体镜像；单位块遍历 kHudRobotIds(1/3/4/7)，
//       从 robots[id-1] 取血量。本车用红色描边+顶部亮线，阵亡弱化，低血(<40%)琥珀，
//       底部细血条按 RatioOf 填充。存活计数同样用 health>0 判定。
// 参数：left/top/width —— 该栏矩形；self_id/we_are_red —— 本车编号与己方阵营。
void DrawTeamBlock(const State &state, const BuildingHealth &health, ImDrawList *draw_list,
                   const Colors &colors, float left, float top, float width,
                   bool red_side, bool mirror, int self_id, bool we_are_red) {
    const float s = ImGui::GetScale();
    const ImU32 team_color = red_side ? colors.red : colors.blue;
    const RobotStatus *robots = red_side ? state.red_robots : state.blue_robots;
    const char *team_name = red_side ? "红方" : "蓝方";

    int alive_count = 0;
    for (int id : kHudRobotIds) {
        if (robots[id - 1].health > 0) {
            ++alive_count;
        }
    }
    char alive_text[32];
    std::snprintf(alive_text, sizeof(alive_text), "%d / 4 存活", alive_count);

    // 队头
    const float head_h = 18.0f * s;
    const ImVec2 name_size = TextSize(team_name, 11);
    if (!mirror) {
        draw_list->AddRectFilled(ImVec2(left, top + 7.0f * s), ImVec2(left + 6.0f * s, top + 13.0f * s),
                                 team_color, 2.0f * s);
        AddText(draw_list, ImVec2(left + 13.0f * s, top + (head_h - name_size.y) * 0.5f),
                colors.text2, team_name, 11);
        AddTextRightAligned(draw_list, left + width, top + 2.0f * s, colors.dim, alive_text, 10);
    } else {
        AddText(draw_list, ImVec2(left, top + 2.0f * s), colors.dim, alive_text, 10);
        const float dot_x = left + width - 6.0f * s;
        draw_list->AddRectFilled(ImVec2(dot_x, top + 7.0f * s), ImVec2(dot_x + 6.0f * s, top + 13.0f * s),
                                 team_color, 2.0f * s);
        AddTextRightAligned(draw_list, dot_x - 7.0f * s, top + (head_h - name_size.y) * 0.5f,
                            colors.text2, team_name, 11);
    }

    // 基地 / 前哨：细条 + 数值，镜像侧左右对调。
    const float row_h = 22.0f * s;
    const float rows_top = top + head_h + 8.0f * s;
    const auto draw_building_row = [&](float y, const char *label, uint32_t value, uint32_t max_value) {
        const float label_w = 30.0f * s;
        const float val_w = 48.0f * s;
        const float gap = 9.0f * s;
        float label_x;
        float val_x;
        float bar_left;
        float bar_right;
        if (!mirror) {
            label_x = left;
            val_x = left + width - val_w;
            bar_left = left + label_w + gap;
            bar_right = left + width - val_w - gap;
        } else {
            label_x = left + width - label_w;
            val_x = left;
            bar_left = left + val_w + gap;
            bar_right = left + width - label_w - gap;
        }
        const ImVec2 label_size = TextSize(label, 11);
        AddText(draw_list, ImVec2(label_x + (label_w - label_size.x) * 0.5f, y + (row_h - label_size.y) * 0.5f),
                colors.text2, label, 11);

        char value_text[32];
        std::snprintf(value_text, sizeof(value_text), "%u", value);
        const ImVec2 value_size = TextSize(value_text, 13);
        AddText(draw_list, ImVec2(val_x + (val_w - value_size.x) * 0.5f, y + (row_h - value_size.y) * 0.5f),
                colors.text2, value_text, 13);

        const float bar_y = y + (row_h - 3.0f * s) * 0.5f;
        draw_list->AddRectFilled(ImVec2(bar_left, bar_y), ImVec2(bar_right, bar_y + 3.0f * s),
                                 colors.slot, 2.0f * s);
        const float ratio = RoboGo::ui::RatioOf(value, max_value);
        if (ratio > 0.0f) {
            if (red_side) {
                draw_list->AddRectFilled(ImVec2(bar_left, bar_y),
                                         ImVec2(bar_left + (bar_right - bar_left) * ratio, bar_y + 3.0f * s),
                                         team_color, 2.0f * s);
            } else {
                draw_list->AddRectFilled(ImVec2(bar_right - (bar_right - bar_left) * ratio, bar_y),
                                         ImVec2(bar_right, bar_y + 3.0f * s),
                                         team_color, 2.0f * s);
            }
        }
    };
    draw_building_row(rows_top, "基地", red_side ? health.red_base : health.blue_base, kBaseMaxHealth);
    draw_building_row(rows_top + row_h, "前哨", red_side ? health.red_outpost : health.blue_outpost,
                      kOutpostMaxHealth);

    const float divider_y = rows_top + 2.0f * row_h + 9.0f * s;
    draw_list->AddLine(ImVec2(left, divider_y), ImVec2(left + width, divider_y), colors.border, 1.0f);

    // 2×2 单位块：形状 = 单位身份，描边 = 阵营，顶部亮线 = 本车，琥珀 = 低血，弱化 = 阵亡。
    const float grid_top = divider_y + 9.0f * s;
    const float gap = 8.0f * s;
    const float block_w = (width - gap) * 0.5f;
    const float block_h = 52.0f * s;
    for (size_t i = 0; i < kHudRobotIds.size(); ++i) {
        const int id = kHudRobotIds[i];
        const RobotStatus &robot = robots[id - 1];
        const float bx = left + static_cast<float>(i % 2) * (block_w + gap);
        const float by = grid_top + static_cast<float>(i / 2) * (block_h + gap);
        const bool mine = (red_side == we_are_red) && (id == self_id);
        const bool dead = robot.max_health == 0 || robot.health == 0;
        const float ratio = RoboGo::ui::RatioOf(robot.health, robot.max_health);
        const bool low = !dead && ratio < 0.4f;

        ImU32 background = colors.chip;
        ImU32 border = colors.border;
        if (mine) {
            background = IM_COL32(45, 22, 26, 140);
            border = WithAlpha(colors.red, 130);
        }
        if (dead) {
            background = WithAlpha(0xFFFFFFFFu, 5);
        }
        draw_list->AddRectFilled(ImVec2(bx, by), ImVec2(bx + block_w, by + block_h), background, 9.0f * s);
        draw_list->AddRect(ImVec2(bx, by), ImVec2(bx + block_w, by + block_h), border, 9.0f * s, 0.5f);
        if (mine) {
            draw_list->AddRectFilled(ImVec2(bx + 10.0f * s, by - 1.0f * s),
                                     ImVec2(bx + block_w - 10.0f * s, by + 1.0f * s), colors.red, 2.0f * s);
        }

        // 编号（很淡的角标）+ 兵种名（主标签）：一眼能认出是哪台，也不丢协议编号。
        char id_text[8];
        std::snprintf(id_text, sizeof(id_text), "%d", id);
        const ImVec2 id_size = TextSize(id_text, 9.5f);
        AddText(draw_list, ImVec2(bx + 10.0f * s, by + 7.0f * s),
                WithAlpha(colors.dim, 170), id_text, 9.5f);
        AddText(draw_list, ImVec2(bx + 10.0f * s + id_size.x + 5.0f * s, by + 6.0f * s),
                dead ? WithAlpha(colors.dim, 140) : colors.text2, kHudRobotLabels[i], 10.5f);

        char hp_text[24];
        if (dead) {
            std::snprintf(hp_text, sizeof(hp_text), "阵亡");
        } else {
            std::snprintf(hp_text, sizeof(hp_text), "%u", robot.health);
        }
        const float hp_font = dead ? 12.0f : 17.0f;
        const ImU32 hp_color = dead ? WithAlpha(colors.dim, 150)
                                    : (low ? colors.amber : colors.text);
        AddTextRightAligned(draw_list, bx + block_w - 10.0f * s, by + 5.0f * s, hp_color, hp_text, hp_font);

        const float track_left = bx + 10.0f * s;
        const float track_right = bx + block_w - 10.0f * s;
        const float track_y = by + block_h - 10.0f * s;
        draw_list->AddRectFilled(ImVec2(track_left, track_y), ImVec2(track_right, track_y + 3.0f * s),
                                 colors.slot, 2.0f * s);
        if (!dead && ratio > 0.0f) {
            draw_list->AddRectFilled(ImVec2(track_left, track_y),
                                     ImVec2(track_left + (track_right - track_left) * ratio, track_y + 3.0f * s),
                                     low ? colors.amber : team_color, 2.0f * s);
        }
    }
}

}  // namespace

/**
 * @brief 绘制主 HUD 顶部区（状态胶囊 + 左右队伍条）。
 * @param state 只读全局状态。
 * @note 思路：整窗覆盖全视口但不接收输入，所有元素按定稿的绝对坐标自绘，避免与下方日志/本车卡抢布局。
 * @note 思路：开一个覆盖全视口且带 NoInputs 的透明窗口，全部用 ImDrawList 按绝对坐标自绘，不与下方日志面板/本车卡抢布局与交互；队伍条与胶囊同为 top=18*s，
 *        视觉上连成一条带。
 */
void ShowHeaderPanel(const State &state) {
    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration
                                          | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove
                                          | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    const ImVec2 viewport_size = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport_size, ImGuiCond_Always);
    if (!ImGui::Begin("HeaderWindow", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const float s = ImGui::GetScale();
    const Colors colors = MakeHudColors();
    const bool we_are_red = RoboGo::app::team_util::WeAreRed(state);
    const int self_id = state.robot_static_status.has_value()
                                ? static_cast<int>(state.robot_static_status->robot_id() % 100)
                                : 0;
    const BuildingHealth health = ReadBuildingHealth(state, we_are_red);

    DrawTopBar(state, draw_list, viewport_size, colors);

    const float side = 24.0f * s;
    const float team_w = 300.0f * s;
    // 用户评审：左右上角原本空了一整条，把队伍条提到与顶部胶囊齐平，
    // 队头 + 基地/前哨正好填满胶囊两侧，视觉上形成一条完整信息带。
    const float team_top = 18.0f * s;
    DrawTeamBlock(state, health, draw_list, colors, side, team_top, team_w,
                  true, false, self_id, we_are_red);
    DrawTeamBlock(state, health, draw_list, colors, viewport_size.x - side - team_w, team_top, team_w,
                  false, true, self_id, we_are_red);

    ImGui::End();
}

/**
 * @brief 把判罚类型码翻译成中文（2027 协议 2.2.15 PenaltyInfo.penalty_type）。
 * @note 思路：1~6 为已知类型，其余返回 nullptr 让调用方拼「未知判罚(N)」，不在这里吞掉新类型，便于赛场发现协议扩展。
 * @return 静态字符串或 nullptr。
 */
const char *GetPenaltyTypeName(uint32_t penalty_type) {
    switch (penalty_type) {
        case 1: return "黄牌";
        case 2: return "双方黄牌";
        case 3: return "红牌";
        case 4: return "超功率";
        case 5: return "超热量";
        case 6: return "超射速";
        default: return nullptr;
    }
}

/**
 * @brief 绘制判罚横幅——顶部居中的一条无交互提示，显示判罚类型与本地倒计时。
 * @note 思路：剩余秒数 = effect_sec - (now - penalty_received_time)，用 steady_clock 计算，不依赖服务器持续推送；<=0 或
 *        effect_sec==0 时直接不画。类型 3(红牌) 用红底白字，1/2(黄牌) 用黄底黑字，其余橙底白字。文案由 ImFormatStringToTempBuffer 生成，
 *        避免每帧分配 std::string。
 */
void ShowPenaltyBanner(const State &state) {
    if (!state.penalty_info.has_value()) {
        return;
    }
    const uint32_t penalty_type = state.penalty_info->penalty_type();
    const uint32_t effect_sec = state.penalty_info->penalty_effect_sec();
    if (effect_sec == 0) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - state.penalty_received_time);
    const long long remain_sec = static_cast<long long>(effect_sec) - static_cast<long long>(elapsed.count());
    if (remain_sec <= 0) {
        return;
    }

    const char *penalty_name = GetPenaltyTypeName(penalty_type);
    const bool is_known = penalty_name != nullptr;
    ImU32 background_color;
    ImU32 text_color;
    if (penalty_type == 3) {
        background_color = IM_COL32(200, 30, 30, 235);
        text_color = IM_COL32(255, 255, 255, 255);
    } else if (penalty_type == 1 || penalty_type == 2) {
        background_color = IM_COL32(235, 190, 40, 235);
        text_color = IM_COL32(30, 30, 30, 255);
    } else {
        background_color = IM_COL32(230, 120, 30, 235);
        text_color = IM_COL32(255, 255, 255, 255);
    }

    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration
                                          | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove
                                          | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    const ImVec2 viewport_size = ImGui::GetMainViewport()->Size;
    const float max_banner_width = 420.0f * ImGui::GetScale();
    const float banner_width = viewport_size.x * 0.28f < max_banner_width
                                       ? viewport_size.x * 0.28f
                                       : max_banner_width;
    const float banner_height = 34.0f * ImGui::GetScale();
    ImGui::SetNextWindowPos(ImVec2((viewport_size.x - banner_width) * 0.5f,
                                   viewport_size.y * 0.15f + 6.0f * ImGui::GetScale()),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(banner_width, banner_height), ImGuiCond_Always);
    if (!ImGui::Begin("PenaltyBannerWindow", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const ImVec2 window_max(window_pos.x + window_size.x, window_pos.y + window_size.y);
    const float rounding = 8.0f * ImGui::GetScale();
    draw_list->AddRectFilled(window_pos, window_max, background_color, rounding);
    draw_list->AddRect(window_pos, window_max, IM_COL32(255, 255, 255, 90), rounding, 1.0f);

    const char *text_begin, *text_end;
    if (is_known) {
        ImFormatStringToTempBuffer(&text_begin, &text_end, "%s   剩余 %lld 秒", penalty_name, remain_sec);
    } else {
        ImFormatStringToTempBuffer(&text_begin, &text_end, "未知判罚(%u)   剩余 %lld 秒",
                                   penalty_type, remain_sec);
    }
    RoboGo::ui::AddTextCentered(draw_list, text_begin, text_end, window_pos.x, window_pos.y,
                                window_max.x, window_max.y, text_color);
    ImGui::End();
}

/**
 * @brief 绘制顶部事件提示层——把最近的重要 Event 以短暂浮层的形式堆叠显示。
 * @note 思路：倒序遍历 event_toasts，跳过超过 6 秒的旧项；3.5 秒后按剩余时间线性淡出；最多显示 6 条，按 level 取背景色并把 alpha 同时应用到背景与文字。
 *        窗口高度按可见条数预分配，但内层仍按实际 slot 绘制，避免过期项留下空洞。
 */
void ShowEventToasts(const State &state) {
    if (state.event_toasts.empty()) {
        return;
    }
    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration
                                          | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove
                                          | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    const ImVec2 viewport_size = ImGui::GetMainViewport()->Size;
    const float toast_height = 26.0f * ImGui::GetScale();
    const float toast_gap = 4.0f * ImGui::GetScale();
    const float max_toast_width = viewport_size.x * 0.32f;
    const float area_top = viewport_size.y * 0.15f + 46.0f * ImGui::GetScale();
    const int max_visible = 6;
    const int visible_count = static_cast<int>(state.event_toasts.size()) > max_visible
                                      ? max_visible
                                      : static_cast<int>(state.event_toasts.size());
    const float area_height = static_cast<float>(visible_count) * (toast_height + toast_gap);
    ImGui::SetNextWindowPos(ImVec2((viewport_size.x - max_toast_width) * 0.5f, area_top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(max_toast_width, area_height), ImGuiCond_Always);
    if (!ImGui::Begin("EventToastWindow", nullptr, window_flags)) {
        ImGui::End();
        return;
    }
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const auto now = std::chrono::steady_clock::now();
    const float rounding = 6.0f * ImGui::GetScale();

    ImGui::PushFont(ImGui::GetFont(), 15);
    int slot = 0;
    for (auto it = state.event_toasts.rbegin(); it != state.event_toasts.rend() && slot < max_visible; ++it) {
        const long long age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->time).count();
        if (age_ms > 6000) {
            continue;
        }
        float alpha = 1.0f;
        if (age_ms > 3500) {
            alpha = 1.0f - static_cast<float>(age_ms - 3500) / 2500.0f;
            if (alpha < 0.0f) {
                alpha = 0.0f;
            }
        }
        const ImVec2 text_size = ImGui::CalcTextSize(it->text.c_str());
        const float padding = 10.0f * ImGui::GetScale();
        const float box_width = text_size.x + padding * 2 < max_toast_width
                                        ? text_size.x + padding * 2
                                        : max_toast_width;
        const float box_left = window_pos.x + (max_toast_width - box_width) * 0.5f;
        const float box_top = window_pos.y + static_cast<float>(slot) * (toast_height + toast_gap);
        ImU32 background_color = IM_COL32(30, 30, 30, 200);
        ImU32 text_color = IM_COL32(255, 255, 255, 255);
        if (it->level == 1) {
            background_color = IM_COL32(200, 110, 20, 215);
        } else if (it->level >= 2) {
            background_color = IM_COL32(190, 40, 40, 225);
        }
        const ImU32 faded_alpha = (static_cast<ImU32>(255.0f * alpha) & 0xFFu) << 24;
        background_color = (background_color & 0x00FFFFFFu) | faded_alpha;
        text_color = (text_color & 0x00FFFFFFu) | faded_alpha;
        draw_list->AddRectFilled(ImVec2(box_left, box_top),
                                 ImVec2(box_left + box_width, box_top + toast_height),
                                 background_color, rounding);
        draw_list->AddText(ImVec2(box_left + padding, box_top + (toast_height - text_size.y) / 2),
                           text_color, it->text.c_str());
        ++slot;
    }
    ImGui::PopFont();
    ImGui::End();
}
