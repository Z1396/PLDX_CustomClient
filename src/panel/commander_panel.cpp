// commander_panel.cpp：指挥手全屏综合指挥台的实现。
// 设计目标：一屏看清"双方阵容 / 建筑 / 资源 / 战场事件 / 战术地图"，
// 并把指挥手真正要做的动作（指挥哨兵、飞镖打击、空中系统状态）放在同一屏，
// 不再需要在小地图、日志和开局面板之间来回切。
#include "commander_panel.h"

#include "keyboard_mouse_state_manager.h"   // 指挥台退出、地图打点、指挥手角色查询
#include "logging/logging.h"                // SPDLOG_INFO/WARN 日志宏

#include <cstdio>   // snprintf：格式化倒计时与血量文本

namespace {   // 匿名命名空间：指挥台内部使用的常量、数据结构与绘制工具

// ---------------- 配色（深色指挥台主题） ----------------
constexpr ImU32 kBg          = IM_COL32(10, 13, 18, 255);    // 整页底色
constexpr ImU32 kPanelBg     = IM_COL32(17, 21, 28, 255);    // 栏底色
constexpr ImU32 kCardBg      = IM_COL32(24, 29, 39, 255);    // 卡片底色
constexpr ImU32 kCardBorder  = IM_COL32(45, 54, 70, 255);    // 卡片描边
constexpr ImU32 kWindowBg    = IM_COL32(13, 17, 23, 255);    // 顶栏 / 地图底色
constexpr ImU32 kText        = IM_COL32(226, 232, 240, 255); // 主文字
constexpr ImU32 kMuted       = IM_COL32(134, 146, 163, 255); // 次要文字
constexpr ImU32 kRed         = IM_COL32(250, 63, 64, 255);   // 红方主色
constexpr ImU32 kBlue        = IM_COL32(20, 176, 240, 255);  // 蓝方主色
constexpr ImU32 kAmber       = IM_COL32(255, 186, 80, 255);  // 警告/标记
constexpr ImU32 kGreen       = IM_COL32(74, 205, 148, 255);  // 正常/在线
constexpr ImU32 kPurple      = IM_COL32(178, 140, 255, 255); // 哨兵/飞镖指令

// 战术地图逻辑尺寸：与雷达坐标系一致（单位 dm）。
constexpr float kMapWidthDm = 280.0f;
constexpr float kMapHeightDm = 150.0f;

// 建筑在地图上的位置（由 map.png 与雷达坐标反复核对得出，单位 dm）。
struct MapAnchor {
    float x;              // 地图坐标 X
    float y;              // 地图坐标 Y
    const char *label;    // 显示名
    bool red_side;        // 是否红方一侧的建筑（只决定描边色）
};   // MapAnchor 结束

constexpr MapAnchor kBases[2] = {
        {22.0f, 27.0f, "红基地", true},     // 红方基地：左上
        {256.0f, 122.0f, "蓝基地", false},  // 蓝方基地：右下
};
constexpr MapAnchor kOutposts[2] = {
        {29.0f, 78.0f, "红前哨", true},     // 红方前哨：左中
        {249.0f, 76.0f, "蓝前哨", false},   // 蓝方前哨：右中
};

// 双方阵容里需要在指挥台展示的 4 台机器人（2027：1/3/4/7）。
struct RobotSlotDef {
    int array_index;      // 在 red_robots / blue_robots 数组中的下标
    int id;               // robot_id 的个位（红方 1/3/4/7，蓝方 101/103/104/107）
    const char *name;     // 显示名
};   // RobotSlotDef 结束

constexpr RobotSlotDef kRobotSlots[4] = {
        {0, 1, "重装"},
        {2, 3, "3号步兵"},
        {3, 4, "4号步兵"},
        {6, 7, "哨兵"},
};

// ---------------- 基础工具 ----------------
ImVec4 Col(ImU32 color) {   // ImU32 -> ImVec4（ImGui 样式接口要 ImVec4）
    return ImGui::ColorConvertU32ToFloat4(color);
}

ImU32 Alpha(ImU32 color, int alpha) {   // 替换颜色的 alpha 通道
    return (color & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24);
}

float RatioOf(uint32_t value, uint32_t max_value) {   // 求比例并夹到 [0,1]，上限为 0 时返回 0
    if (max_value == 0) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(value) / static_cast<float>(max_value), 0.0f, 1.0f);
}

std::string FormatClock(int32_t seconds) {   // 秒 -> "mm:ss"
    const int32_t safe = std::max<int32_t>(seconds, 0);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", safe / 60, safe % 60);
    return std::string(buffer);
}

const char *StageName(uint32_t stage) {   // 比赛阶段中文名（协议 2.2.3）
    switch (stage) {
        case 0: return "未开始";
        case 1: return "准备阶段";
        case 2: return "自检阶段";
        case 3: return "五秒倒计时";
        case 4: return "比赛中";
        case 5: return "结算中";
        default: return "未知阶段";
    }
}

// 判断己方是否红方：robot_id < 100 为红方；缺数据时按红方处理（与其它面板保持一致）。
bool WeAreRed(const State &state) {
    return !state.robot_static_status.has_value() || state.robot_static_status->robot_id() < 100;
}

// 单个机器人的血量快照（来自 GlobalUnitStatus 换算出的 red_robots/blue_robots）。
struct UnitHp {
    uint32_t hp = 0;        // 当前血量
    uint32_t max_hp = 0;    // 历史最大血量（血条基准）
    bool known = false;     // 是否收到过数据
};   // UnitHp 结束

UnitHp GetUnitHp(const State &state, uint32_t robot_id) {   // robot_id 为完整编号（1/101/7/107 等）
    int index = -1;                                        // 对应本地数组下标
    switch (robot_id % 100) {
        case 1: index = 0; break;    // 1 号重装
        case 3: index = 2; break;    // 3 号步兵
        case 4: index = 3; break;    // 4 号步兵
        case 7: index = 6; break;    // 7 号哨兵
        default: index = -1; break;  // 空中/飞镖/雷达不在血量数组里
    }
    if (index < 0) {
        return {};
    }
    const RobotStatus &robot = robot_id < 100 ? state.red_robots[index] : state.blue_robots[index];
    UnitHp result;
    result.hp = robot.health;
    result.max_hp = robot.max_health;
    result.known = robot.max_health > 0;
    return result;
}

// ---------------- 地图坐标换算 ----------------
// 与小地图使用同一块裁剪区域：map.png 里的场地俯视图像素范围。
std::pair<ImVec2, ImVec2> MapUvRect(const ImVec2 &texture_size) {
    constexpr float kLeft = 45.0f;
    constexpr float kTop = 31.0f;
    constexpr float kRight = 1226.0f;
    constexpr float kBottom = 683.0f;
    const float width = std::max(texture_size.x, 1.0f);
    const float height = std::max(texture_size.y, 1.0f);
    return {
            ImVec2(std::clamp(kLeft / width, 0.0f, 1.0f), std::clamp(kTop / height, 0.0f, 1.0f)),
            ImVec2(std::clamp(kRight / width, 0.0f, 1.0f), std::clamp(kBottom / height, 0.0f, 1.0f)),
    };
}

// ---------------- 建筑状态 ----------------
struct BuildingInfo {
    const char *label;         // 建筑名
    uint32_t hp;               // 当前血量
    uint32_t max_hp;           // 满血值（前哨 1000 / 基地 10000）
    const char *control;       // 阵地控制状态文案
    ImU32 control_color;       // 状态配色（我方色/敌方色/灰）
    const char *extra;         // 附加状态（基地护甲）
};   // BuildingInfo 结束

// 前哨阵地状态：1 我方完全 / 2 我方临时 / 3 敌方临时 / 4 敌方完全。
BuildingInfo MakeOutpostInfo(const char *label, uint32_t hp, uint32_t status, ImU32 ours, ImU32 theirs) {
    BuildingInfo info{label, hp, 1000, "未知", kMuted, ""};
    switch (status) {
        case 1: info.control = "我方完全"; info.control_color = ours; break;
        case 2: info.control = "我方临时"; info.control_color = ours; break;
        case 3: info.control = "敌方临时"; info.control_color = theirs; break;
        case 4: info.control = "敌方完全"; info.control_color = theirs; break;
        default: break;
    }
    return info;
}

// 基地阵地状态：own_base=true 用 base_shield（1 我方完全 / 2 敌方临时）；
// own_base=false 用 enemy_base_shield（1 敌方完全 / 2 我方临时）。
BuildingInfo MakeBaseInfo(const char *label, uint32_t hp, uint32_t shield, bool own_base,
                          uint32_t base_status, ImU32 ours, ImU32 theirs) {
    BuildingInfo info{label, hp, 10000, "未知", kMuted, ""};
    if (own_base) {
        if (shield == 1) { info.control = "我方完全"; info.control_color = ours; }
        else if (shield == 2) { info.control = "敌方临时"; info.control_color = theirs; }
    } else {
        if (shield == 1) { info.control = "敌方完全"; info.control_color = theirs; }
        else if (shield == 2) { info.control = "我方临时"; info.control_color = ours; }
    }
    switch (base_status) {   // 基地护甲状态（2.2.4 base_status）
        case 0: info.extra = "100% 防御增益"; break;
        case 1: info.extra = "护甲未展开"; break;
        case 2: info.extra = "护甲展开"; break;
        default: info.extra = ""; break;
    }
    return info;
}

// 我方两座建筑：我方基地 + 属于我方的那座前哨。
std::array<BuildingInfo, 2> OurBuildings(const State &state, bool we_red, ImU32 ours, ImU32 theirs) {
    const GlobalUnitStatus *unit = state.global_unit_status.has_value() ? &*state.global_unit_status : nullptr;
    if (unit == nullptr) {
        return {BuildingInfo{"基地", 0, 10000, "未知", kMuted, ""},
                BuildingInfo{"前哨站", 0, 1000, "未知", kMuted, ""}};
    }
    BuildingInfo base = MakeBaseInfo("基地", unit->base_health(), unit->base_shield(), true,
                                     unit->base_status(), ours, theirs);
    BuildingInfo outpost = we_red
            ? MakeOutpostInfo("前哨站 R", unit->outpost_r_health(), unit->outpost_r_status(), ours, theirs)
            : MakeOutpostInfo("前哨站 B", unit->outpost_b_health(), unit->outpost_b_status(), ours, theirs);
    return {base, outpost};
}

// 敌方两座建筑：敌方基地 + 属于敌方的那座前哨。
std::array<BuildingInfo, 2> EnemyBuildings(const State &state, bool we_red, ImU32 ours, ImU32 theirs) {
    const GlobalUnitStatus *unit = state.global_unit_status.has_value() ? &*state.global_unit_status : nullptr;
    if (unit == nullptr) {
        return {BuildingInfo{"基地", 0, 10000, "未知", kMuted, ""},
                BuildingInfo{"前哨站", 0, 1000, "未知", kMuted, ""}};
    }
    BuildingInfo base = MakeBaseInfo("基地", unit->enemy_base_health(), unit->enemy_base_shield(), false,
                                     unit->enemy_base_status(), ours, theirs);
    BuildingInfo outpost = we_red
            ? MakeOutpostInfo("前哨站 B", unit->outpost_b_health(), unit->outpost_b_status(), ours, theirs)
            : MakeOutpostInfo("前哨站 R", unit->outpost_r_health(), unit->outpost_r_status(), ours, theirs);
    return {base, outpost};
}

// ---------------- 通用绘制 ----------------
void SectionTitle(const char *title, const char *right_text, ImU32 accent) {
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 22.0f * ImGui::GetScale();
    // 左侧竖条 + 标题，右侧可选的计数/说明
    draw_list->AddRectFilled(ImVec2(pos.x, pos.y + 4.0f), ImVec2(pos.x + 3.0f, pos.y + height - 4.0f), accent, 1.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kText));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 9.0f, pos.y + 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (right_text != nullptr && right_text[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
        const ImVec2 size = ImGui::CalcTextSize(right_text);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + width - size.x, pos.y + 3.0f));
        ImGui::TextUnformatted(right_text);
        ImGui::PopStyleColor();
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height));
}

// 机器人卡片：名字/编号、血量、弹量、底部血条。高度 48（随缩放）。
void DrawRobotCard(ImDrawList *draw_list, ImVec2 pos, float width, const RobotSlotDef &slot,
                   const UnitHp &hp, int bullets, ImU32 team_color, bool ally) {
    const float scale = ImGui::GetScale();
    const float height = 48.0f * scale;
    const ImVec2 max(pos.x + width, pos.y + height);

    const bool dead = hp.known && hp.hp == 0;
    draw_list->AddRectFilled(pos, max, dead ? IM_COL32(30, 20, 22, 255) : kCardBg, 6.0f * scale);
    draw_list->AddRect(pos, max, Alpha(team_color, dead ? 40 : 90), 6.0f * scale, 1.0f, ImDrawFlags_None);
    draw_list->AddRectFilled(pos, ImVec2(pos.x + 3.0f * scale, max.y), Alpha(team_color, dead ? 90 : 220),
                             3.0f * scale, ImDrawFlags_RoundCornersLeft);

    // 第一行：编号 + 兵种名（左），血量（右）
    char name_text[48];
    std::snprintf(name_text, sizeof(name_text), "%d  %s", slot.id, slot.name);
    draw_list->AddText(ImVec2(pos.x + 12.0f * scale, pos.y + 6.0f * scale), kText, name_text);

    char hp_text[48];
    if (hp.known) {
        std::snprintf(hp_text, sizeof(hp_text), "%u / %u", hp.hp, hp.max_hp);
    } else {
        std::snprintf(hp_text, sizeof(hp_text), "等待数据");
    }
    const ImVec2 hp_size = ImGui::CalcTextSize(hp_text);
    draw_list->AddText(ImVec2(max.x - 12.0f * scale - hp_size.x, pos.y + 6.0f * scale),
                       dead ? kRed : (ally ? kText : Alpha(team_color, 255)), hp_text);

    // 第二行：状态 / 弹量（左），血量百分比（右）
    const char *state_text = dead ? "阵亡" : (hp.known ? "在线" : "离线");
    draw_list->AddText(ImVec2(pos.x + 12.0f * scale, pos.y + 24.0f * scale),
                       dead ? kRed : kMuted, state_text);
    char ammo_text[48];
    std::snprintf(ammo_text, sizeof(ammo_text), "弹量 %d", bullets);
    draw_list->AddText(ImVec2(pos.x + 52.0f * scale, pos.y + 24.0f * scale), kMuted, ammo_text);

    if (hp.known) {
        char percent_text[16];
        std::snprintf(percent_text, sizeof(percent_text), "%d%%",
                      static_cast<int>(RatioOf(hp.hp, hp.max_hp) * 100.0f + 0.5f));
        const ImVec2 percent_size = ImGui::CalcTextSize(percent_text);
        draw_list->AddText(ImVec2(max.x - 12.0f * scale - percent_size.x, pos.y + 24.0f * scale),
                           Alpha(team_color, 235), percent_text);
    }

    // 底部血条
    const ImVec2 bar_min(pos.x + 12.0f * scale, max.y - 8.0f * scale);
    const ImVec2 bar_max(max.x - 12.0f * scale, max.y - 4.0f * scale);
    draw_list->AddRectFilled(bar_min, bar_max, IM_COL32(48, 55, 68, 255), 2.0f * scale);
    if (hp.known && hp.hp > 0) {
        const float fill = (bar_max.x - bar_min.x) * RatioOf(hp.hp, hp.max_hp);
        draw_list->AddRectFilled(bar_min, ImVec2(bar_min.x + fill, bar_max.y), team_color, 2.0f * scale);
    }
}   // DrawRobotCard 结束

// 建筑行：名称 / 控制状态 / 血量 / 血条。高度 46（随缩放）。
void DrawBuildingRow(ImDrawList *draw_list, ImVec2 pos, float width, const BuildingInfo &info, ImU32 side_color) {
    const float scale = ImGui::GetScale();
    const float height = 46.0f * scale;
    const ImVec2 max(pos.x + width, pos.y + height);

    draw_list->AddRectFilled(pos, max, kCardBg, 5.0f * scale);
    draw_list->AddRect(pos, max, Alpha(side_color, 55), 5.0f * scale, 1.0f, ImDrawFlags_None);
    draw_list->AddText(ImVec2(pos.x + 10.0f * scale, pos.y + 5.0f * scale), kText, info.label);

    // 控制状态徽标：底色调暗，文字用状态色
    const ImVec2 control_size = ImGui::CalcTextSize(info.control);
    const ImVec2 chip_min(max.x - 10.0f * scale - control_size.x - 8.0f * scale, pos.y + 4.0f * scale);
    const ImVec2 chip_max(max.x - 10.0f * scale, pos.y + 5.0f * scale + control_size.y + 2.0f * scale);
    draw_list->AddRectFilled(chip_min, chip_max, Alpha(info.control_color, 40), 3.0f * scale);
    draw_list->AddText(ImVec2(chip_min.x + 4.0f * scale, chip_min.y + 1.0f * scale), info.control_color, info.control);

    // 血量文本与护甲补充说明
    char hp_text[48];
    std::snprintf(hp_text, sizeof(hp_text), "%u / %u", info.hp, info.max_hp);
    draw_list->AddText(ImVec2(pos.x + 10.0f * scale, pos.y + 24.0f * scale), kMuted, hp_text);
    if (info.extra[0] != '\0') {
        const ImVec2 extra_size = ImGui::CalcTextSize(info.extra);
        draw_list->AddText(ImVec2(max.x - 10.0f * scale - extra_size.x, pos.y + 24.0f * scale),
                           Alpha(kAmber, 220), info.extra);
    }

    const ImVec2 bar_min(pos.x + 10.0f * scale, max.y - 7.0f * scale);
    const ImVec2 bar_max(max.x - 10.0f * scale, max.y - 4.0f * scale);
    draw_list->AddRectFilled(bar_min, bar_max, IM_COL32(48, 55, 68, 255), 2.0f * scale);
    if (info.hp > 0) {
        const float fill = (bar_max.x - bar_min.x) * RatioOf(info.hp, info.max_hp);
        draw_list->AddRectFilled(bar_min, ImVec2(bar_min.x + fill, bar_max.y), side_color, 2.0f * scale);
    }
}   // DrawBuildingRow 结束

// 一行推进光标：直接塞一个占位控件，避免手写 cursor 造成重叠。
void AdvanceRow(float height) {
    ImGui::Dummy(ImVec2(1.0f, height));
}

// ---------------- 指令下发（不进比赛模式） ----------------
// 指挥台必须保持鼠标可见，所以这里不能复用开局面板里"发完就进比赛"的封装。
template<typename Message>
void PublishCommanderCommand(RoboGo::io::MqttClient &mqtt_client, const char *topic, const Message &message) {
    std::string serialized;
    if (message.SerializeToString(&serialized)) {
        mqtt_client.publish(topic, serialized);
        SPDLOG_INFO("指挥台下发指令 topic={} bytes={}", topic, serialized.size());
    } else {
        SPDLOG_ERROR("指挥台指令序列化失败 topic={}", topic);
    }
}

void SendSentryCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t command_id) {
    SentryCtrlCommand command;
    command.set_command_id(command_id);
    PublishCommanderCommand(mqtt_client, "SentryCtrlCommand", command);
}

void SendDartCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t target_id, bool open, bool launch) {
    DartCommand command;
    command.set_target_id(target_id);
    command.set_open(open);
    command.set_launch_confirm(launch);
    PublishCommanderCommand(mqtt_client, "DartCommand", command);
}

// 通用按钮：自定义配色 + 可禁用 + 悬停说明。
bool CommandButton(const char *label, const ImVec2 &size, ImU32 color, bool enabled = true,
                   const char *tooltip = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Button, Col(Alpha(color, 210)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col(Alpha(color, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col(Alpha(color, 170)));
    ImGui::PushStyleColor(ImGuiCol_Text, Col(IM_COL32(16, 18, 22, 255)));
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, size);
    ImGui::EndDisabled();
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                                   ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::PopStyleColor(4);
    return clicked;
}

// ---------------- 顶栏 ----------------
void DrawHeader(const State &state, RoboGo::io::MqttClient &mqtt_client, ImVec2 pos, ImVec2 size) {
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const float scale = ImGui::GetScale();
    const bool we_red = WeAreRed(state);
    const ImU32 ours = we_red ? kRed : kBlue;
    const ImU32 theirs = we_red ? kBlue : kRed;
    const ImVec2 max(pos.x + size.x, pos.y + size.y);

    draw_list->AddRectFilled(pos, max, kWindowBg);
    draw_list->AddLine(ImVec2(pos.x, max.y - 1.0f), ImVec2(max.x, max.y - 1.0f), Alpha(ours, 160), 2.0f);

    // 左侧：标题 + 队伍/局号
    draw_list->AddRectFilled(ImVec2(pos.x + 16.0f * scale, pos.y + 12.0f * scale),
                             ImVec2(pos.x + 20.0f * scale, pos.y + 46.0f * scale), ours, 2.0f);
    ImGui::PushFont(ImGui::GetFont(), 20);
    draw_list->AddText(ImVec2(pos.x + 28.0f * scale, pos.y + 8.0f * scale), kText, "综合指挥台");
    ImGui::PopFont();

    char subtitle[96];
    const uint32_t round = state.game_status.has_value() ? state.game_status->current_round() : 0;
    const uint32_t total_rounds = state.game_status.has_value() ? state.game_status->total_rounds() : 0;
    std::snprintf(subtitle, sizeof(subtitle), "%s · 第 %u/%u 局 · 指挥手",
                  we_red ? "红队" : "蓝队", round, total_rounds);
    draw_list->AddText(ImVec2(pos.x + 28.0f * scale, pos.y + 34.0f * scale), kMuted, subtitle);

    // 中间：比分 + 阶段倒计时
    const uint32_t red_score = state.game_status.has_value() ? state.game_status->red_score() : 0;
    const uint32_t blue_score = state.game_status.has_value() ? state.game_status->blue_score() : 0;
    char score_text[32];
    std::snprintf(score_text, sizeof(score_text), "%u  :  %u", red_score, blue_score);
    ImGui::PushFont(ImGui::GetFont(), 26);
    const ImVec2 score_size = ImGui::CalcTextSize(score_text);
    const float center_x = pos.x + size.x * 0.5f;
    draw_list->AddText(ImVec2(center_x - score_size.x * 0.5f, pos.y + 4.0f * scale), kText, score_text);
    ImGui::PopFont();
    // 比分两侧标红/蓝，避免只看到两个数字不知道哪边是哪队
    ImGui::PushFont(ImGui::GetFont(), 12);
    draw_list->AddText(ImVec2(center_x - score_size.x * 0.5f - 20.0f * scale, pos.y + 14.0f * scale), kRed, "红");
    draw_list->AddText(ImVec2(center_x + score_size.x * 0.5f + 8.0f * scale, pos.y + 14.0f * scale), kBlue, "蓝");
    ImGui::PopFont();

    const char *stage = state.game_status.has_value() ? StageName(state.game_status->current_stage()) : "等待数据";
    const int32_t countdown = state.game_status.has_value() ? state.game_status->stage_countdown_sec() : 0;
    const std::string clock = FormatClock(countdown);
    char stage_text[96];
    std::snprintf(stage_text, sizeof(stage_text), "%s   %s", stage, clock.c_str());
    const ImVec2 stage_size = ImGui::CalcTextSize(stage_text);
    draw_list->AddText(ImVec2(center_x - stage_size.x * 0.5f, pos.y + 38.0f * scale),
                       state.game_status.has_value() ? kMuted : kRed, stage_text);

    // 右侧：经济 / 在线状态
    // 右侧信息区：固定从"退出按钮左侧 200px"开始排，先算好按钮占位，避免文字压到按钮上。
    const float exit_width = 96.0f * scale;
    const float exit_x = pos.x + size.x - exit_width - 14.0f * scale;
    const float info_x = exit_x - 190.0f * scale;

    const uint32_t gold = state.global_logistics_status.has_value()
                                  ? state.global_logistics_status->remaining_economy()
                                  : 0;
    char gold_text[48];
    std::snprintf(gold_text, sizeof(gold_text), "经济 %u", gold);
    draw_list->AddText(ImVec2(info_x, pos.y + 10.0f * scale), kAmber, gold_text);

    const bool connected = mqtt_client.get_connection_state() == RoboGo::io::MqttClient::ConnectionState::Connected;
    draw_list->AddCircleFilled(ImVec2(info_x + 4.0f * scale, pos.y + 38.0f * scale), 4.0f * scale,
                               connected ? kGreen : kRed);
    draw_list->AddText(ImVec2(info_x + 12.0f * scale, pos.y + 31.0f * scale), kMuted,
                       connected ? "裁判系统在线" : "裁判系统离线");

    // 暂停徽标：挂在阶段文字右侧，别跟右侧信息区抢位置
    if (state.game_status.has_value() && state.game_status->is_paused()) {
        const ImVec2 chip_pos(center_x + stage_size.x * 0.5f + 10.0f * scale, pos.y + 38.0f * scale);
        draw_list->AddRectFilled(chip_pos, ImVec2(chip_pos.x + 52.0f * scale, chip_pos.y + 17.0f * scale),
                                 Alpha(kRed, 70), 3.0f * scale);
        draw_list->AddText(ImVec2(chip_pos.x + 7.0f * scale, chip_pos.y + 1.0f * scale), kRed, "已暂停");
    }

    // 退出按钮：指挥手唯一的出口——直接退出登录回到身份选择页，中间不再有"赛前指令页"。
    ImGui::SetCursorScreenPos(ImVec2(exit_x, pos.y + 15.0f * scale));
    if (CommandButton("退出登录", ImVec2(exit_width, 30.0f * scale), kMuted, true,
                      "退出登录并回到身份选择页（Esc 在指挥台内不会退出，避免现场误触掉线）。")) {
        leave_commander_console(mqtt_client);
    }
}   // DrawHeader 结束

// ---------------- 阵容栏 ----------------
void ShowRoster(const State &state, const char *title, bool ally, ImU32 team_color) {
    const bool we_red = WeAreRed(state);
    const int alive = [&]() {
        int count = 0;
        for (const RobotSlotDef &slot : kRobotSlots) {
            const UnitHp hp = GetUnitHp(state, ally == we_red ? slot.id : slot.id + 100);
            if (!hp.known || hp.hp > 0) {
                ++count;
            }
        }
        return count;
    }();

    char right_text[32];
    std::snprintf(right_text, sizeof(right_text), "%d/4 在线", alive);
    SectionTitle(title, right_text, team_color);

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    for (const RobotSlotDef &slot : kRobotSlots) {
        const uint32_t robot_id = ally == we_red ? static_cast<uint32_t>(slot.id)
                                                 : static_cast<uint32_t>(slot.id + 100);
        const UnitHp hp = GetUnitHp(state, robot_id);
        const int bullets = ally == we_red ? state.red_robots[slot.array_index].bullets
                                           : state.blue_robots[slot.array_index].bullets;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        DrawRobotCard(draw_list, pos, width, slot, hp, bullets, team_color, ally);
        AdvanceRow(48.0f * ImGui::GetScale() + 5.0f * ImGui::GetScale());
    }
}   // ShowRoster 结束

// ---------------- 建筑栏 ----------------
void ShowBuildings(const State &state, const char *title, const std::array<BuildingInfo, 2> &buildings,
                   ImU32 side_color) {
    SectionTitle(title, "", side_color);
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    for (const BuildingInfo &info : buildings) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        DrawBuildingRow(draw_list, pos, width, info, side_color);
        AdvanceRow(46.0f * ImGui::GetScale() + 5.0f * ImGui::GetScale());
    }
}   // ShowBuildings 结束

// ---------------- 资源与机制 ----------------
void ShowResources(const State &state, ImU32 ours) {
    SectionTitle("资源与机制", "", ours);

    const uint32_t gold = state.global_logistics_status.has_value()
                                  ? state.global_logistics_status->remaining_economy()
                                  : 0;
    const uint32_t tech = state.global_logistics_status.has_value()
                                  ? state.global_logistics_status->tech_level()
                                  : 0;
    const uint32_t encryption = state.global_logistics_status.has_value()
                                        ? state.global_logistics_status->encryption_level()
                                        : 0;
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
    ImGui::Text("经济 %u    科技 %u 级    加密 %u 级", gold, tech, encryption);
    ImGui::PopStyleColor();

    // 堡垒占领计时（2.2.6）：1=己方堡垒被占，2=对方堡垒被占
    if (state.global_special_mechanism.has_value()) {
        const auto &mechanism = *state.global_special_mechanism;
        bool shown = false;
        for (int i = 0; i < mechanism.mechanism_id_size(); ++i) {
            const uint32_t id = mechanism.mechanism_id(i);
            const int32_t seconds = i < mechanism.mechanism_time_sec_size() ? mechanism.mechanism_time_sec(i) : 0;
            const char *label = id == 1 ? "己方堡垒被占" : (id == 2 ? "对方堡垒被占" : nullptr);
            if (label == nullptr) {
                continue;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, Col(id == 1 ? kRed : kGreen));
            ImGui::Text("%s  %s", label, FormatClock(seconds).c_str());
            ImGui::PopStyleColor();
            shown = true;
        }
        if (!shown) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
            ImGui::TextUnformatted("堡垒：无占领计时");
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
        ImGui::TextUnformatted("堡垒：等待数据");
        ImGui::PopStyleColor();
    }
}   // ShowResources 结束

// ---------------- 现场画面 ----------------
void ShowVideoPreview(ImTextureRef texture, const ImVec2 &texture_size, bool ready, ImU32 accent) {
    SectionTitle("现场画面", ready ? "LIVE" : "无信号", accent);
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = width * 9.0f / 16.0f;
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 max(pos.x + width, pos.y + height);
    draw_list->AddRectFilled(pos, max, IM_COL32(8, 10, 14, 255), 4.0f);
    if (ready && texture.GetTexID() != ImTextureID_Invalid && texture_size.x > 0.0f && texture_size.y > 0.0f) {
        // 等比适配，避免画面被拉伸
        const float target_ratio = width / height;
        const float source_ratio = texture_size.x / texture_size.y;
        ImVec2 uv0(0.0f, 0.0f);
        ImVec2 uv1(1.0f, 1.0f);
        if (source_ratio > target_ratio) {   // 源更宽：裁左右
            const float visible = target_ratio / source_ratio;
            uv0.x = (1.0f - visible) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        } else {   // 源更高：裁上下
            const float visible = source_ratio / target_ratio;
            uv0.y = (1.0f - visible) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        }
        draw_list->AddImage(texture, pos, max, uv0, uv1);
    } else {
        const char *text = "暂无图传画面";
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        draw_list->AddText(ImVec2(pos.x + (width - text_size.x) * 0.5f, pos.y + (height - text_size.y) * 0.5f),
                           kMuted, text);
    }
    draw_list->AddRect(pos, max, kCardBorder, 4.0f);
    AdvanceRow(height + 4.0f);
}   // ShowVideoPreview 结束

// ---------------- 指令面板 ----------------
void ShowSentryTab(const State &state, RoboGo::io::MqttClient &mqtt_client) {
    const float scale = ImGui::GetScale();
    // 当前姿态
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
    if (state.sentry_status_sync.has_value()) {
        const auto &sync = *state.sentry_status_sync;
        const char *posture = sync.posture_id() == 1 ? "进攻" : sync.posture_id() == 2 ? "防御"
                : sync.posture_id() == 3 ? "移动" : "未知";
        ImGui::Text("姿态 %s · 弱化 %s · 强化 %s", posture,
                    sync.is_weakened() ? "是" : "否", sync.is_powered() ? "是" : "否");
    } else {
        ImGui::TextUnformatted("姿态：等待同步");
    }
    if (state.sentry_ctrl_result.has_value()) {
        ImGui::Text("上一条指令 %u → 结果 %u", state.sentry_ctrl_result->command_id(),
                    state.sentry_ctrl_result->result_code());
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();
    // 按钮网格改用表格：每列按可用宽度等分，不会再因为 SameLine 累加间隙而溢出面板右边界
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.0f * scale, 2.0f * scale));
    const float row_height = 30.0f * scale;
    auto cell_button = [&](const char *label, ImU32 color, uint32_t command_id) {
        const ImVec2 cell_size(ImGui::GetContentRegionAvail().x, row_height);
        if (CommandButton(label, cell_size, color)) {
            SendSentryCommand(mqtt_client, command_id);
        }
    };

    if (ImGui::BeginTable("##sentry_commands", 3, ImGuiTableFlags_SizingStretchSame)) {
        // 姿态
        ImGui::TableNextColumn(); cell_button("进攻", kPurple, 7);
        ImGui::TableNextColumn(); cell_button("防御", kPurple, 8);
        ImGui::TableNextColumn(); cell_button("移动", kPurple, 9);
        // 强化姿态
        ImGui::TableNextColumn(); cell_button("强化进攻", kPurple, 10);
        ImGui::TableNextColumn(); cell_button("强化防御", kPurple, 11);
        ImGui::TableNextColumn(); cell_button("强化移动", kPurple, 12);
        // 补给
        ImGui::TableNextColumn(); cell_button("补弹", kGreen, 1);
        ImGui::TableNextColumn(); cell_button("远程补弹", kGreen, 3);
        ImGui::TableNextColumn(); cell_button("远程回血", kGreen, 4);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("##sentry_revive", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); cell_button("确认复活", kAmber, 5);
        ImGui::TableNextColumn(); cell_button("金币复活", kAmber, 6);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
    ImGui::TextWrapped("指挥手下发哨兵指令会覆盖哨兵的自动决策；自动哨兵模式下每次干预会扣除全队金币。");
    ImGui::PopStyleColor();
}   // ShowSentryTab 结束

void ShowDartTab(const State &state, RoboGo::io::MqttClient &mqtt_client) {
    const float scale = ImGui::GetScale();
    static const char *kTargets[5] = {"前哨站", "基地固定目标", "基地随机固定", "基地随机移动", "基地末端移动"};

    // 本地目标/闸门状态：收到服务器同步就覆盖（与开局面板同一套逻辑）
    static uint32_t target_id = 1;
    static bool gate_open = false;
    if (state.dart_select_target_status_sync.has_value()) {
        target_id = state.dart_select_target_status_sync->target_id();
        gate_open = state.dart_select_target_status_sync->open() != 0;
    }
    const int gate_state = state.dart_select_target_status_sync.has_value()
                                   ? static_cast<int>(state.dart_select_target_status_sync->open())
                                   : 0;

    ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
    ImGui::Text("闸门：%s", gate_state == 2 ? "已开启" : gate_state == 1 ? "开启中" : "已关闭");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // 目标选择：两列表格，行距由表格统管，不会因为 SameLine + NewLine 多出空行
    const float gap = 6.0f * scale;
    if (ImGui::BeginTable("##dart_targets", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (int i = 0; i < 5; ++i) {
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, Col(target_id == static_cast<uint32_t>(i + 1) ? kText : kMuted));
            if (ImGui::RadioButton(kTargets[i], target_id == static_cast<uint32_t>(i + 1))) {
                target_id = static_cast<uint32_t>(i + 1);
                SendDartCommand(mqtt_client, target_id, gate_open, false);   // 切目标即时下发
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    const float button_width = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    const ImVec2 button_size(button_width, 32.0f * scale);
    if (CommandButton(gate_open ? "关闭闸门" : "开启闸门", button_size, kAmber)) {
        gate_open = !gate_open;
        SendDartCommand(mqtt_client, target_id, gate_open, false);
    }
    ImGui::SameLine(0.0f, gap);
    const bool can_launch = gate_state == 2;   // 协议要求闸门"已开启"才能确认发射
    if (CommandButton("确认发射", button_size, kRed, can_launch,
                      can_launch ? "确认发射飞镖" : "闸门尚未完全开启，暂时无法发射")) {
        SendDartCommand(mqtt_client, target_id, gate_open, true);
    }

    ImGui::Spacing();
    if (target_id == 4 || target_id == 5) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kRed));
        ImGui::TextWrapped("警告：该目标命中后对方全体存活地面机器人扣除 20%% / 50%% 上限血量，发射前请确认。");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
        ImGui::TextWrapped("命中固定目标不造成群体扣血；4发全中固定目标会触发对方基地护甲展开。");
        ImGui::PopStyleColor();
    }
}   // ShowDartTab 结束

void ShowAirTab(const State &state) {
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
    ImGui::TextWrapped("2027赛季：6号为全自动空中系统，提供全场视野与冲击伤害，不允许发射机构。");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kText));
    ImGui::TextUnformatted("空中系统状态");
    ImGui::PopStyleColor();

    // 雷达 12 槽位里才有 6 号；2027 的 8 槽位数组不含它。
    bool found = false;
    float map_x = 0.0f;
    float map_y = 0.0f;
    if (state.radar_info_to_client.has_value()) {
        const auto &radar = *state.radar_info_to_client;
        if (radar.robots_info_size() >= 12) {
            // 12 槽位顺序固定为「对方 1/2/3/4/6/7 + 己方 1/2/3/4/6/7」，
            // 己方 6 号（空中系统）位于己方段的第 5 项，即整体下标 10，与红蓝无关。
            const int air_index = 10;
            const auto &info = radar.robots_info(air_index);
            if (info.target_pos_x() != 0 || info.target_pos_y() != 0) {
                found = true;
                map_x = static_cast<float>(info.target_pos_x());
                map_y = static_cast<float>(info.target_pos_y());
            }
        }
    }
    if (found) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kGreen));
        ImGui::Text("在场 · 坐标 %.0f, %.0f", map_x, map_y);
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
        ImGui::TextUnformatted("未获取到空中系统位置（雷达 8 槽位数组不含 6 号）");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    const float gap = 6.0f * ImGui::GetScale();
    const float button_width = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    const ImVec2 button_size(button_width, 32.0f * ImGui::GetScale());
    CommandButton("紧急停桨", button_size, kMuted, false,
                  "2027 协议未提供空中系统下行指令；紧急停桨只能由遥控器执行。");
    ImGui::SameLine(0.0f, gap);
    CommandButton("召回", button_size, kMuted, false,
                  "2027 协议未提供空中系统下行指令；空中系统按规则全自动行动。");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
    ImGui::TextWrapped("此处只做状态监视。2026的“空中支援呼叫”已在2027协议中删除。");
    ImGui::PopStyleColor();
}   // ShowAirTab 结束

void ShowCommandDeck(const State &state, RoboGo::io::MqttClient &mqtt_client, ImU32 accent) {
    SectionTitle("指挥指令", "", accent);

    ImGui::PushStyleColor(ImGuiCol_Tab, Col(kCardBg));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, Col(Alpha(accent, 120)));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, Col(Alpha(accent, 200)));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, Col(accent));
    ImGui::PushStyleColor(ImGuiCol_Text, Col(kText));
    if (ImGui::BeginTabBar("##commander_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("哨兵")) {
            ShowSentryTab(state, mqtt_client);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("飞镖")) {
            ShowDartTab(state, mqtt_client);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("空中")) {
            ShowAirTab(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleColor(5);
}   // ShowCommandDeck 结束

// ---------------- 战术地图 ----------------
struct LocalMarker {
    bool valid = false;                                 // 是否点过
    float x = 0.0f;                                     // 地图坐标 X（dm）
    float y = 0.0f;                                     // 地图坐标 Y（dm）
    std::chrono::steady_clock::time_point time{};        // 下发时刻
};   // LocalMarker 结束

LocalMarker g_local_marker;   // 指挥台本地标记点

// 把雷达槽位映射成 robot_id（与 mini_map 完全一致，兼容 8 槽位与旧 12 槽位）。
std::vector<uint32_t> RadarRobotIds(const State &state, bool we_red) {
    const auto &radar = *state.radar_info_to_client;
    if (radar.robots_info_size() >= 12) {
        return we_red ? std::vector<uint32_t>{101, 102, 103, 104, 106, 107, 1, 2, 3, 4, 6, 7}
                      : std::vector<uint32_t>{1, 2, 3, 4, 6, 7, 101, 102, 103, 104, 106, 107};
    }
    return we_red ? std::vector<uint32_t>{101, 103, 104, 107, 1, 3, 4, 7}
                  : std::vector<uint32_t>{1, 3, 4, 7, 101, 103, 104, 107};
}

// 绘制一个建筑标记：菱形 + 血条 + 控制状态点。
void DrawBuildingMarker(ImDrawList *draw_list, ImVec2 center, const char *label, uint32_t hp, uint32_t max_hp,
                        ImU32 side_color, ImU32 control_color, bool has_control, float scale) {
    const float radius = 9.0f * scale;
    const ImU32 fill = Alpha(side_color, 46);
    const ImVec2 points[4] = {
            ImVec2(center.x, center.y - radius),
            ImVec2(center.x + radius, center.y),
            ImVec2(center.x, center.y + radius),
            ImVec2(center.x - radius, center.y),
    };
    draw_list->AddConvexPolyFilled(points, 4, fill);
    draw_list->AddPolyline(points, 4, side_color, 2.0f * scale, ImDrawFlags_Closed);
    if (has_control) {
        draw_list->AddCircleFilled(center, 3.0f * scale, control_color);
    }
    if (hp == 0) {   // 被摧毁：打叉
        draw_list->AddLine(ImVec2(center.x - radius * 0.6f, center.y - radius * 0.6f),
                           ImVec2(center.x + radius * 0.6f, center.y + radius * 0.6f), kRed, 2.0f * scale);
        draw_list->AddLine(ImVec2(center.x - radius * 0.6f, center.y + radius * 0.6f),
                           ImVec2(center.x + radius * 0.6f, center.y - radius * 0.6f), kRed, 2.0f * scale);
    }

    ImGui::PushFont(ImGui::GetFont(), 12);
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    draw_list->AddText(ImVec2(center.x - label_size.x * 0.5f, center.y - radius - label_size.y - 2.0f * scale),
                       side_color, label);
    char hp_text[16];
    std::snprintf(hp_text, sizeof(hp_text), "%d%%", static_cast<int>(RatioOf(hp, max_hp) * 100.0f + 0.5f));
    const ImVec2 hp_size = ImGui::CalcTextSize(hp_text);
    draw_list->AddText(ImVec2(center.x - hp_size.x * 0.5f, center.y + radius + 2.0f * scale), kMuted, hp_text);
    ImGui::PopFont();
}   // DrawBuildingMarker 结束

void ShowTacticalMap(const State &state, RoboGo::io::MqttClient &mqtt_client,
                     ImTextureRef texture, const ImVec2 &texture_size, ImVec2 size) {
    const float scale = ImGui::GetScale();
    const bool we_red = WeAreRed(state);
    const ImU32 ours = we_red ? kRed : kBlue;
    const ImU32 theirs = we_red ? kBlue : kRed;

    // 先把地图区域占住（InvisibleButton 负责接收点击）
    ImGui::PushID("CommanderMap");
    ImGui::InvisibleButton("map_area", size);
    ImGui::PopID();

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const ImVec2 map_min = ImGui::GetItemRectMin();
    const ImVec2 map_max = ImGui::GetItemRectMax();
    const float map_width = size.x;
    const float map_height = size.y;

    draw_list->AddRectFilled(map_min, map_max, kWindowBg, 6.0f * scale);
    if (texture.GetTexID() != ImTextureID_Invalid && texture_size.x > 0.0f && texture_size.y > 0.0f) {
        const auto [uv_min, uv_max] = MapUvRect(texture_size);
        draw_list->AddImage(texture, map_min, map_max, uv_min, uv_max);
        draw_list->AddRectFilled(map_min, map_max, IM_COL32(8, 16, 24, 66));   // 压暗底图，突出单位
    }
    draw_list->AddRect(map_min, map_max, Alpha(ours, 120), 6.0f * scale, 1.0f, ImDrawFlags_None);

    // 参考网格：四等分，方便口述位置
    for (int i = 1; i < 4; ++i) {
        const float x = map_min.x + map_width * static_cast<float>(i) / 4.0f;
        draw_list->AddLine(ImVec2(x, map_min.y), ImVec2(x, map_max.y), IM_COL32(255, 255, 255, 14), 1.0f);
    }
    for (int i = 1; i < 3; ++i) {
        const float y = map_min.y + map_height * static_cast<float>(i) / 3.0f;
        draw_list->AddLine(ImVec2(map_min.x, y), ImVec2(map_max.x, y), IM_COL32(255, 255, 255, 14), 1.0f);
    }

    // ---- 建筑标记 ----
    if (state.global_unit_status.has_value()) {
        const auto &unit = *state.global_unit_status;
        for (const MapAnchor &anchor : kBases) {
            const bool own = anchor.red_side == we_red;
            const uint32_t hp = own ? unit.base_health() : unit.enemy_base_health();
            const uint32_t shield = own ? unit.base_shield() : unit.enemy_base_shield();
            ImU32 control_color = kMuted;
            if (own) {
                control_color = shield == 1 ? ours : (shield == 2 ? theirs : kMuted);
            } else {
                control_color = shield == 1 ? theirs : (shield == 2 ? ours : kMuted);
            }
            const ImVec2 center(map_min.x + anchor.x / kMapWidthDm * map_width,
                                map_min.y + anchor.y / kMapHeightDm * map_height);
            DrawBuildingMarker(draw_list, center, anchor.label, hp, 10000,
                               anchor.red_side ? kRed : kBlue, control_color, shield != 0, scale);
        }
        for (const MapAnchor &anchor : kOutposts) {
            const bool own = anchor.red_side == we_red;
            const uint32_t hp = anchor.red_side ? unit.outpost_r_health() : unit.outpost_b_health();
            const uint32_t status = anchor.red_side ? unit.outpost_r_status() : unit.outpost_b_status();
            ImU32 control_color = kMuted;
            if (status == 1 || status == 2) {
                control_color = ours;
            } else if (status == 3 || status == 4) {
                control_color = theirs;
            }
            const ImVec2 center(map_min.x + anchor.x / kMapWidthDm * map_width,
                                map_min.y + anchor.y / kMapHeightDm * map_height);
            DrawBuildingMarker(draw_list, center, anchor.label, hp, 1000,
                               anchor.red_side ? kRed : kBlue, control_color, status != 0, scale);
            (void) own;
        }
    }

    // ---- 机器人位置 ----
    if (state.radar_info_to_client.has_value()) {
        const auto &radar = *state.radar_info_to_client;
        const std::vector<uint32_t> ids = RadarRobotIds(state, we_red);
        for (int i = 0; i < radar.robots_info_size() && i < static_cast<int>(ids.size()); ++i) {
            const auto &info = radar.robots_info(i);
            if (info.target_pos_x() == 0 && info.target_pos_y() == 0) {
                continue;   // 未定位
            }
            const uint32_t robot_id = ids[i];
            const ImU32 color = robot_id < 100 ? kRed : kBlue;
            const ImVec2 center(map_min.x + static_cast<float>(info.target_pos_x()) / kMapWidthDm * map_width,
                                map_min.y + static_cast<float>(info.target_pos_y()) / kMapHeightDm * map_height);
            const float radius = 10.0f * scale;

            if (robot_id / 100 == (we_red ? 0u : 1u)) {   // 己方单位加一圈淡白，便于一眼区分
                draw_list->AddCircle(center, radius + 3.0f * scale, IM_COL32(255, 255, 255, 45), 0, 1.5f * scale);
            }
            draw_list->AddCircleFilled(center, radius, Alpha(color, 70));
            draw_list->AddCircleFilled(center, radius - 3.0f * scale, IM_COL32(22, 26, 33, 255));
            draw_list->AddCircle(center, radius, color, 0, 2.0f * scale);
            if (info.is_high_light() > 0) {
                draw_list->AddCircle(center, radius + 5.0f * scale, kAmber, 0, 1.5f * scale);
            }

            char id_text[8];
            std::snprintf(id_text, sizeof(id_text), "%u", robot_id % 100);
            ImGui::PushFont(ImGui::GetFont(), 12);
            const ImVec2 id_size = ImGui::CalcTextSize(id_text);
            draw_list->AddText(ImVec2(center.x - id_size.x * 0.5f, center.y - id_size.y * 0.5f), kText, id_text);
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

    // ---- 本地标记点（6 秒保留 + 2 秒淡出） ----
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
            draw_list->AddCircleFilled(center, 4.0f * scale, Alpha(kAmber, a));
            draw_list->AddCircle(center, 8.0f * scale, Alpha(kAmber, a / 2), 0, 1.5f * scale);
            draw_list->AddLine(ImVec2(center.x - 12.0f * scale, center.y), ImVec2(center.x + 12.0f * scale, center.y),
                               Alpha(kAmber, a), 1.0f);
            draw_list->AddLine(ImVec2(center.x, center.y - 12.0f * scale), ImVec2(center.x, center.y + 12.0f * scale),
                               Alpha(kAmber, a), 1.0f);
            char text[48];
            std::snprintf(text, sizeof(text), "标记 %d,%d", static_cast<int>(g_local_marker.x),
                          static_cast<int>(g_local_marker.y));
            ImGui::PushFont(ImGui::GetFont(), 12);
            draw_list->AddText(ImVec2(center.x + 12.0f * scale, center.y - 6.0f * scale), Alpha(kAmber, a), text);
            ImGui::PopFont();
        }
    }

    // ---- 悬停十字线与实时坐标 ----
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
        draw_list->AddText(text_pos, kText, text);
        ImGui::PopFont();

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const float clamped_x = std::clamp(map_x, 0.0f, kMapWidthDm);
            const float clamped_y = std::clamp(map_y, 0.0f, kMapHeightDm);
            send_map_marker_now(mqtt_client, clamped_x, clamped_y, true);   // 群发含哨兵
            g_local_marker.valid = true;
            g_local_marker.x = clamped_x;
            g_local_marker.y = clamped_y;
            g_local_marker.time = std::chrono::steady_clock::now();
        }
    }

}   // ShowTacticalMap 结束

// ---------------- 数据对比 ----------------
void DrawVersusTile(ImDrawList *draw_list, ImVec2 pos, ImVec2 size, const char *label, uint32_t ally_value,
                    uint32_t enemy_value, ImU32 ally_color, ImU32 enemy_color) {
    const float scale = ImGui::GetScale();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    draw_list->AddRectFilled(pos, max, kCardBg, 6.0f * scale);
    draw_list->AddRect(pos, max, kCardBorder, 6.0f * scale, 1.0f, ImDrawFlags_None);
    draw_list->AddText(ImVec2(pos.x + 10.0f * scale, pos.y + 6.0f * scale), kMuted, label);

    ImGui::PushFont(ImGui::GetFont(), 22);
    char value_text[32];
    std::snprintf(value_text, sizeof(value_text), "%u", ally_value);
    draw_list->AddText(ImVec2(pos.x + 10.0f * scale, pos.y + 24.0f * scale), ally_color, value_text);
    ImGui::PopFont();

    char enemy_text[32];
    std::snprintf(enemy_text, sizeof(enemy_text), "%u", enemy_value);
    const ImVec2 enemy_size = ImGui::CalcTextSize(enemy_text);
    draw_list->AddText(ImVec2(max.x - 10.0f * scale - enemy_size.x, pos.y + 32.0f * scale), enemy_color, enemy_text);

    const ImVec2 bar_min(pos.x + 10.0f * scale, max.y - 10.0f * scale);
    const ImVec2 bar_max(max.x - 10.0f * scale, max.y - 6.0f * scale);
    draw_list->AddRectFilled(bar_min, bar_max, enemy_color, 2.0f * scale);
    const uint32_t total = ally_value + enemy_value;
    if (total > 0) {
        const float ally_ratio = static_cast<float>(ally_value) / static_cast<float>(total);
        draw_list->AddRectFilled(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * ally_ratio, bar_max.y),
                                 ally_color, 2.0f * scale);
    }
}   // DrawVersusTile 结束

void DrawStats(const State &state, ImVec2 pos, float width, float height) {
    const bool we_red = WeAreRed(state);
    const ImU32 ours = we_red ? kRed : kBlue;
    const ImU32 theirs = we_red ? kBlue : kRed;
    const auto &unit = state.global_unit_status;
    const uint32_t cp_ally = unit.has_value() ? unit->total_cp_ally() : 0;
    const uint32_t cp_enemy = unit.has_value() ? unit->total_cp_enemy() : 0;
    const uint32_t dmg_ally = unit.has_value() ? unit->total_damage_ally() : 0;
    const uint32_t dmg_enemy = unit.has_value() ? unit->total_damage_enemy() : 0;

    int ally_alive = 0;
    int enemy_alive = 0;
    for (const RobotSlotDef &slot : kRobotSlots) {
        const UnitHp ally = GetUnitHp(state, we_red ? slot.id : slot.id + 100);
        const UnitHp enemy = GetUnitHp(state, we_red ? slot.id + 100 : slot.id);
        if (!ally.known || ally.hp > 0) ++ally_alive;
        if (!enemy.known || enemy.hp > 0) ++enemy_alive;
    }

    const float scale = ImGui::GetScale();
    const float gap = 8.0f * scale;
    const float tile_width = (width - gap * 3.0f) / 4.0f;
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    DrawVersusTile(draw_list, pos, ImVec2(tile_width, height), "控制度", cp_ally, cp_enemy, ours, theirs);
    DrawVersusTile(draw_list, ImVec2(pos.x + tile_width + gap, pos.y), ImVec2(tile_width, height),
                   "总伤害", dmg_ally, dmg_enemy, ours, theirs);

    // 存活：数字用 "3 : 4"，不画比例条
    {
        const ImVec2 tile_pos(pos.x + (tile_width + gap) * 2.0f, pos.y);
        const ImVec2 tile_max(tile_pos.x + tile_width, tile_pos.y + height);
        draw_list->AddRectFilled(tile_pos, tile_max, kCardBg, 6.0f * scale);
        draw_list->AddRect(tile_pos, tile_max, kCardBorder, 6.0f * scale, 1.0f, ImDrawFlags_None);
        draw_list->AddText(ImVec2(tile_pos.x + 10.0f * scale, tile_pos.y + 6.0f * scale), kMuted, "存活");
        ImGui::PushFont(ImGui::GetFont(), 22);
        char text[32];
        std::snprintf(text, sizeof(text), "%d : %d", ally_alive, enemy_alive);
        draw_list->AddText(ImVec2(tile_pos.x + 10.0f * scale, tile_pos.y + 24.0f * scale), kText, text);
        ImGui::PopFont();
        draw_list->AddText(ImVec2(tile_pos.x + 10.0f * scale, tile_pos.y + height - 20.0f * scale), kMuted,
                           "我方 / 敌方");
    }

    // 经济差
    {
        const ImVec2 tile_pos(pos.x + (tile_width + gap) * 3.0f, pos.y);
        const ImVec2 tile_max(tile_pos.x + tile_width, tile_pos.y + height);
        draw_list->AddRectFilled(tile_pos, tile_max, kCardBg, 6.0f * scale);
        draw_list->AddRect(tile_pos, tile_max, kCardBorder, 6.0f * scale, 1.0f, ImDrawFlags_None);
        draw_list->AddText(ImVec2(tile_pos.x + 10.0f * scale, tile_pos.y + 6.0f * scale), kMuted, "剩余经济");
        const uint32_t gold = state.global_logistics_status.has_value()
                                      ? state.global_logistics_status->remaining_economy()
                                      : 0;
        ImGui::PushFont(ImGui::GetFont(), 22);
        char text[32];
        std::snprintf(text, sizeof(text), "%u", gold);
        draw_list->AddText(ImVec2(tile_pos.x + 10.0f * scale, tile_pos.y + 24.0f * scale), kAmber, text);
        ImGui::PopFont();
        const uint32_t level = state.global_logistics_status.has_value()
                                       ? state.global_logistics_status->tech_level()
                                       : 0;
        char sub[48];
        std::snprintf(sub, sizeof(sub), "科技等级 %u", level);
        draw_list->AddText(ImVec2(tile_pos.x + 10.0f * scale, tile_pos.y + height - 20.0f * scale), kMuted, sub);
    }
}   // DrawStats 结束

// ---------------- 战场事件 ----------------
void ShowEvents(const State &state, float height) {
    SectionTitle("战场事件", "最近 8 条", kAmber);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(kCardBg));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f * ImGui::GetScale());
    ImGui::BeginChild("##events", ImVec2(0.0f, height), true, ImGuiWindowFlags_None);

    std::vector<const LogEntry *> entries;
    entries.reserve(state.system_logs.size() + state.vehicle_logs.size());
    for (const LogEntry &entry : state.system_logs) entries.push_back(&entry);
    for (const LogEntry &entry : state.vehicle_logs) entries.push_back(&entry);
    std::ranges::sort(entries, [](const LogEntry *a, const LogEntry *b) { return a->sequence > b->sequence; });

    const size_t count = std::min<size_t>(entries.size(), 8);
    if (count == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
        ImGui::TextUnformatted("暂无事件");
        ImGui::PopStyleColor();
    }
    for (size_t i = 0; i < count; ++i) {
        const LogEntry &entry = *entries[i];
        const ImU32 color = entry.type == 2 ? kRed : (entry.type == 1 ? kAmber : kText);
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
        ImGui::TextUnformatted(entry.time);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 8.0f * ImGui::GetScale());
        ImGui::PushStyleColor(ImGuiCol_Text, Col(color));
        ImGui::PushTextWrapPos(0.0f);
        if (entry.repeat_count > 1) {
            ImGui::Text("%s (×%u)", entry.text.c_str(), entry.repeat_count);
        } else {
            ImGui::TextUnformatted(entry.text.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}   // ShowEvents 结束

// ---------------- 关键机制 ----------------
// 指挥手在比赛中真正需要盯的四个窗口：能量机关、飞镖闸门、哨兵姿态、堡垒占领。
// 这些信息原本散落在日志与各处角标里，这里压成四行常驻。
void ShowMechanisms(const State &state, float height) {
    const float scale = ImGui::GetScale();
    SectionTitle("关键机制", "", kPurple);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(kCardBg));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f * scale);
    ImGui::BeginChild("##mechanisms", ImVec2(0.0f, height), true, ImGuiWindowFlags_None);

    // 每项两行：小号灰标签 + 主色数值。窄栏里把标签和数值挤在一行必然折行，不如直接上下排。
    auto row = [](const char *label, const char *value, ImU32 color) {
        ImGui::PushFont(ImGui::GetFont(), 12);
        ImGui::PushStyleColor(ImGuiCol_Text, Col(kMuted));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, Col(color));
        ImGui::TextWrapped("%s", value);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(1.0f, 2.0f * ImGui::GetScale()));
    };

    // 能量机关（2.2.28）：状态 + 灯臂数 + 平均环数
    {
        char value[96];
        ImU32 color = kMuted;
        if (state.rune_status_sync.has_value()) {
            const auto &rune = *state.rune_status_sync;
            const char *status = rune.rune_status() == 1 ? "未激活" : rune.rune_status() == 2 ? "激活中" : "已激活";
            color = rune.rune_status() == 2 ? kAmber : (rune.rune_status() == 3 ? kGreen : kMuted);
            std::snprintf(value, sizeof(value), "%s · 臂 %u · %.1f 环",
                          status, rune.activated_arms(), static_cast<double>(rune.average_rings()));
        } else {
            std::snprintf(value, sizeof(value), "等待数据");
        }
        row("能量机关", value, color);
    }

    // 飞镖（2.2.31 + 2.2.29）：目标 + 闸门
    {
        static const char *kTargets[5] = {"前哨站", "基地固定", "基地随机固定", "基地随机移动", "基地末端移动"};
        char value[96];
        ImU32 color = kMuted;
        if (state.dart_select_target_status_sync.has_value()) {
            const auto &dart = *state.dart_select_target_status_sync;
            const uint32_t target = dart.target_id();
            const char *target_name = target >= 1 && target <= 5 ? kTargets[target - 1] : "未知目标";
            const char *gate = dart.open() == 2 ? "已开启" : dart.open() == 1 ? "开启中" : "已关闭";
            color = dart.open() == 2 ? kRed : kMuted;
            std::snprintf(value, sizeof(value), "%s · 闸门%s", target_name, gate);
        } else {
            std::snprintf(value, sizeof(value), "等待同步");
        }
        row("飞镖", value, color);
    }

    // 哨兵（2.2.30）
    {
        char value[96];
        ImU32 color = kMuted;
        if (state.sentry_status_sync.has_value()) {
            const auto &sentry = *state.sentry_status_sync;
            const char *posture = sentry.posture_id() == 1 ? "进攻" : sentry.posture_id() == 2 ? "防御"
                    : sentry.posture_id() == 3 ? "移动" : "未知";
            std::snprintf(value, sizeof(value), "%s%s%s", posture,
                          sentry.is_weakened() ? " · 弱化" : "",
                          sentry.is_powered() ? " · 强化" : "");
            color = sentry.is_weakened() ? kAmber : kGreen;
        } else {
            std::snprintf(value, sizeof(value), "等待同步");
        }
        row("哨兵", value, color);
    }

    // 堡垒占领计时（2.2.6）：最多两条
    {
        char value[128];
        value[0] = '\0';
        ImU32 color = kMuted;
        if (state.global_special_mechanism.has_value()) {
            const auto &mechanism = *state.global_special_mechanism;
            for (int i = 0; i < mechanism.mechanism_id_size(); ++i) {
                const uint32_t id = mechanism.mechanism_id(i);
                const int32_t seconds = i < mechanism.mechanism_time_sec_size() ? mechanism.mechanism_time_sec(i) : 0;
                if (id == 1) {
                    std::snprintf(value, sizeof(value), "己方堡垒 %s", FormatClock(seconds).c_str());
                    color = kRed;
                } else if (id == 2) {
                    std::snprintf(value, sizeof(value), "对方堡垒 %s", FormatClock(seconds).c_str());
                    color = kGreen;
                }
            }
        }
        if (value[0] == '\0') {
            std::snprintf(value, sizeof(value), "无占领计时");
        }
        row("堡垒", value, color);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}   // ShowMechanisms 结束

// ---------------- 中央栏 ----------------
void ShowCenterColumn(const State &state, RoboGo::io::MqttClient &mqtt_client,
                      ImTextureRef map_texture, const ImVec2 &map_texture_size, ImU32 accent) {
    const float scale = ImGui::GetScale();
    SectionTitle("战术地图", "点击地图 = 向全队（含哨兵）下发标记", accent);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float stats_height = 86.0f * scale;
    const float events_height = 170.0f * scale;
    const float gaps = 8.0f * scale * 2.0f;
    const float map_available_height = std::max(avail.y - stats_height - events_height - gaps, 120.0f * scale);
    float map_width = std::min(avail.x, map_available_height * (kMapWidthDm / kMapHeightDm));
    map_width = std::max(map_width, 120.0f * scale);
    const ImVec2 map_size(map_width, map_width * (kMapHeightDm / kMapWidthDm));

    // 地图水平居中，留一点边距
    const float offset_x = (avail.x - map_size.x) * 0.5f;
    if (offset_x > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset_x);
    }
    ShowTacticalMap(state, mqtt_client, map_texture, map_texture_size, map_size);
    AdvanceRow(8.0f * scale);

    const ImVec2 stats_pos = ImGui::GetCursorScreenPos();
    DrawStats(state, stats_pos, ImGui::GetContentRegionAvail().x, stats_height);
    AdvanceRow(stats_height + 10.0f * scale);

    // 底部：左边战场事件，右边关键机制。两栏并排，避免"只有一条事件"时整块留白。
    const float remaining = std::max(ImGui::GetContentRegionAvail().y, 120.0f * scale);
    const float center_width = ImGui::GetContentRegionAvail().x;
    const float mechanism_width = std::clamp(center_width * 0.42f, 210.0f * scale, 300.0f * scale);
    const float events_width = std::max(center_width - mechanism_width - 8.0f * scale, 160.0f * scale);

    ImGui::BeginChild("##bottom_left", ImVec2(events_width, remaining), false, ImGuiWindowFlags_None);
    ShowEvents(state, remaining - ImGui::GetFrameHeight() - 4.0f * scale);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 8.0f * scale);

    ImGui::BeginChild("##bottom_right", ImVec2(mechanism_width, remaining), false, ImGuiWindowFlags_None);
    ShowMechanisms(state, remaining - ImGui::GetFrameHeight() - 4.0f * scale);
    ImGui::EndChild();
}   // ShowCenterColumn 结束

}   // namespace

// 功能：指挥台总入口。详见头文件。
void ShowCommanderPanel(const State &state,
                        RoboGo::io::MqttClient &mqtt_client,
                        ImTextureRef video_texture_ref,
                        const ImVec2 &video_size,
                        bool video_ready,
                        ImTextureRef map_texture_ref,
                        const ImVec2 &map_texture_size) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float scale = ImGui::GetScale();
    const bool we_red = WeAreRed(state);
    const ImU32 ours = we_red ? kRed : kBlue;
    const ImU32 theirs = we_red ? kBlue : kRed;

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

    const float header_height = 58.0f * scale;
    DrawHeader(state, mqtt_client, viewport->Pos, ImVec2(viewport->Size.x, header_height));

    const float pad = 10.0f * scale;
    // 再减去 4px：子窗口边框 + 行间距会让内容比理论值高一点，不减就会顶出一条滚动条。
    const float body_height = std::max(viewport->Size.y - header_height - pad * 2.0f - 4.0f * scale,
                                       200.0f * scale);
    const float left_width = std::clamp(viewport->Size.x * 0.225f, 250.0f * scale, 340.0f * scale);
    const float right_width = std::clamp(viewport->Size.x * 0.26f, 280.0f * scale, 390.0f * scale);
    const float center_width = std::max(viewport->Size.x - left_width - right_width - pad * 4.0f, 200.0f * scale);

    // 三个栏共用一套子窗口样式
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(kPanelBg));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(kCardBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 8.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 3.0f * scale));

    ImGui::SetCursorScreenPos(ImVec2(viewport->Pos.x + pad, viewport->Pos.y + header_height + pad));

    // ----- 左栏：我方阵容 + 我方建筑 + 资源 + 现场画面 -----
    ImGui::BeginChild("CommanderLeft", ImVec2(left_width, body_height), true, ImGuiWindowFlags_None);
    ShowRoster(state, "我方机器人", true, ours);
    ImGui::Spacing();
    ShowBuildings(state, "我方建筑", OurBuildings(state, we_red, ours, theirs), ours);
    ImGui::Spacing();
    ShowResources(state, ours);
    ImGui::Spacing();
    ShowVideoPreview(video_texture_ref, video_size, video_ready, ours);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, pad);

    // ----- 中栏：战术地图 + 数据对比 + 事件 -----
    ImGui::BeginChild("CommanderCenter", ImVec2(center_width, body_height), true, ImGuiWindowFlags_None);
    ShowCenterColumn(state, mqtt_client, map_texture_ref, map_texture_size, ours);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, pad);

    // ----- 右栏：敌方阵容 + 敌方建筑 + 指挥指令 -----
    ImGui::BeginChild("CommanderRight", ImVec2(right_width, body_height), true,
                      ImGuiWindowFlags_None);
    ShowRoster(state, "敌方机器人", false, theirs);
    ImGui::Spacing();
    ShowBuildings(state, "敌方建筑", EnemyBuildings(state, we_red, ours, theirs), theirs);
    ImGui::Spacing();
    ShowCommandDeck(state, mqtt_client, kPurple);
    ImGui::EndChild();

    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}   // ShowCommanderPanel 结束
