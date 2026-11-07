// commander_panel.cpp：指挥手全屏综合指挥台的实现。
//
// 2027-11-06 按「综合指挥台视觉设计稿 v1」实装：沿用主 HUD 定稿的视觉令牌
// （深色中性底 / 14px 圆角 / 低对比描边 / 等宽数字 / 6 级字阶），
// 把三个带灰底的方块子窗口换成透明三栏，一条连续顶栏贯穿全宽。
//   · 双方阵容：宽卡（在线 / 弹量 / 百分比三连）→ 2×2 大数值单位块 + 兵种名；
//   · 建筑：46px 卡片 → 22px 细行；
//   · 四张 KPI 方卡 → 一行指标卡 + 双色控制度带；
//   · 事件流：带标题灰面板 → 透明无标题行；
//   · 指令区：高饱和实心按钮 → 分层级幽灵按钮；
//   · 战术地图：绘制与交互完全沿用项目原设计，未改动。
// 所有字段都取裁判系统真实数据；指令下发、页签与地图打点回调全部保留。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
#include <messages.pb.h>
// [/pch-split]
#include "commander_panel.h"

#include "keyboard_mouse_state_manager.h"   // 指挥台退出、地图打点、指挥手角色查询
#include "logging/logging.h"                // SPDLOG_INFO/WARN 日志宏
#include "io/command_sender.h"              // 指令下发统一出口
#include "ui/theme.h"
#include "ui/hud_primitives.h"               // 与主 HUD 共用的色板与文字绘制
#include "ui/format_utils.h"                 // 比例 / mm:ss / 居中文本
#include "app/robot_registry.h"              // 集中机器人 ID / 槽位枚举常量
#include "app/match_display.h"               // 阶段码 -> 中文（唯一来源）
#include "app/team_util.h"                   // 共享阵营/雷达工具：WeAreRed / RadarRobotIds

#include <cstdio>
#include <functional>
#include <string>

using RoboGo::app::match_display::StageName;
using RoboGo::ui::FormatClock;
using RoboGo::ui::RatioOf;
using RoboGo::app::team_util::IsRedRobotId;
using RoboGo::app::team_util::WeAreRed;
using RoboGo::app::team_util::RadarRobotIds;

namespace {

namespace hud = RoboGo::ui::hud;
using RoboGo::ui::WithAlpha;

using hud::AddText;
using hud::AddTextCenteredIn;
using hud::AddTextRightAligned;
using hud::Colors;
using hud::TextSize;

// ---------------- 配色 ----------------
// 绝大多数颜色与「主界面定稿」同源；只补设计稿里独有的几项。
constexpr ImU32 kBg = RoboGo::ui::kSurface0;
constexpr ImU32 kLine = WithAlpha(0xFFFFFFFFu, 12);       // 单位块 / 按钮的低对比描边
constexpr ImU32 kSlot = WithAlpha(0xFFFFFFFFu, 18);       // 血条底槽
constexpr ImU32 kPurple = IM_COL32(160, 143, 214, 255);   // 页签选中 / 哨兵指令
constexpr ImU32 kMineBg = IM_COL32(45, 22, 26, 190);      // 本车单位块底色
constexpr ImU32 kScanLine = WithAlpha(0xFFFFFFFFu, 2);    // 背景极淡扫描线

Colors MakeColors() {
    return hud::MakeColors();
}

ImVec4 Col(ImU32 color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

ImU32 Alpha(ImU32 color, int alpha) {
    return (color & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24);
}

// ---------------- 布局常量（设计稿 1280×800 基准，随 DPI 缩放） ----------------
constexpr float kPad = 24.0f;
constexpr float kTopY = 18.0f;
constexpr float kTopH = 48.0f;
constexpr float kColTop = 82.0f;
constexpr float kColBottom = 24.0f;
constexpr float kSideW = 300.0f;
constexpr float kColGap = 12.0f;
constexpr float kSectH = 26.0f;
constexpr float kUnitH = 52.0f;
constexpr float kUnitGap = 8.0f;
constexpr float kBlockGap = 14.0f;
constexpr float kBuildH = 22.0f;
constexpr float kKvH = 20.0f;
constexpr float kCardRadius = 14.0f;

struct Layout {
    ImVec2 origin;
    ImVec2 size;
    float pad;
    float top_y;
    float top_h;
    float col_y;
    float col_h;
    float left_x;
    float left_w;
    float mid_x;
    float mid_w;
    float right_x;
    float right_w;
};

Layout ComputeLayout(const ImVec2 &viewport_pos, const ImVec2 &viewport_size, float s) {
    const float pad = kPad * s;
    const float left_w = kSideW * s;
    const float right_w = kSideW * s;
    const float gap = kColGap * s;
    const float mid_w = std::max(viewport_size.x - 2.0f * pad - 2.0f * gap - left_w - right_w,
                                 240.0f * s);
    const float col_y = kColTop * s;
    return Layout{
            viewport_pos, viewport_size, pad,
            kTopY * s, kTopH * s,
            col_y, std::max(viewport_size.y - col_y - kColBottom * s, 320.0f * s),
            pad, left_w,
            pad + left_w + gap, mid_w,
            pad + left_w + gap + mid_w + gap, right_w,
    };
}

ImVec2 At(const Layout &layout, float x, float y) {
    return ImVec2(layout.origin.x + x, layout.origin.y + y);
}

// ---------------- 绘制原语 ----------------
void Card(ImDrawList *dl, ImVec2 p, ImVec2 q, const Colors &c, float radius, ImU32 fill) {
    dl->AddRectFilled(p, q, fill, radius);
    dl->AddRect(p, q, c.border, radius, 0.5f);
}

// 分区小标题：无彩色竖条，右侧是灰色的计数/说明。
void SectionLabel(ImDrawList *dl, ImVec2 p, float width, const Colors &c,
                  const char *title, const char *right) {
    AddText(dl, p, c.text2, title, 10.5f);
    if (right != nullptr && right[0] != '\0') {
        AddTextRightAligned(dl, p.x + width, p.y + 1.0f, c.dim, right, 9.5f);
    }
}

// 单位块：兵种名（主）+ 17px 血量（右）+ 底部血条；本车高亮、低血琥珀、阵亡弱化。
struct UnitVisual {
    const char *label;
    uint32_t hp;
    uint32_t max_hp;
    bool known;
    bool mine;
};

ImU32 UnitAccent(const UnitVisual &unit, const Colors &c, ImU32 team_color) {
    if (unit.known && unit.hp == 0) return WithAlpha(c.dim, 170);
    if (unit.known && RatioOf(unit.hp, unit.max_hp) < 0.4f) return c.amber;
    return team_color;
}

void UnitBlock(ImDrawList *dl, ImVec2 p, float width, const UnitVisual &unit,
               const Colors &c, ImU32 team_color, float s) {
    const ImVec2 q(p.x + width, p.y + kUnitH * s);
    const bool dead = unit.known && unit.hp == 0;
    const bool low = unit.known && !dead && RatioOf(unit.hp, unit.max_hp) < 0.4f;
    const ImU32 accent = UnitAccent(unit, c, team_color);

    Card(dl, p, q, c, 9.0f * s, unit.mine ? kMineBg : WithAlpha(0xFFFFFFFFu, 7));
    if (unit.mine) {
        dl->AddRectFilled(ImVec2(p.x + 10.0f * s, p.y - 1.0f),
                          ImVec2(q.x - 10.0f * s, p.y + 1.0f), team_color, 2.0f * s);
    }
    AddText(dl, ImVec2(p.x + 10.0f * s, p.y + 7.0f * s),
            dead ? c.dim : c.text2, unit.label, 9.5f);

    char hp_text[24];
    if (!unit.known) {
        std::snprintf(hp_text, sizeof(hp_text), "—");
    } else if (dead) {
        std::snprintf(hp_text, sizeof(hp_text), "阵亡");
    } else {
        std::snprintf(hp_text, sizeof(hp_text), "%u", unit.hp);
    }
    AddTextRightAligned(dl, q.x - 10.0f * s, p.y + 6.0f * s,
                        dead ? c.dim : (low ? c.amber : c.text), hp_text, 17);

    const ImVec2 bar_min(p.x + 10.0f * s, q.y - 9.0f * s);
    const ImVec2 bar_max(q.x - 10.0f * s, q.y - 6.0f * s);
    dl->AddRectFilled(bar_min, bar_max, kSlot, 2.0f * s);
    if (unit.known && unit.hp > 0) {
        const float fill = (bar_max.x - bar_min.x) * RatioOf(unit.hp, unit.max_hp);
        dl->AddRectFilled(bar_min, ImVec2(bar_min.x + fill, bar_max.y), accent, 2.0f * s);
    }
}

// 建筑细行：标签 / 血条 / 数值；mirror=true 时数值在左、标签在右（敌方栏镜像）。
struct BuildingLine {
    const char *label;
    uint32_t hp;
    uint32_t max_hp;
};

void BuildingRow(ImDrawList *dl, ImVec2 p, float width, const BuildingLine &line,
                 const Colors &c, ImU32 team_color, bool mirror, float s) {
    const float cy = p.y + kBuildH * s * 0.5f;
    const float lb_w = 30.0f * s;
    const float val_w = 48.0f * s;
    const float bar_x0 = p.x + (mirror ? val_w + 9.0f * s : lb_w + 9.0f * s);
    const float bar_x1 = p.x + width - (mirror ? lb_w + 9.0f * s : val_w + 9.0f * s);

    char value[24];
    std::snprintf(value, sizeof(value), "%u", line.hp);
    if (mirror) {
        AddText(dl, ImVec2(p.x, cy - 7.0f * s), c.text2, value, 12);
        AddTextRightAligned(dl, p.x + width, cy - 6.0f * s, c.dim, line.label, 10.5f);
    } else {
        AddText(dl, ImVec2(p.x, cy - 6.0f * s), c.dim, line.label, 10.5f);
        AddTextRightAligned(dl, p.x + width, cy - 7.0f * s, c.text2, value, 12);
    }

    dl->AddRectFilled(ImVec2(bar_x0, cy - 1.5f * s), ImVec2(bar_x1, cy + 1.5f * s), kSlot, 2.0f * s);
    if (line.hp > 0) {
        const float fill = (bar_x1 - bar_x0) * RatioOf(line.hp, line.max_hp);
        if (mirror) {
            dl->AddRectFilled(ImVec2(bar_x1 - fill, cy - 1.5f * s), ImVec2(bar_x1, cy + 1.5f * s),
                              team_color, 2.0f * s);
        } else {
            dl->AddRectFilled(ImVec2(bar_x0, cy - 1.5f * s), ImVec2(bar_x0 + fill, cy + 1.5f * s),
                              team_color, 2.0f * s);
        }
    }
}

// 柔性徽标：淡色底 + 低对比描边。
float SoftBadge(ImDrawList *dl, ImVec2 p, const char *text, ImU32 color, const Colors &c, float s) {
    const ImVec2 size = TextSize(text, 9);
    const float w = size.x + 12.0f * s;
    const ImVec2 q(p.x + w, p.y + 16.0f * s);
    dl->AddRectFilled(p, q, Alpha(color, 22), 4.0f * s);
    dl->AddRect(p, q, Alpha(color, 55), 4.0f * s, 0.5f);
    AddText(dl, ImVec2(p.x + 6.0f * s, p.y + 2.5f * s), Alpha(color, 235), text, 9);
    return w;
}

// 紧凑键值行：标签左、数值右。
void KvRow(ImDrawList *dl, ImVec2 p, float width, const Colors &c,
           const char *label, const char *value, float s, ImU32 value_color = 0) {
    AddText(dl, ImVec2(p.x, p.y + 4.0f * s), c.dim, label, 10);
    AddTextRightAligned(dl, p.x + width, p.y + 3.0f * s,
                        value_color == 0 ? c.text2 : value_color, value, 10.5f);
}

// ---------------- 数据快照 ----------------
struct UnitHp {
    uint32_t hp = 0;
    uint32_t max_hp = 0;
    bool known = false;
};

UnitHp GetUnitHp(const State &state, uint32_t robot_id) {
    const int type = static_cast<int>(robot_id % 100);
    const int index = (type >= 0 && type < 9) ? RoboGo::app::robot_registry::kRobotTypeToSlot[type] : -1;
    if (index < 0) {
        return {};
    }
    const RobotStatus &robot = IsRedRobotId(robot_id) ? state.red_robots[index] : state.blue_robots[index];
    UnitHp result;
    result.hp = robot.health;
    result.max_hp = robot.max_health;
    result.known = robot.max_health > 0;
    return result;
}

struct RobotSlotDef {
    int array_index;
    int id;
    const char *label;
};

// 与主 HUD 的 kHudRobotLabels 一致：兵种名是主标签，编号只在角标里保留。
constexpr RobotSlotDef kRobotSlots[4] = {
        {0, 1, "1 重装"},
        {2, 3, "3 步兵3"},
        {3, 4, "4 步兵4"},
        {6, 7, "7 哨兵"},
};

struct BuildingInfo {
    const char *label;
    uint32_t hp;
    uint32_t max_hp;
    const char *control;
    ImU32 control_color;
    const char *extra;
};

BuildingInfo MakeOutpostInfo(const char *label, uint32_t hp, uint32_t status, ImU32 ours, ImU32 theirs) {
    BuildingInfo info{label, hp, 1000, "未知", RoboGo::ui::kTextSecondary, ""};
    switch (status) {
        case 1: info.control = "我方完全"; info.control_color = ours; break;
        case 2: info.control = "我方临时"; info.control_color = ours; break;
        case 3: info.control = "敌方临时"; info.control_color = theirs; break;
        case 4: info.control = "敌方完全"; info.control_color = theirs; break;
        default: break;
    }
    return info;
}

BuildingInfo MakeBaseInfo(const char *label, uint32_t hp, uint32_t shield, bool own_base,
                          uint32_t base_status, ImU32 ours, ImU32 theirs) {
    BuildingInfo info{label, hp, 10000, "未知", RoboGo::ui::kTextSecondary, ""};
    if (own_base) {
        if (shield == 1) { info.control = "我方完全"; info.control_color = ours; }
        else if (shield == 2) { info.control = "敌方临时"; info.control_color = theirs; }
    } else {
        if (shield == 1) { info.control = "敌方完全"; info.control_color = theirs; }
        else if (shield == 2) { info.control = "我方临时"; info.control_color = ours; }
    }
    switch (base_status) {
        case 0: info.extra = "100% 防御增益"; break;
        case 1: info.extra = "护甲未展开"; break;
        case 2: info.extra = "护甲展开"; break;
        default: info.extra = ""; break;
    }
    return info;
}

std::array<BuildingInfo, 2> OurBuildings(const State &state, bool we_red, ImU32 ours, ImU32 theirs) {
    const GlobalUnitStatus *unit = state.global_unit_status.has_value() ? &*state.global_unit_status : nullptr;
    if (unit == nullptr) {
        return {BuildingInfo{"基地", 0, 10000, "未知", RoboGo::ui::kTextSecondary, ""},
                BuildingInfo{"前哨", 0, 1000, "未知", RoboGo::ui::kTextSecondary, ""}};
    }
    BuildingInfo base = MakeBaseInfo("基地", unit->base_health(), unit->base_shield(), true,
                                     unit->base_status(), ours, theirs);
    BuildingInfo outpost = we_red
            ? MakeOutpostInfo("前哨", unit->outpost_r_health(), unit->outpost_r_status(), ours, theirs)
            : MakeOutpostInfo("前哨", unit->outpost_b_health(), unit->outpost_b_status(), ours, theirs);
    return {base, outpost};
}

std::array<BuildingInfo, 2> EnemyBuildings(const State &state, bool we_red, ImU32 ours, ImU32 theirs) {
    const GlobalUnitStatus *unit = state.global_unit_status.has_value() ? &*state.global_unit_status : nullptr;
    if (unit == nullptr) {
        return {BuildingInfo{"基地", 0, 10000, "未知", RoboGo::ui::kTextSecondary, ""},
                BuildingInfo{"前哨", 0, 1000, "未知", RoboGo::ui::kTextSecondary, ""}};
    }
    BuildingInfo base = MakeBaseInfo("基地", unit->enemy_base_health(), unit->enemy_base_shield(), false,
                                     unit->enemy_base_status(), ours, theirs);
    BuildingInfo outpost = we_red
            ? MakeOutpostInfo("前哨", unit->outpost_b_health(), unit->outpost_b_status(), ours, theirs)
            : MakeOutpostInfo("前哨", unit->outpost_r_health(), unit->outpost_r_status(), ours, theirs);
    return {base, outpost};
}

// ---------------- 按钮 ----------------
enum class ButtonKind { Ghost, Primary, Danger, TabOn, TabOff };

bool DeckButton(const char *label, ImVec2 p, ImVec2 size, ButtonKind kind,
                bool enabled = true, const char *tooltip = nullptr) {
    namespace ui = RoboGo::ui;
    ImU32 fill = WithAlpha(0xFFFFFFFFu, 7);
    ImU32 border = kLine;
    ImU32 text = ui::kTextSecondary;
    ImU32 hover = WithAlpha(0xFFFFFFFFu, 34);
    ImU32 active = WithAlpha(0xFFFFFFFFu, 55);
    switch (kind) {
        case ButtonKind::Primary:
            fill = Alpha(ui::kAccent, 45); border = Alpha(ui::kAccent, 90); text = ui::Tint(ui::kAccent, 0.3f, 0.45f);
            hover = Alpha(ui::kAccent, 80); active = Alpha(ui::kAccent, 120); break;
        case ButtonKind::Danger:
            fill = Alpha(ui::kTeamRed, 33); border = Alpha(ui::kTeamRed, 78); text = ui::Tint(ui::kTeamRed, 0.3f, 0.5f);
            hover = Alpha(ui::kTeamRed, 65); active = Alpha(ui::kTeamRed, 105); break;
        case ButtonKind::TabOn:
            fill = Alpha(kPurple, 40); border = Alpha(kPurple, 72); text = ui::Tint(kPurple, 0.3f, 0.4f);
            hover = Alpha(kPurple, 78); active = Alpha(kPurple, 110); break;
        case ButtonKind::TabOff:
            fill = WithAlpha(0xFFFFFFFFu, 4); border = kLine; text = ui::kTextTertiary;
            hover = WithAlpha(0xFFFFFFFFu, 22); active = WithAlpha(0xFFFFFFFFu, 40); break;
        default: break;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, Col(fill));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(active));
    ImGui::PushStyleColor(ImGuiCol_Text, Col(text));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f * ImGui::GetScale());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Border, Col(border));
    ImGui::SetCursorScreenPos(p);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, size);
    ImGui::EndDisabled();
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                                   ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    return clicked;
}

// 网格按钮：grid 为列数，col_span 允许最后一行合并列。
void GridButton(const char *label, ImVec2 origin, float width, float row_h, float gap,
                int columns, int row, int col, int col_span, ButtonKind kind,
                bool enabled, const char *tooltip,
                const std::function<void()> &on_click) {
    const float cell_w = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float w = cell_w * static_cast<float>(col_span) + gap * static_cast<float>(col_span - 1);
    const ImVec2 p(origin.x + (cell_w + gap) * static_cast<float>(col),
                  origin.y + (row_h + gap) * static_cast<float>(row));
    if (DeckButton(label, p, ImVec2(w, row_h), kind, enabled, tooltip)) {
        on_click();
    }
}

// ---------------- 顶栏 ----------------
void DrawTopBar(const State &state, RoboGo::io::MqttClient &mqtt_client,
                const Layout &layout, ImDrawList *dl, const Colors &c, float s) {
    const ImVec2 p = At(layout, layout.pad, layout.top_y);
    const ImVec2 q(p.x + layout.size.x - 2.0f * layout.pad, p.y + layout.top_h);
    Card(dl, p, q, c, 13.0f * s, c.surface);

    // 左：身份
    const float badge_x = p.x + 14.0f * s;
    dl->AddRectFilled(ImVec2(badge_x, p.y + 14.0f * s), ImVec2(badge_x + 20.0f * s, p.y + 34.0f * s),
                      c.red, 5.0f * s);
    dl->AddRectFilled(ImVec2(badge_x + 6.5f * s, p.y + 20.5f * s),
                      ImVec2(badge_x + 13.5f * s, p.y + 27.5f * s), WithAlpha(0xFFFFFFFFu, 235), 1.0f * s);
    AddText(dl, ImVec2(badge_x + 29.0f * s, p.y + 9.0f * s), c.text, "综合指挥台", 12);
    const uint32_t round = state.game_status.has_value() ? state.game_status->current_round() : 0;
    const uint32_t total_rounds = state.game_status.has_value() ? state.game_status->total_rounds() : 0;
    const uint32_t self_robot_id = state.robot_static_status.has_value() ? state.robot_static_status->robot_id() : 0;
    char identity[64];
    if (is_commander_role()) {
        std::snprintf(identity, sizeof(identity), "指挥手");
    } else if (self_robot_id != 0) {
        std::snprintf(identity, sizeof(identity), "ID %u", self_robot_id);
    } else {
        std::snprintf(identity, sizeof(identity), "未知身份");
    }
    char subtitle[96];
    std::snprintf(subtitle, sizeof(subtitle), "%s · %s · 第 %u/%u 局", identity,
                  WeAreRed(state) ? "红方" : "蓝方", round, total_rounds);
    AddText(dl, ImVec2(badge_x + 29.0f * s, p.y + 27.0f * s), c.dim, subtitle, 9.5f);

    // 中：红比分 / 时钟+阶段 / 蓝比分
    const uint32_t red_score = state.game_status.has_value() ? state.game_status->red_score() : 0;
    const uint32_t blue_score = state.game_status.has_value() ? state.game_status->blue_score() : 0;
    const int32_t countdown = state.game_status.has_value() ? state.game_status->stage_countdown_sec() : 0;
    const char *stage = state.game_status.has_value() ? StageName(state.game_status->current_stage()) : "等待数据";
    char clock[16];
    RoboGo::ui::FormatClockTo(clock, countdown);
    const float cx = p.x + (q.x - p.x) * 0.5f;
    const float clock_w = 108.0f * s;
    const auto sep = [&](float x) {
        dl->AddLine(ImVec2(x, p.y + 10.0f * s), ImVec2(x, q.y - 10.0f * s), c.border, 1.0f);
    };
    const float left_sep = cx - clock_w * 0.5f - 58.0f * s;
    const float right_sep = cx + clock_w * 0.5f + 58.0f * s;
    sep(left_sep);
    sep(right_sep);

    dl->AddCircleFilled(ImVec2(left_sep - 30.0f * s, p.y + 24.0f * s), 3.0f * s, c.red);
    const std::string red_text = std::to_string(red_score);
    AddText(dl, ImVec2(left_sep - 22.0f * s, p.y + 12.0f * s), c.text, red_text.c_str(), 19);
    AddTextCenteredIn(dl, cx - clock_w * 0.5f, cx + clock_w * 0.5f, p.y + 7.0f * s, c.text, clock, 20);
    AddTextCenteredIn(dl, cx - clock_w * 0.5f, cx + clock_w * 0.5f, p.y + 30.0f * s, c.dim, stage, 9.5f);
    const std::string blue_text = std::to_string(blue_score);
    AddText(dl, ImVec2(right_sep + 14.0f * s, p.y + 12.0f * s), c.text, blue_text.c_str(), 19);
    dl->AddCircleFilled(ImVec2(right_sep + 44.0f * s, p.y + 24.0f * s), 3.0f * s, c.blue);

    // 右：经济 / 链路 / 返回
    const uint32_t gold = state.global_logistics_status.has_value()
                                  ? state.global_logistics_status->remaining_economy() : 0;
    char gold_text[24];
    std::snprintf(gold_text, sizeof(gold_text), "%u", gold);
    const float return_w = 56.0f * s;
    const float return_x = q.x - 14.0f * s - return_w;
    const bool connected = mqtt_client.get_connection_state() == RoboGo::io::MqttClient::ConnectionState::Connected;
    const char *link_text = connected ? "裁判系统已连接" : "裁判系统离线";
    const float link_w = TextSize(link_text, 10).x + 14.0f * s;
    const float link_x = return_x - 16.0f * s - link_w;
    dl->AddCircleFilled(ImVec2(link_x + 3.0f * s, p.y + 24.0f * s), 2.5f * s, connected ? c.green : c.red);
    AddText(dl, ImVec2(link_x + 11.0f * s, p.y + 18.0f * s), c.text2, link_text, 10);
    const float econ_w = TextSize(gold_text, 14).x + TextSize("剩余经济", 9.5f).x + 10.0f * s;
    const float econ_x = link_x - 20.0f * s - econ_w;
    AddText(dl, ImVec2(econ_x, p.y + 15.0f * s), c.gold, gold_text, 14);
    AddText(dl, ImVec2(econ_x + TextSize(gold_text, 14).x + 5.0f * s, p.y + 20.0f * s),
            c.dim, "剩余经济", 9.5f);
    dl->AddLine(ImVec2(econ_x - 12.0f * s, p.y + 10.0f * s),
                ImVec2(econ_x - 12.0f * s, q.y - 10.0f * s), c.border, 1.0f);

    const bool commander_exit = is_commander_role();
    const char *exit_tooltip = commander_exit
            ? "指挥手没有上一层：退出指挥台并退出登录，回到身份选择页。"
            : "返回上一层，保持登录连接（要退出登录请回开局面板的「登录控制」）。";
    if (DeckButton("返回", ImVec2(return_x, p.y + 10.0f * s),
                   ImVec2(return_w, 28.0f * s), ButtonKind::Ghost, true, exit_tooltip)) {
        if (commander_exit) {
            SPDLOG_INFO("退出指挥台并退出登录（指挥手）client_id={}", mqtt_client.get_client_id());
            set_commander_role(false);
            mqtt_client.set_client_id("");
        } else {
            exit_commander_console();
        }
    }
}

// ---------------- 左栏：我方阵容 / 建筑 / 资源 / 现场画面 ----------------
void DrawRoster(const State &state, ImDrawList *dl, ImVec2 p, float width,
                bool ally, const Colors &c, ImU32 team_color, float s) {
    const bool we_red = WeAreRed(state);
    int alive = 0;
    for (const RobotSlotDef &slot : kRobotSlots) {
        const uint32_t robot_id = (ally == we_red) ? static_cast<uint32_t>(slot.id)
                                                   : static_cast<uint32_t>(slot.id + 100);
        const UnitHp hp = GetUnitHp(state, robot_id);
        if (!hp.known || hp.hp > 0) ++alive;
    }
    char right[32];
    std::snprintf(right, sizeof(right), "%d / 4 在线", alive);
    SectionLabel(dl, p, width, c, ally ? "我方阵容" : "敌方阵容", right);

    const float block_w = (width - kUnitGap * s) * 0.5f;
    const float top = p.y + kSectH * s;
    const uint32_t self_robot_id = state.robot_static_status.has_value() ? state.robot_static_status->robot_id() : 0;
    for (int i = 0; i < 4; ++i) {
        const RobotSlotDef &slot = kRobotSlots[i];
        const uint32_t robot_id = (ally == we_red) ? static_cast<uint32_t>(slot.id)
                                                   : static_cast<uint32_t>(slot.id + 100);
        const UnitHp hp = GetUnitHp(state, robot_id);
        const ImVec2 bp(p.x + static_cast<float>(i % 2) * (block_w + kUnitGap * s),
                        top + static_cast<float>(i / 2) * (kUnitH + kUnitGap) * s);
        UnitVisual unit{slot.label, hp.hp, hp.max_hp, hp.known,
                        ally && robot_id == self_robot_id};
        UnitBlock(dl, bp, block_w, unit, c, team_color, s);
    }
}

float DrawBuildings(ImDrawList *dl, ImVec2 p, float width,
                    const std::array<BuildingInfo, 2> &buildings,
                    const Colors &c, ImU32 team_color, bool mirror, float s) {
    SectionLabel(dl, p, width, c, mirror ? "敌方建筑" : "我方建筑", "阵地状态");
    float y = p.y + kSectH * s;
    for (const BuildingInfo &info : buildings) {
        BuildingRow(dl, ImVec2(p.x, y), width, BuildingLine{info.label, info.hp, info.max_hp},
                    c, team_color, mirror, s);
        y += kBuildH * s;
    }
    // 控制状态与基地护甲，两个软徽标。
    float bx = mirror ? p.x + width : p.x;
    for (const BuildingInfo &info : buildings) {
        if (info.control[0] == '\0') continue;
        const float w = SoftBadge(dl, ImVec2(mirror ? bx - TextSize(info.control, 9).x - 12.0f * s : bx,
                                             y + 5.0f * s),
                                  info.control, info.control_color, c, s);
        if (info.extra[0] != '\0') {
            float extra_x = mirror ? bx - TextSize(info.control, 9).x - 12.0f * s
                                            - TextSize(info.extra, 9).x - 18.0f * s
                                   : bx + w + 6.0f * s;
            SoftBadge(dl, ImVec2(extra_x, y + 5.0f * s), info.extra, c.green, c, s);
        }
        break;   // 只展示基地的阵地状态，避免两行徽标拥挤
    }
    return y + 25.0f * s;
}

void DrawResources(const State &state, ImDrawList *dl, ImVec2 p, float width,
                   const Colors &c, float s) {
    SectionLabel(dl, p, width, c, "资源与机制", "实时");
    const uint32_t gold = state.global_logistics_status.has_value()
                                  ? state.global_logistics_status->remaining_economy() : 0;
    const uint32_t tech = state.global_logistics_status.has_value()
                                  ? state.global_logistics_status->tech_level() : 0;
    const uint32_t encryption = state.global_logistics_status.has_value()
                                        ? state.global_logistics_status->encryption_level() : 0;
    char line1[96];
    std::snprintf(line1, sizeof(line1), "%u 经济   %u 科技   %u 加密", gold, tech, encryption);
    AddText(dl, ImVec2(p.x, p.y + kSectH * s + 3.0f * s), c.text2, line1, 10.5f);

    const char *fort = "无占领计时";
    if (state.global_special_mechanism.has_value()) {
        const auto &mechanism = *state.global_special_mechanism;
        for (int i = 0; i < mechanism.mechanism_id_size(); ++i) {
            const uint32_t id = mechanism.mechanism_id(i);
            const int32_t seconds = i < mechanism.mechanism_time_sec_size() ? mechanism.mechanism_time_sec(i) : 0;
            if (id == 1 || id == 2) {
                fort = id == 1 ? "己方堡垒被占" : "对方堡垒被占";
                (void) seconds;
            }
        }
    } else {
        fort = "等待数据";
    }
    char line2[96];
    std::snprintf(line2, sizeof(line2), "堡垒 %s   能量机关 %s", fort,
                  state.rune_status_sync.has_value() ? "已同步" : "未激活");
    AddText(dl, ImVec2(p.x, p.y + (kSectH + kKvH) * s + 3.0f * s), c.dim, line2, 10);
}

// 现场画面：有图传时等比贴纹理；无图传时保留深色配色 + 取景环 + 四角框。
void DrawVideoPreview(ImTextureRef texture, const ImVec2 &texture_size, bool ready,
                      ImDrawList *dl, ImVec2 p, float width, float height,
                      const Colors &c, float s) {
    const ImVec2 q(p.x + width, p.y + height);
    dl->AddRectFilled(p, q, IM_COL32(13, 18, 26, 255), kCardRadius * s);
    dl->AddRect(p, q, kLine, kCardRadius * s, 0.5f);

    const bool has_frame = ready && texture.GetTexID() != ImTextureID_Invalid &&
                           texture_size.x > 0.0f && texture_size.y > 0.0f;
    if (has_frame) {
        ImVec2 uv0(0.0f, 0.0f);
        ImVec2 uv1(1.0f, 1.0f);
        const float target_ratio = width / height;
        const float source_ratio = texture_size.x / texture_size.y;
        if (source_ratio > target_ratio) {
            const float visible = target_ratio / source_ratio;
            uv0.x = (1.0f - visible) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        } else {
            const float visible = source_ratio / target_ratio;
            uv0.y = (1.0f - visible) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        }
        dl->AddImage(texture, p, q, uv0, uv1);
    } else {
        const float arm = 18.0f * s;
        const ImU32 corner = WithAlpha(0xFFFFFFFFu, 24);
        dl->AddLine(ImVec2(p.x + 14.0f * s, p.y + 14.0f * s + arm), ImVec2(p.x + 14.0f * s, p.y + 14.0f * s), corner, 1.0f);
        dl->AddLine(ImVec2(p.x + 14.0f * s, p.y + 14.0f * s), ImVec2(p.x + 14.0f * s + arm, p.y + 14.0f * s), corner, 1.0f);
        dl->AddLine(ImVec2(q.x - 14.0f * s - arm, p.y + 14.0f * s), ImVec2(q.x - 14.0f * s, p.y + 14.0f * s), corner, 1.0f);
        dl->AddLine(ImVec2(q.x - 14.0f * s, p.y + 14.0f * s), ImVec2(q.x - 14.0f * s, p.y + 14.0f * s + arm), corner, 1.0f);
        dl->AddLine(ImVec2(p.x + 14.0f * s, q.y - 14.0f * s - arm), ImVec2(p.x + 14.0f * s, q.y - 14.0f * s), corner, 1.0f);
        dl->AddLine(ImVec2(p.x + 14.0f * s, q.y - 14.0f * s), ImVec2(p.x + 14.0f * s + arm, q.y - 14.0f * s), corner, 1.0f);
        dl->AddLine(ImVec2(q.x - 14.0f * s - arm, q.y - 14.0f * s), ImVec2(q.x - 14.0f * s, q.y - 14.0f * s), corner, 1.0f);
        dl->AddLine(ImVec2(q.x - 14.0f * s, q.y - 14.0f * s - arm), ImVec2(q.x - 14.0f * s, q.y - 14.0f * s), corner, 1.0f);

        const ImVec2 center(p.x + width * 0.5f, p.y + height * 0.44f);
        const float r = 26.0f * s;
        dl->AddCircle(center, r, WithAlpha(0xFFFFFFFFu, 28), 0, 2.0f * s);
        dl->AddLine(ImVec2(center.x, center.y - r - 8.0f * s), ImVec2(center.x, center.y - r - 1.0f * s),
                    WithAlpha(0xFFFFFFFFu, 70), 1.0f);
        dl->AddLine(ImVec2(center.x, center.y + r + 1.0f * s), ImVec2(center.x, center.y + r + 8.0f * s),
                    WithAlpha(0xFFFFFFFFu, 70), 1.0f);
        AddTextCenteredIn(dl, p.x, q.x, center.y + r + 16.0f * s, c.dim, "暂无图传画面", 11);
    }
    AddText(dl, ImVec2(p.x + 16.0f * s, q.y - 22.0f * s), c.dim,
            has_frame ? "UDP 图传 · 已连接" : "UDP 图传 · 无信号", 9);
}

void DrawLeftColumn(const State &state, ImTextureRef video_texture, const ImVec2 &video_size,
                    bool video_ready, const Layout &layout, ImDrawList *dl,
                    const Colors &c, ImU32 ours, ImU32 theirs, float s) {
    const ImVec2 base = At(layout, layout.left_x, layout.col_y);
    const float w = layout.left_w;
    float y = 0.0f;

    DrawRoster(state, dl, ImVec2(base.x, base.y + y), w, true, c, ours, s);
    y += (kSectH + kUnitH * 2.0f + kUnitGap + kBlockGap) * s;

    const float next = DrawBuildings(dl, ImVec2(base.x, base.y + y), w,
                                     OurBuildings(state, WeAreRed(state), ours, theirs),
                                     c, ours, false, s);
    y = next - base.y + kBlockGap * s;

    DrawResources(state, dl, ImVec2(base.x, base.y + y), w, c, s);
    y += (kSectH + kKvH * 2.0f + kBlockGap) * s;

    SectionLabel(dl, ImVec2(base.x, base.y + y), w, c, "现场画面", "UDP 图传");
    y += kSectH * s;
    const float preview_h = std::max(base.y + layout.col_h - (base.y + y) - 4.0f * s, 170.0f * s);
    DrawVideoPreview(video_texture, video_size, video_ready, dl,
                     ImVec2(base.x, base.y + y), w, preview_h, c, s);
}

// ---------------- 中栏：KPI / 事件 / 关键机制 ----------------
void DrawKpi(const State &state, ImDrawList *dl, ImVec2 p, float width, float height,
             const Colors &c, ImU32 ours, ImU32 theirs, float s) {
    const ImVec2 q(p.x + width, p.y + height);
    Card(dl, p, q, c, kCardRadius * s, c.surface);

    const auto &unit = state.global_unit_status;
    const uint32_t cp_ally = unit.has_value() ? unit->total_cp_ally() : 0;
    const uint32_t cp_enemy = unit.has_value() ? unit->total_cp_enemy() : 0;
    const uint32_t dmg_ally = unit.has_value() ? unit->total_damage_ally() : 0;
    const uint32_t dmg_enemy = unit.has_value() ? unit->total_damage_enemy() : 0;
    int ally_alive = 0;
    int enemy_alive = 0;
    for (const RobotSlotDef &slot : kRobotSlots) {
        const UnitHp ally = GetUnitHp(state, WeAreRed(state) ? slot.id : slot.id + 100);
        const UnitHp enemy = GetUnitHp(state, WeAreRed(state) ? slot.id + 100 : slot.id);
        if (!ally.known || ally.hp > 0) ++ally_alive;
        if (!enemy.known || enemy.hp > 0) ++enemy_alive;
    }
    const uint32_t gold = state.global_logistics_status.has_value()
                                  ? state.global_logistics_status->remaining_economy() : 0;

    char cp_text[48], dmg_text[48], alive_text[48], gold_text[48];
    std::snprintf(cp_text, sizeof(cp_text), "%u : %u", cp_ally, cp_enemy);
    std::snprintf(dmg_text, sizeof(dmg_text), "%u : %u", dmg_ally, dmg_enemy);
    std::snprintf(alive_text, sizeof(alive_text), "%d : %d", ally_alive, enemy_alive);
    std::snprintf(gold_text, sizeof(gold_text), "%u", gold);
    const char *keys[4] = {"控制度", "总伤害", "存活", "剩余经济"};
    const char *values[4] = {cp_text, dmg_text, alive_text, gold_text};

    const float cell_w = (width - 32.0f * s) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        const float x = p.x + 16.0f * s + cell_w * static_cast<float>(i);
        AddText(dl, ImVec2(x, p.y + 14.0f * s), c.dim, keys[i], 9.5f);
        AddText(dl, ImVec2(x, p.y + 31.0f * s),
                i == 3 ? c.gold : c.text, values[i], 16);
    }

    // 双色控制度带：我方从左、敌方从右，中间一条分界。
    const ImVec2 band_min(p.x + 16.0f * s, q.y - 24.0f * s);
    const ImVec2 band_max(q.x - 16.0f * s, q.y - 18.0f * s);
    dl->AddRectFilled(band_min, band_max, kSlot, 3.0f * s);
    const uint32_t total = cp_ally + cp_enemy;
    const float ratio = total > 0 ? static_cast<float>(cp_ally) / static_cast<float>(total) : 0.5f;
    const float split = band_min.x + (band_max.x - band_min.x) * ratio;
    if (split > band_min.x) {
        dl->AddRectFilled(band_min, ImVec2(split, band_max.y), ours, 3.0f * s, ImDrawFlags_RoundCornersLeft);
    }
    if (split < band_max.x) {
        dl->AddRectFilled(ImVec2(split, band_min.y), band_max, theirs, 3.0f * s, ImDrawFlags_RoundCornersRight);
    }
}

void DrawEvents(const State &state, ImDrawList *dl, ImVec2 p, float width, float height,
                const Colors &c, float s) {
    std::vector<const LogEntry *> entries;
    entries.reserve(state.system_logs.size() + state.vehicle_logs.size());
    for (const LogEntry &entry : state.system_logs) entries.push_back(&entry);
    for (const LogEntry &entry : state.vehicle_logs) entries.push_back(&entry);
    std::ranges::sort(entries, [](const LogEntry *a, const LogEntry *b) { return a->sequence > b->sequence; });

    if (entries.empty()) {
        AddText(dl, ImVec2(p.x, p.y + 6.0f * s), c.dim, "暂无事件", 10.5f);
        return;
    }
    const size_t count = std::min<size_t>(entries.size(), 8);
    const float row_h = 25.0f * s;
    const float max_y = p.y + height;
    for (size_t i = 0; i < count; ++i) {
        const float y = p.y + static_cast<float>(i) * row_h;
        if (y + row_h > max_y) break;
        const LogEntry &entry = *entries[i];
        const ImU32 mark = entry.type == 2 ? c.red : (entry.type == 1 ? c.amber : WithAlpha(0xFFFFFFFFu, 46));
        const ImU32 text_color = entry.type == 2 ? IM_COL32(232, 188, 188, 255)
                                : entry.type == 1 ? IM_COL32(220, 187, 150, 255) : c.text2;
        dl->AddRectFilled(ImVec2(p.x, y + 3.0f * s), ImVec2(p.x + 3.0f * s, y + 15.0f * s), mark, 2.0f * s);
        AddText(dl, ImVec2(p.x + 12.0f * s, y + 2.0f * s), c.dim, entry.time, 9.5f);
        char text[256];
        if (entry.repeat_count > 1) {
            std::snprintf(text, sizeof(text), "%s (×%u)", entry.text.c_str(), entry.repeat_count);
        } else {
            std::snprintf(text, sizeof(text), "%s", entry.text.c_str());
        }
        AddText(dl, ImVec2(p.x + 84.0f * s, y + 1.0f * s), text_color, text, 10.5f);
    }
}

void DrawMechanisms(const State &state, ImDrawList *dl, ImVec2 p, float width, float height,
                    const Colors &c, float s) {
    const ImVec2 q(p.x + width, p.y + height);
    Card(dl, p, q, c, kCardRadius * s, c.surface);
    AddText(dl, ImVec2(p.x + 14.0f * s, p.y + 12.0f * s), c.text2, "关键机制", 10.5f);

    char rune[48] = "未激活 · 0 环";
    if (state.rune_status_sync.has_value()) {
        const auto &v = *state.rune_status_sync;
        const char *status = v.rune_status() == 1 ? "未激活" : v.rune_status() == 2 ? "激活中" : "已激活";
        std::snprintf(rune, sizeof(rune), "%s · %.1f 环", status, static_cast<double>(v.average_rings()));
    }
    char fort[48] = "无占领计时";
    char enemy_fort[48] = "等待数据";
    if (state.global_special_mechanism.has_value()) {
        const auto &m = *state.global_special_mechanism;
        bool any = false;
        for (int i = 0; i < m.mechanism_id_size(); ++i) {
            const uint32_t id = m.mechanism_id(i);
            const int32_t sec = i < m.mechanism_time_sec_size() ? m.mechanism_time_sec(i) : 0;
            if (id == 1) { std::snprintf(fort, sizeof(fort), "%s", FormatClock(sec).c_str()); any = true; }
            if (id == 2) { std::snprintf(enemy_fort, sizeof(enemy_fort), "%s", FormatClock(sec).c_str()); any = true; }
        }
        if (!any) std::snprintf(fort, sizeof(fort), "无占领计时");
    }
    char dart[48] = "已关闭";
    if (state.dart_select_target_status_sync.has_value()) {
        const auto &d = *state.dart_select_target_status_sync;
        std::snprintf(dart, sizeof(dart), "%s",
                      d.open() == 2 ? "已开启" : d.open() == 1 ? "开启中" : "已关闭");
    }
    char sentry[48] = "等待同步";
    if (state.sentry_status_sync.has_value()) {
        const auto &v = *state.sentry_status_sync;
        std::snprintf(sentry, sizeof(sentry), "%s",
                      v.posture_id() == 1 ? "进攻" : v.posture_id() == 2 ? "防御"
                      : v.posture_id() == 3 ? "移动" : "未知");
    }
    char outpost[48] = "等待数据";
    char armor[48] = "等待数据";
    char tech[24] = "0";
    if (state.global_unit_status.has_value()) {
        const auto &u = *state.global_unit_status;
        const uint32_t status = WeAreRed(state) ? u.outpost_r_status() : u.outpost_b_status();
        std::snprintf(outpost, sizeof(outpost), "%s",
                      status == 1 ? "我方完全" : status == 2 ? "我方临时"
                      : status == 3 ? "敌方临时" : status == 4 ? "敌方完全" : "未知");
        std::snprintf(armor, sizeof(armor), "%s",
                      u.base_status() == 0 ? "100% 防御增益" : u.base_status() == 1 ? "未展开" : "已展开");
    }
    if (state.global_logistics_status.has_value()) {
        std::snprintf(tech, sizeof(tech), "%u", state.global_logistics_status->tech_level());
    }

    const char *labels[8] = {"能量机关", "堡垒", "飞镖闸门", "前哨站", "哨兵姿态", "护甲状态", "对方堡垒", "科技等级"};
    const char *values[8] = {rune, fort, dart, outpost, sentry, armor, enemy_fort, tech};
    const float row_h = 19.0f * s;
    const float top = p.y + 34.0f * s;
    for (int i = 0; i < 8; ++i) {
        const float y = top + static_cast<float>(i) * row_h;
        if (y + row_h > q.y - 30.0f * s) break;
        KvRow(dl, ImVec2(p.x + 14.0f * s, y), width - 28.0f * s, c, labels[i], values[i], s);
    }
    // 说明只在剩余高度够时画，避免小窗口里压住最后一行状态。
    const float note_y = top + 8.0f * row_h + 6.0f * s;
    if (note_y + 26.0f * s <= q.y - 6.0f * s) {
        AddText(dl, ImVec2(p.x + 14.0f * s, note_y), c.dim,
                "指挥手下发哨兵指令会覆盖自动决策；", 9);
        AddText(dl, ImVec2(p.x + 14.0f * s, note_y + 13.0f * s), c.dim,
                "每次干预扣除全队金币。", 9);
    }
}

// ---------------- 战术地图（项目原设计，绘制与交互完全保留） ----------------
constexpr float kMapWidthDm = 280.0f;
constexpr float kMapHeightDm = 150.0f;

struct MapAnchor {
    float x;
    float y;
    const char *label;
    bool red_side;
};
constexpr MapAnchor kBases[2] = {{22.0f, 27.0f, "红基地", true}, {256.0f, 122.0f, "蓝基地", false}};
constexpr MapAnchor kOutposts[2] = {{29.0f, 78.0f, "红前哨", true}, {249.0f, 76.0f, "蓝前哨", false}};

std::pair<ImVec2, ImVec2> MapUvRect(const ImVec2 &texture_size) {
    (void) texture_size;
    return {ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f)};
}

struct LocalMarker {
    bool valid = false;
    float x = 0.0f;
    float y = 0.0f;
    std::chrono::steady_clock::time_point time{};
};
LocalMarker g_local_marker;

void DrawBuildingMarker(ImDrawList *draw_list, ImVec2 center, const char *label, uint32_t hp, uint32_t max_hp,
                        ImU32 side_color, ImU32 control_color, bool has_control, float scale) {
    const float radius = 9.0f * scale;
    const ImU32 fill = Alpha(side_color, 46);
    const ImVec2 points[4] = {
            ImVec2(center.x, center.y - radius), ImVec2(center.x + radius, center.y),
            ImVec2(center.x, center.y + radius), ImVec2(center.x - radius, center.y),
    };
    draw_list->AddConvexPolyFilled(points, 4, fill);
    draw_list->AddPolyline(points, 4, side_color, 2.0f * scale, ImDrawFlags_Closed);
    if (has_control) draw_list->AddCircleFilled(center, 3.0f * scale, control_color);
    if (hp == 0) {
        draw_list->AddLine(ImVec2(center.x - radius * 0.6f, center.y - radius * 0.6f),
                           ImVec2(center.x + radius * 0.6f, center.y + radius * 0.6f),
                           RoboGo::ui::kTeamRed, 2.0f * scale);
        draw_list->AddLine(ImVec2(center.x - radius * 0.6f, center.y + radius * 0.6f),
                           ImVec2(center.x + radius * 0.6f, center.y - radius * 0.6f),
                           RoboGo::ui::kTeamRed, 2.0f * scale);
    }
    ImGui::PushFont(ImGui::GetFont(), 12);
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    draw_list->AddText(ImVec2(center.x - label_size.x * 0.5f, center.y - radius - label_size.y - 2.0f * scale),
                       side_color, label);
    char hp_text[16];
    std::snprintf(hp_text, sizeof(hp_text), "%d%%", static_cast<int>(RatioOf(hp, max_hp) * 100.0f + 0.5f));
    const ImVec2 hp_size = ImGui::CalcTextSize(hp_text);
    draw_list->AddText(ImVec2(center.x - hp_size.x * 0.5f, center.y + radius + 2.0f * scale),
                       RoboGo::ui::kTextSecondary, hp_text);
    ImGui::PopFont();
}

void ShowTacticalMap(const State &state, RoboGo::io::MqttClient &mqtt_client,
                     ImTextureRef texture, const ImVec2 &texture_size,
                     ImTextureRef terrain_texture, const ImVec2 &terrain_texture_size,
                     ImVec2 size) {
    const float scale = ImGui::GetScale();
    const bool we_red = WeAreRed(state);
    const ImU32 ours = we_red ? RoboGo::ui::kTeamRed : RoboGo::ui::kTeamBlue;
    const ImU32 theirs = we_red ? RoboGo::ui::kTeamBlue : RoboGo::ui::kTeamRed;

    ImGui::PushID("CommanderMap");
    ImGui::InvisibleButton("map_area", size);
    ImGui::PopID();

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const ImVec2 map_min = ImGui::GetItemRectMin();
    const ImVec2 map_max = ImGui::GetItemRectMax();
    const float map_width = size.x;
    const float map_height = size.y;

    draw_list->AddRectFilled(map_min, map_max, kBg, 6.0f * scale);
    if (texture.GetTexID() != ImTextureID_Invalid && texture_size.x > 0.0f && texture_size.y > 0.0f) {
        const auto [uv_min, uv_max] = MapUvRect(texture_size);
        draw_list->AddImage(texture, map_min, map_max, uv_min, uv_max);
        draw_list->AddRectFilled(map_min, map_max, IM_COL32(8, 16, 24, 96));
        if (is_terrain_overlay_enabled() && terrain_texture.GetTexID() != ImTextureID_Invalid
            && terrain_texture_size.x > 0.0f && terrain_texture_size.y > 0.0f) {
            draw_list->AddImage(terrain_texture, map_min, map_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                IM_COL32(255, 255, 255, 90));
        }
    }
    draw_list->AddRect(map_min, map_max, RoboGo::ui::kBorder, 6.0f * scale, 1.0f, ImDrawFlags_None);

    for (int i = 1; i < 4; ++i) {
        const float x = map_min.x + map_width * static_cast<float>(i) / 4.0f;
        draw_list->AddLine(ImVec2(x, map_min.y), ImVec2(x, map_max.y), IM_COL32(255, 255, 255, 14), 1.0f);
    }
    for (int i = 1; i < 3; ++i) {
        const float y = map_min.y + map_height * static_cast<float>(i) / 3.0f;
        draw_list->AddLine(ImVec2(map_min.x, y), ImVec2(map_max.x, y), IM_COL32(255, 255, 255, 14), 1.0f);
    }

    if (state.global_unit_status.has_value()) {
        const auto &unit = *state.global_unit_status;
        for (const MapAnchor &anchor : kBases) {
            const bool own = anchor.red_side == we_red;
            const uint32_t hp = own ? unit.base_health() : unit.enemy_base_health();
            const uint32_t shield = own ? unit.base_shield() : unit.enemy_base_shield();
            ImU32 control_color = RoboGo::ui::kTextSecondary;
            if (own) control_color = shield == 1 ? ours : (shield == 2 ? theirs : RoboGo::ui::kTextSecondary);
            else control_color = shield == 1 ? theirs : (shield == 2 ? ours : RoboGo::ui::kTextSecondary);
            const ImVec2 center(map_min.x + anchor.x / kMapWidthDm * map_width,
                                map_min.y + anchor.y / kMapHeightDm * map_height);
            DrawBuildingMarker(draw_list, center, anchor.label, hp, 10000,
                               anchor.red_side ? RoboGo::ui::kTeamRed : RoboGo::ui::kTeamBlue,
                               control_color, shield != 0, scale);
        }
        for (const MapAnchor &anchor : kOutposts) {
            const uint32_t hp = anchor.red_side ? unit.outpost_r_health() : unit.outpost_b_health();
            const uint32_t status = anchor.red_side ? unit.outpost_r_status() : unit.outpost_b_status();
            ImU32 control_color = RoboGo::ui::kTextSecondary;
            if (status == 1 || status == 2) control_color = ours;
            else if (status == 3 || status == 4) control_color = theirs;
            const ImVec2 center(map_min.x + anchor.x / kMapWidthDm * map_width,
                                map_min.y + anchor.y / kMapHeightDm * map_height);
            DrawBuildingMarker(draw_list, center, anchor.label, hp, 1000,
                               anchor.red_side ? RoboGo::ui::kTeamRed : RoboGo::ui::kTeamBlue,
                               control_color, status != 0, scale);
        }
    }

    if (state.radar_info_to_client.has_value()) {
        const auto &radar = *state.radar_info_to_client;
        const std::vector<uint32_t> ids = RadarRobotIds(state, we_red);
        for (int i = 0; i < radar.robots_info_size() && i < static_cast<int>(ids.size()); ++i) {
            const auto &info = radar.robots_info(i);
            if (info.target_pos_x() == 0 && info.target_pos_y() == 0) continue;
            const uint32_t robot_id = ids[i];
            const ImU32 color = IsRedRobotId(robot_id) ? RoboGo::ui::kTeamRed : RoboGo::ui::kTeamBlue;
            const ImVec2 center(map_min.x + static_cast<float>(info.target_pos_x()) / kMapWidthDm * map_width,
                                map_min.y + static_cast<float>(info.target_pos_y()) / kMapHeightDm * map_height);
            const float radius = 10.0f * scale;
            if (robot_id / 100 == (we_red ? 0u : 1u)) {
                draw_list->AddCircle(center, radius + 3.0f * scale, IM_COL32(255, 255, 255, 45), 0, 1.5f * scale);
            }
            draw_list->AddCircleFilled(center, radius, Alpha(color, 70));
            draw_list->AddCircleFilled(center, radius - 3.0f * scale, IM_COL32(22, 26, 33, 255));
            draw_list->AddCircle(center, radius, color, 0, 2.0f * scale);
            if (info.is_high_light() > 0) {
                draw_list->AddCircle(center, radius + 5.0f * scale, RoboGo::ui::kWarning, 0, 1.5f * scale);
            }
            char id_text[8];
            std::snprintf(id_text, sizeof(id_text), "%u", robot_id % 100);
            ImGui::PushFont(ImGui::GetFont(), 12);
            const ImVec2 id_size = ImGui::CalcTextSize(id_text);
            draw_list->AddText(ImVec2(center.x - id_size.x * 0.5f, center.y - id_size.y * 0.5f),
                               RoboGo::ui::kTextPrimary, id_text);
            ImGui::PopFont();
            const UnitHp hp = GetUnitHp(state, robot_id);
            if (hp.known) {
                const ImVec2 bar_min(center.x - 13.0f * scale, center.y + radius + 2.0f * scale);
                const ImVec2 bar_max(center.x + 13.0f * scale, center.y + radius + 5.0f * scale);
                draw_list->AddRectFilled(bar_min, bar_max, IM_COL32(40, 46, 58, 255), 1.5f * scale);
                if (hp.hp > 0) {
                    const float fill = (bar_max.x - bar_min.x) * RatioOf(hp.hp, hp.max_hp);
                    draw_list->AddRectFilled(bar_min, ImVec2(bar_min.x + fill, bar_max.y), color, 1.5f * scale);
                }
            }
        }
    }

    if (g_local_marker.valid) {
        const auto age = std::chrono::steady_clock::now() - g_local_marker.time;
        if (age > std::chrono::seconds(8)) {
            g_local_marker.valid = false;
        } else {
            float alpha = 1.0f;
            if (age > std::chrono::seconds(6)) {
                alpha = 1.0f - static_cast<float>((age - std::chrono::seconds(6)).count()) / 2000.0f;
                alpha = std::clamp(alpha, 0.0f, 1.0f);
            }
            const int a = static_cast<int>(alpha * 255.0f);
            const ImVec2 center(map_min.x + g_local_marker.x / kMapWidthDm * map_width,
                                map_min.y + g_local_marker.y / kMapHeightDm * map_height);
            draw_list->AddCircleFilled(center, 4.0f * scale, Alpha(RoboGo::ui::kWarning, a));
            draw_list->AddCircle(center, 8.0f * scale, Alpha(RoboGo::ui::kWarning, a / 2), 0, 1.5f * scale);
            draw_list->AddLine(ImVec2(center.x - 12.0f * scale, center.y), ImVec2(center.x + 12.0f * scale, center.y),
                               Alpha(RoboGo::ui::kWarning, a), 1.0f);
            draw_list->AddLine(ImVec2(center.x, center.y - 12.0f * scale), ImVec2(center.x, center.y + 12.0f * scale),
                               Alpha(RoboGo::ui::kWarning, a), 1.0f);
            char text[48];
            std::snprintf(text, sizeof(text), "标记 %d,%d", static_cast<int>(g_local_marker.x),
                          static_cast<int>(g_local_marker.y));
            ImGui::PushFont(ImGui::GetFont(), 12);
            draw_list->AddText(ImVec2(center.x + 12.0f * scale, center.y - 6.0f * scale),
                               Alpha(RoboGo::ui::kWarning, a), text);
            ImGui::PopFont();
        }
    }

    if (ImGui::IsItemHovered()) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        draw_list->AddLine(ImVec2(map_min.x, mouse.y), ImVec2(map_max.x, mouse.y), IM_COL32(255, 255, 255, 40), 1.0f);
        draw_list->AddLine(ImVec2(mouse.x, map_min.y), ImVec2(mouse.x, map_max.y), IM_COL32(255, 255, 255, 40), 1.0f);
        const float map_x = (mouse.x - map_min.x) / map_width * kMapWidthDm;
        const float map_y = (mouse.y - map_min.y) / map_height * kMapHeightDm;
        char text[48];
        std::snprintf(text, sizeof(text), "%.0f, %.0f", map_x, map_y);
        ImGui::PushFont(ImGui::GetFont(), 12);
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        const ImVec2 text_pos(mouse.x + 12.0f * scale, mouse.y + 10.0f * scale);
        draw_list->AddRectFilled(ImVec2(text_pos.x - 4.0f, text_pos.y - 2.0f),
                                 ImVec2(text_pos.x + text_size.x + 4.0f, text_pos.y + text_size.y + 2.0f),
                                 IM_COL32(10, 13, 18, 210), 3.0f);
        draw_list->AddText(text_pos, RoboGo::ui::kTextPrimary, text);
        ImGui::PopFont();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const float clamped_x = std::clamp(map_x, 0.0f, kMapWidthDm);
            const float clamped_y = std::clamp(map_y, 0.0f, kMapHeightDm);
            send_map_marker_now(mqtt_client, clamped_x, clamped_y, true);
            g_local_marker.valid = true;
            g_local_marker.x = clamped_x;
            g_local_marker.y = clamped_y;
            g_local_marker.time = std::chrono::steady_clock::now();
        }
    }
}

// ---------------- 右栏：敌方阵容 / 建筑 / 指挥指令 / 链路 ----------------
using RoboGo::io::SendDartCommand;
using RoboGo::io::SendSentryCtrlCommand;

int g_active_tab = 0;   // 0 哨兵 / 1 飞镖 / 2 空中

void DrawSentryTab(RoboGo::io::MqttClient &mqtt_client, ImDrawList *dl, ImVec2 p, float width,
                   const Colors &c, float s) {
    const float row_h = 29.0f * s;
    const float gap = 6.0f * s;
    struct Cmd { const char *label; uint32_t id; };
    const Cmd cmds[9] = {
            {"进攻", 7}, {"防御", 8}, {"移动", 9},
            {"强化进攻", 10}, {"强化防御", 11}, {"强化移动", 12},
            {"远程补弹", 3}, {"远程回血", 4}, {"补弹", 1},
    };
    for (int i = 0; i < 9; ++i) {
        GridButton(cmds[i].label, p, width, row_h, gap, 3, i / 3, i % 3, 1,
                   ButtonKind::Ghost, true, nullptr,
                   [&, i]() { SendSentryCtrlCommand(mqtt_client, cmds[i].id); });
    }
    GridButton("确认复活", p, width, row_h, gap, 3, 3, 0, 1,
               ButtonKind::Primary, true, nullptr, [&]() { SendSentryCtrlCommand(mqtt_client, 5); });
    GridButton("金币复活", p, width, row_h, gap, 3, 3, 1, 2,
               ButtonKind::Danger, true, nullptr, [&]() { SendSentryCtrlCommand(mqtt_client, 6); });
    (void) dl; (void) c;
}

void DrawDartTab(const State &state, RoboGo::io::MqttClient &mqtt_client,
                 ImDrawList *dl, ImVec2 p, float width, const Colors &c, float s) {
    static const char *kTargets[5] = {"前哨站", "基地固定", "随机固定", "随机移动", "末端移动"};
    static uint32_t target_id = 1;
    static bool gate_open = false;
    if (state.dart_select_target_status_sync.has_value()) {
        target_id = state.dart_select_target_status_sync->target_id();
        gate_open = state.dart_select_target_status_sync->open() != 0;
    }
    const int gate_state = state.dart_select_target_status_sync.has_value()
                                   ? static_cast<int>(state.dart_select_target_status_sync->open()) : 0;

    const float row_h = 29.0f * s;
    const float gap = 6.0f * s;
    for (int i = 0; i < 5; ++i) {
        const bool selected = target_id == static_cast<uint32_t>(i + 1);
        GridButton(kTargets[i], p, width, row_h, gap, 3, i / 3, i % 3, 1,
                   selected ? ButtonKind::TabOn : ButtonKind::Ghost, true, nullptr,
                   [&, i]() {
                       target_id = static_cast<uint32_t>(i + 1);
                       SendDartCommand(mqtt_client, target_id, gate_open, false);
                   });
    }
    GridButton(gate_open ? "关闭闸门" : "开启闸门", p, width, row_h, gap, 3, 2, 0, 1,
               ButtonKind::Ghost, true, nullptr,
               [&]() { gate_open = !gate_open; SendDartCommand(mqtt_client, target_id, gate_open, false); });
    const bool can_launch = gate_state == 2;
    GridButton("确认发射", p, width, row_h, gap, 3, 2, 1, 2,
               ButtonKind::Danger, can_launch,
               can_launch ? "确认发射飞镖" : "闸门尚未完全开启，暂时无法发射",
               [&]() { SendDartCommand(mqtt_client, target_id, gate_open, true); });

    char gate_text[64];
    std::snprintf(gate_text, sizeof(gate_text), "闸门 %s",
                  gate_state == 2 ? "已开启" : gate_state == 1 ? "开启中" : "已关闭");
    AddText(dl, ImVec2(p.x + 2.0f * s, p.y + (row_h + gap) * 3.0f + 8.0f * s), c.dim, gate_text, 10);
}

void DrawAirTab(const State &state, ImDrawList *dl, ImVec2 p, float width,
                const Colors &c, float s) {
    const float row_h = 29.0f * s;
    const float gap = 6.0f * s;
    bool found = false;
    float map_x = 0.0f, map_y = 0.0f;
    if (state.radar_info_to_client.has_value()) {
        const auto &radar = *state.radar_info_to_client;
        if (radar.robots_info_size() >= 12) {
            const auto &info = radar.robots_info(10);
            if (info.target_pos_x() != 0 || info.target_pos_y() != 0) {
                found = true;
                map_x = static_cast<float>(info.target_pos_x());
                map_y = static_cast<float>(info.target_pos_y());
            }
        }
    }
    char status[96];
    if (found) std::snprintf(status, sizeof(status), "在场 · 坐标 %.0f, %.0f", map_x, map_y);
    else std::snprintf(status, sizeof(status), "未获取到空中系统位置");
    AddText(dl, ImVec2(p.x + 2.0f * s, p.y + 2.0f * s), found ? c.green : c.dim, status, 10.5f);

    GridButton("紧急停桨", p, width, row_h, gap, 3, 1, 0, 1, ButtonKind::Ghost, false,
               "2027 协议未提供空中系统下行指令；紧急停桨只能由遥控器执行。", []() {});
    GridButton("召回", p, width, row_h, gap, 3, 1, 1, 2, ButtonKind::Ghost, false,
               "2027 协议未提供空中系统下行指令；空中系统按规则全自动行动。", []() {});
    AddText(dl, ImVec2(p.x + 2.0f * s, p.y + (row_h + gap) * 2.0f + 10.0f * s), c.dim,
            "6 号全自动，仅做状态监视", 10);
}

void DrawCommandDeck(const State &state, RoboGo::io::MqttClient &mqtt_client,
                     ImDrawList *dl, ImVec2 p, float width, const Colors &c, float s) {
    SectionLabel(dl, p, width, c, "指挥指令", "哨兵 / 飞镖 / 空中");
    float y = p.y + kSectH * s;

    // 页签：三个分段按钮，选中态用低饱和紫。
    const float tab_gap = 5.0f * s;
    const float tab_w = (width - tab_gap * 2.0f) / 3.0f;
    const char *tabs[3] = {"哨兵", "飞镖", "空中"};
    for (int i = 0; i < 3; ++i) {
        const ImVec2 tp(p.x + (tab_w + tab_gap) * static_cast<float>(i), y);
        if (DeckButton(tabs[i], tp, ImVec2(tab_w, 25.0f * s),
                       g_active_tab == i ? ButtonKind::TabOn : ButtonKind::TabOff)) {
            g_active_tab = i;
        }
    }
    y += 25.0f * s + 9.0f * s;

    const ImVec2 grid_origin(p.x, y);
    if (g_active_tab == 0) DrawSentryTab(mqtt_client, dl, grid_origin, width, c, s);
    else if (g_active_tab == 1) DrawDartTab(state, mqtt_client, dl, grid_origin, width, c, s);
    else DrawAirTab(state, dl, grid_origin, width, c, s);
    y += (29.0f + 6.0f) * 4.0f * s;

    // 指令回执
    const ImVec2 rp(p.x, y);
    const ImVec2 rq(p.x + width, y + 34.0f * s);
    Card(dl, rp, rq, c, 9.0f * s, WithAlpha(0xFFFFFFFFu, 7));
    char receipt[96];
    if (state.sentry_ctrl_result.has_value()) {
        std::snprintf(receipt, sizeof(receipt), "上一条指令 #%u · 结果 %u",
                      state.sentry_ctrl_result->command_id(), state.sentry_ctrl_result->result_code());
    } else {
        std::snprintf(receipt, sizeof(receipt), "暂无指令回执");
    }
    AddText(dl, ImVec2(rp.x + 12.0f * s, rp.y + 10.0f * s), c.text2, receipt, 9.5f);
    AddTextRightAligned(dl, rq.x - 12.0f * s, rp.y + 10.0f * s, c.dim, "链路正常", 9.5f);
}

void DrawLinkStatus(const State &state, RoboGo::io::MqttClient &mqtt_client,
                    bool video_ready, ImDrawList *dl, ImVec2 p, float width, float height,
                    const Colors &c, float s) {
    const ImVec2 q(p.x + width, p.y + height);
    Card(dl, p, q, c, kCardRadius * s, c.surface);
    SectionLabel(dl, ImVec2(p.x + 14.0f * s, p.y + 12.0f * s), width - 28.0f * s, c, "链路状态", "实时");

    const bool connected = mqtt_client.get_connection_state() == RoboGo::io::MqttClient::ConnectionState::Connected;
    const char *rows[4][2] = {
            {"裁判系统", connected ? "在线" : "离线"},
            {"MQTT", "QoS1"},
            {"图传", video_ready ? "已连接" : "无信号"},
            {"雷达", state.radar_info_to_client.has_value() ? "已同步" : "等待数据"},
    };
    float y = p.y + 34.0f * s;
    for (const auto &row : rows) {
        KvRow(dl, ImVec2(p.x + 14.0f * s, y), width - 28.0f * s, c, row[0], row[1], s);
        y += 19.0f * s;
    }
    KvRow(dl, ImVec2(p.x + 14.0f * s, y), width - 28.0f * s, c, "标记下发", "全队（含哨兵）", s);
    const float note_y = y + 22.0f * s;
    if (note_y + 26.0f * s <= q.y - 6.0f * s) {
        AddText(dl, ImVec2(p.x + 14.0f * s, note_y), c.dim,
                "命中固定目标不造成群体扣血；4 发全中会触发对方基地护甲展开。", 9);
        AddText(dl, ImVec2(p.x + 14.0f * s, note_y + 13.0f * s), c.dim,
                "空中系统 2027 起全自动，仅做状态监视。", 9);
    }
}

void DrawRightColumn(const State &state, RoboGo::io::MqttClient &mqtt_client,
                     bool video_ready, const Layout &layout, ImDrawList *dl,
                     const Colors &c, ImU32 ours, ImU32 theirs, float s) {
    const ImVec2 base = At(layout, layout.right_x, layout.col_y);
    const float w = layout.right_w;
    float y = 0.0f;

    DrawRoster(state, dl, ImVec2(base.x, base.y + y), w, false, c, theirs, s);
    y += (kSectH + kUnitH * 2.0f + kUnitGap + kBlockGap) * s;

    const float next = DrawBuildings(dl, ImVec2(base.x, base.y + y), w,
                                     EnemyBuildings(state, WeAreRed(state), ours, theirs),
                                     c, theirs, true, s);
    y = next - base.y + kBlockGap * s;

    DrawCommandDeck(state, mqtt_client, dl, ImVec2(base.x, base.y + y), w, c, s);
    y += (kSectH + 25.0f + 9.0f + (29.0f + 6.0f) * 4.0f + 34.0f + 12.0f) * s;

    const float link_h = std::max(base.y + layout.col_h - (base.y + y) - 4.0f * s, 140.0f * s);
    DrawLinkStatus(state, mqtt_client, video_ready, dl,
                   ImVec2(base.x, base.y + y), w, link_h, c, s);
}

}  // namespace

// 功能：指挥台总入口。详见头文件。
void ShowCommanderPanel(const State &state,
                        RoboGo::io::MqttClient &mqtt_client,
                        ImTextureRef video_texture_ref,
                        const ImVec2 &video_size,
                        bool video_ready,
                        ImTextureRef map_texture_ref,
                        const ImVec2 &map_texture_size,
                        ImTextureRef map_terrain_texture_ref,
                        const ImVec2 &map_terrain_texture_size) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float s = ImGui::GetScale();
    const Layout layout = ComputeLayout(viewport->Pos, viewport->Size, s);
    const Colors colors = MakeColors();
    const ImU32 ours = WeAreRed(state) ? colors.red : colors.blue;
    const ImU32 theirs = WeAreRed(state) ? colors.blue : colors.red;

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(kBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("CommanderConsole", nullptr, flags);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // 背景：与主 HUD 一致的深色中性底，叠一层几乎不可见的扫描线。
    dl->AddRectFilled(layout.origin,
                      ImVec2(layout.origin.x + layout.size.x, layout.origin.y + layout.size.y), kBg);
    for (float y = layout.origin.y; y < layout.origin.y + layout.size.y; y += 4.0f * s) {
        dl->AddLine(ImVec2(layout.origin.x, y), ImVec2(layout.origin.x + layout.size.x, y), kScanLine, 1.0f);
    }

    DrawTopBar(state, mqtt_client, layout, dl, colors, s);
    DrawLeftColumn(state, video_texture_ref, video_size, video_ready, layout, dl, colors, ours, theirs, s);
    DrawRightColumn(state, mqtt_client, video_ready, layout, dl, colors, ours, theirs, s);

    // 中栏：地图（项目原设计）/ KPI / 事件流与关键机制。
    const ImVec2 mid = At(layout, layout.mid_x, layout.col_y);
    // 按场地 280×150 dm 的原始宽高比取高，避免宽窗口下把底图压扁。
    const float map_h = std::clamp(layout.mid_w * (kMapHeightDm / kMapWidthDm), 220.0f * s, 360.0f * s);
    ImGui::SetCursorScreenPos(mid);
    ShowTacticalMap(state, mqtt_client, map_texture_ref, map_texture_size,
                    map_terrain_texture_ref, map_terrain_texture_size,
                    ImVec2(layout.mid_w, map_h));
    const char *map_hint = "点击地图 = 向全队（含哨兵）下发标记";
    const ImVec2 hint_size = TextSize(map_hint, 9);
    const ImVec2 hint_p(mid.x + layout.mid_w - hint_size.x - 24.0f * s, mid.y + 9.0f * s);
    dl->AddRectFilled(ImVec2(hint_p.x - 7.0f * s, hint_p.y - 3.0f * s),
                      ImVec2(hint_p.x + hint_size.x + 7.0f * s, hint_p.y + hint_size.y + 3.0f * s),
                      IM_COL32(8, 10, 14, 160), 5.0f * s);
    AddText(dl, hint_p, WithAlpha(0xFFFFFFFFu, 128), map_hint, 9);

    const float kpi_y = mid.y + map_h + 12.0f * s;
    const float kpi_h = 100.0f * s;
    DrawKpi(state, dl, ImVec2(mid.x, kpi_y), layout.mid_w, kpi_h, colors, ours, theirs, s);

    const float lower_y = kpi_y + kpi_h + 12.0f * s;
    const float lower_h = std::max(mid.y + layout.col_h - lower_y, 150.0f * s);
    const float mech_w = std::clamp(layout.mid_w * 0.395f, 210.0f * s, 240.0f * s);
    const float ev_w = std::max(layout.mid_w - mech_w - 12.0f * s, 160.0f * s);
    DrawEvents(state, dl, ImVec2(mid.x + 4.0f * s, lower_y + 4.0f * s), ev_w - 8.0f * s,
               lower_h - 8.0f * s, colors, s);
    DrawMechanisms(state, dl, ImVec2(mid.x + ev_w + 12.0f * s, lower_y),
                   mech_w, lower_h, colors, s);

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}
