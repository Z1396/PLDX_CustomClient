// commander_panel.cpp：指挥手全屏综合指挥台的实现。
//
// 用 ImGui 自绘「一条顶栏 + 左/中/右三栏」的全屏指挥视图，并承载两类交互：
// 1) 战术地图点击 → 下发 MapClickCmd 标记给全队（含哨兵）；
// 2) 右栏指令区 → 下发哨兵控制 / 飞镖选靶与发射指令。
// 数据流：State 是主线程每帧由 MQTT 下行消息刷新的只读快照，本文件所有 Draw* 函数只读它；
// 交互经 io::SendSentryCtrlCommand / SendDartCommand / send_map_marker_now
// → MqttClient::publish → 裁判系统。文件不持有跨帧业务状态，只有两个 UI 态：
// g_active_tab（当前页签）与 g_local_marker（点击后 8 秒淡出的本地标记）。
// 设计要点：
// · 全部走 ImDrawList 直接绘制，布局由 ComputeLayout 按 ImGui::GetScale() 一次算好，
// 所有尺寸以 1280×800 设计稿为基准乘 s，保证高 DPI 下不糊不越界；
// · 数据缺失时显示「未知 / 等待数据」而非臆造数值，所有 optional 都先 has_value() 再解引用；
// · 阵营统一走 team_util::WeAreRed / IsRedRobotId。注意协议里两类字段语义不同：
// base_health / enemy_base_* 是「己方 / 对方」相对语义，必须按阵营交换；
// outpost_r_* / outpost_b_* 是绝对红/蓝前哨，不能交换。
// · 战术地图沿用项目原设计（场地 280×150 dm 坐标），本文件只把纹理、雷达点与建筑标记画上去。
// 依赖：ImGui（imgui_internal.h 提供 GetScale）、messages.pb.h、ui/theme.h、
// ui/hud_primitives.h、ui/format_utils.h、app/robot_registry.h、app/team_util.h、
// app/match_display.h、io/command_sender.h、keyboard_mouse_state_manager.h。
// 注意：本文件只读 State；需要改变比赛状态时必须回到 MQTT handler 或主线程，不要在这里写回。
// 2027-11-06 按「综合指挥台视觉设计稿 v1」实装：沿用主 HUD 定稿的视觉令牌
// （深色中性底 / 14px 圆角 / 低对比描边 / 等宽数字 / 6 级字阶），
// 把三个带灰底的方块子窗口换成透明三栏，一条连续顶栏贯穿全宽。
// · 双方阵容：宽卡（在线 / 弹量 / 百分比三连）→ 2×2 大数值单位块 + 兵种名；
// · 建筑：46px 卡片 → 22px 细行；
// · 四张 KPI 方卡 → 一行指标卡 + 双色控制度带；
// · 事件流：带标题灰面板 → 透明无标题行；
// · 指令区：高饱和实心按钮 → 分层级幽灵按钮；
// · 战术地图：绘制与交互完全沿用项目原设计，未改动。
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

// 三种指挥模式各自的视觉主题：切换页签时，页签 / 色带 / 指令按钮 / 回执条一起换色，
// 避免「哨兵 / 飞镖 / 空中」看起来都是同一套灰色按钮。颜色沿用主 HUD 的低饱和风格。
struct ModeTheme {
    const char *name;   // 中文模式名
    const char *code;   // 英文代号（显示在分区标题右侧）
    ImU32 accent;       // 该模式的强调色
};
constexpr ModeTheme kModeThemes[3] = {
        {"哨兵", "SENTRY", kPurple},                        // 紫：地面单位指挥
        {"飞镖", "DART", IM_COL32(112, 190, 214, 255)},     // 青：远程投射
        {"空中", "AIR", IM_COL32(146, 202, 160, 255)},      // 绿：空中监视
};
int g_active_tab = 0;   // 当前模式 0 哨兵 / 1 飞镖 / 2 空中；前移至此供按钮原语读取主题色

/**
 * @brief 取本文件使用的统一色板。
 * @note 思路：直接复用 hud_primitives.h 的 MakeColors()，保证指挥台与主 HUD 配色同源。
 * @return Colors 值类型（12 个 ImU32 色值，按值返回无所有权问题）。
 */
Colors MakeColors() {
    return hud::MakeColors();
}

/**
 * @brief 把 ImU32 打包色转成 ImGui 需要的 ImVec4 浮点色。
 * @param color ABGR 打包的 32 位色值。
 * @return 可直接传给 PushStyleColor 的 ImVec4。
 */
ImVec4 Col(ImU32 color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

/**
 * @brief 替换颜色的 alpha 通道，保留 RGB。
 * @param color 原色；alpha —— 0~255 的新不透明度。
 * @return 改过 alpha 的 ImU32。用于徽标底色/描边的分层弱化。
 */
ImU32 Alpha(ImU32 color, int alpha) {
    return (color & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24);
}

// ---------------- 布局常量（设计稿 1280×800 基准，随 DPI 缩放） ----------------
constexpr float kPad = 24.0f;
constexpr float kTopY = 18.0f;
constexpr float kTopH = 48.0f;
constexpr float kColTop = 82.0f;
constexpr float kColBottom = 24.0f;
// 侧栏宽度按视口宽度自适应：大屏放宽以吃掉中栏两侧的空带，小屏保底不挤占中栏。
constexpr float kSideRatio = 0.20f;   // 侧栏目标宽度 = 视口宽 × 该比例
constexpr float kSideMinW = 260.0f;   // 保底：再窄指令按钮 / 链路卡会挤
constexpr float kSideMaxW = 400.0f;   // 上限：再宽就是浪费
constexpr float kColGap = 12.0f;
constexpr float kSectH = 26.0f;
constexpr float kUnitH = 52.0f;
constexpr float kUnitGap = 8.0f;
constexpr float kBlockGap = 14.0f;
constexpr float kBuildH = 22.0f;
constexpr float kKvH = 20.0f;
constexpr float kCardRadius = 14.0f;

// 全屏三栏布局的一次性计算结果。
// 思路：1280×800 设计稿里的每个常量都乘 ImGui::GetScale()，在帧首算好后传给所有 Draw*，
//       避免各函数重复计算导致 DPI 下错位。字段分三组：视口原点/尺寸、顶栏、左右中三栏。
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

/**
 * @brief 按当前视口与 DPI 缩放算出顶栏和三栏的矩形。
 * @note 思路：左右栏按视口宽的 20% 自适应（夹在 260~400*s），中栏取剩余宽度并保底 240*s；列高扣掉上下边距并保底 320*s，保证窗口被拉得很小时各栏不会算出负数或重叠。
 *        侧栏加宽会同步压窄中栏，战术地图与 KPI 框因此一起变窄，不必在地图两侧留空带。
 * @note 参数：viewport_pos/size —— 主视口位置与尺寸；s —— ImGui::GetScale()。
 * @return 值类型 Layout，调用方按值持有，无生命周期顾虑。
 */
Layout ComputeLayout(const ImVec2 &viewport_pos, const ImVec2 &viewport_size, float s) {
    const float pad = kPad * s;
    const float side_w = std::clamp(viewport_size.x * kSideRatio, kSideMinW * s, kSideMaxW * s);
    const float left_w = side_w;
    const float right_w = side_w;
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

/**
 * @brief 把「相对视口左上角」的设计坐标转成屏幕绝对坐标。
 * @param layout 帧首算好的布局；x/y —— 设计稿坐标（已乘过 s）。
 * @return 可直接传给 AddText / AddRect 的 ImVec2。
 */
ImVec2 At(const Layout &layout, float x, float y) {
    return ImVec2(layout.origin.x + x, layout.origin.y + y);
}

/**
 * @brief 画一张圆角卡片（填充 + 0.5px 低对比描边）。
 * @note ---------------- 绘制原语 ----------------
 * @note 思路：抽成原语是为了顶栏、KPI、机制卡、链路卡共用同一种「深色面 + 淡描边」质感。
 * @note 参数：p/q —— 左上/右下角；radius —— 圆角半径；fill —— 填充色。
 */
void Card(ImDrawList *dl, ImVec2 p, ImVec2 q, const Colors &c, float radius, ImU32 fill) {
    dl->AddRectFilled(p, q, fill, radius);
    dl->AddRect(p, q, c.border, radius, 0.5f);
}

// 分区小标题：无彩色竖条，右侧是灰色的计数/说明。
// 参数：title —— 左对齐标题；right —— 右对齐补充文字，nullptr 或空串则不画。
// 思路：不用彩色竖条，靠 11.5px 字号与 text2/dim 的对比分层，保持低干扰。
void SectionLabel(ImDrawList *dl, ImVec2 p, float width, const Colors &c,
                  const char *title, const char *right) {
    AddText(dl, p, c.text2, title, 11.5f);
    if (right != nullptr && right[0] != '\0') {
        AddTextRightAligned(dl, p.x + width, p.y + 1.0f, c.dim, right, 10.5f);
    }
}

// 单位块：兵种名（主）+ 17px 血量（右）+ 底部血条；本车高亮、低血琥珀、阵亡弱化。
// 思路：把「已知/未知」「是否本车」等派生状态提前算好，UnitBlock 只管画，不再碰 State。
struct UnitVisual {
    const char *label;
    uint32_t hp;
    uint32_t max_hp;
    bool known;
    bool mine;
};

/**
 * @brief 决定单位强调色（血条/数值）。
 * @note 思路：阵亡弱化 → 低于 40% 琥珀警示 → 其余用阵营色；未知数据时保持阵营色，不误报低血。
 * @return ImU32 颜色。
 */
ImU32 UnitAccent(const UnitVisual &unit, const Colors &c, ImU32 team_color) {
    if (unit.known && unit.hp == 0) return WithAlpha(c.dim, 170);
    if (unit.known && RatioOf(unit.hp, unit.max_hp) < 0.4f) return c.amber;
    return team_color;
}

/**
 * @brief 画一个单位块（兵种名 + 大号血量 + 底部血条）。
 * @param p 左上角；width —— 块宽；team_color —— 红/蓝阵营色；s —— DPI 缩放。
 * @note 思路：本车用 kMineBg 底色 + 顶部一条阵营亮线；阵亡降对比；低血由 UnitAccent 给琥珀色。未知数据（!known）显示「—」且不画填充血条，避免把 0 误读成阵亡。
 */
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
            dead ? c.dim : c.text2, unit.label, 10.5f);

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

/**
 * @brief 画一行建筑状态（标签 / 细血条 / 数值）。
 * @param mirror 是否镜像；team_color —— 该侧阵营色。
 * @note 思路：mirror=true 为敌方栏镜像布局（数值在左、标签在右，血条从右往左填充），使左右两栏沿中轴视觉对称；血量满值来自调用方传入的 1000(前哨)/10000(基地)
 *        。
 */
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
// 参数：p —— 左上角；color —— 徽标主色；text —— 短文案。
// 返回值：徽标实际占用的宽度，供调用方横向排下一个徽标。
// 思路：底色/描边分别取 alpha=22/55，不与卡片抢视觉重心。
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
// 参数：value_color 为 0 时用 c.text2，否则用指定色；s 为 DPI 缩放。
void KvRow(ImDrawList *dl, ImVec2 p, float width, const Colors &c,
           const char *label, const char *value, float s, ImU32 value_color = 0) {
    AddText(dl, ImVec2(p.x, p.y + 4.0f * s), c.dim, label, 11);
    AddTextRightAligned(dl, p.x + width, p.y + 3.0f * s,
                        value_color == 0 ? c.text2 : value_color, value, 11.5f);
}

// ---------------- 数据快照 ----------------
// 协议血量到本地数组的一层只读快照。
// known 由 max_health>0 推出：协议未下发时 hp/max_hp 都是 0，必须与「真阵亡」区分开。
struct UnitHp {
    uint32_t hp = 0;
    uint32_t max_hp = 0;
    bool known = false;
};

/**
 * @brief 按完整 robot_id（1/101/7/107…）取该机器人的血量快照。
 * @param state 只读全局状态；robot_id —— 完整编号（含百位阵营）。
 * @note 思路：robot_id%100 查 robot_registry::kRobotTypeToSlot 得到本地数组下标，再由 IsRedRobotId 选 red_robots/blue_robots。
 *        映射表与 main_window 的入站映射同源，避免两处各自硬编码兵种号导致漂移；空中/飞镖/雷达等无血量单位返回空快照。
 * @return UnitHp{ hp, max_hp, known }。
 */
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

// 一个单位槽位的静态描述：本地数组下标、协议编号、界面兵种名。
struct RobotSlotDef {
    int array_index;
    int id;
    const char *label;
};

// 与主 HUD 的 kHudRobotLabels 一致：兵种名是主标签，编号只在角标里保留。
// 思路：兵种名作主标签、编号留在角标里；这里存 array_index 是为了与 GetUnitHp 的
//       kRobotTypeToSlot 对齐，避免用 id-1 直接下标（7 号哨兵实际在下标 6）。
constexpr RobotSlotDef kRobotSlots[4] = {
        {0, 1, "1 重装"},
        {2, 3, "3 步兵3"},
        {3, 4, "4 步兵4"},
        {6, 7, "7 哨兵"},
};

// 建筑行的完整显示数据（含控制状态文案与附加说明）。
struct BuildingInfo {
    const char *label;
    uint32_t hp;
    uint32_t max_hp;
    const char *control;
    ImU32 control_color;
    const char *extra;
};

/**
 * @brief 把前哨站的 status 码翻成「我方/敌方 完全/临时」并选好颜色。
 * @param hp 当前血量；status —— 控制状态码；ours/theirs —— 双方强调色。
 * @note 思路：status 1~4 是协议 2.2.13 的控制状态，未知值保留「未知」+ 中性色，不把 0 误显示成某方控制。ours/theirs 按当前阵营传入，保证文案始终是「我方」视角。
 * @return 可直接交给 BuildingRow 的 BuildingInfo。
 */
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

/**
 * @brief 把基地护罩/状态翻成控制文案与护甲说明。
 * @note 思路：own_base 决定 shield=1 是「我方完全」还是「敌方完全」——这是相对语义字段，与前哨站的绝对红蓝字段不同，必须按视角换算；base_status 0/1/2
 *        分别对应100% 防御增益 / 护甲未展开 / 已展开。
 * @return 含 extra（护甲说明）的 BuildingInfo。
 */
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

/**
 * @brief 组装「我方基地 + 我方前哨」两行数据。
 * @note 思路：GlobalUnitStatus 未到时返回两条占位（血量 0、控制「未知」），界面照常绘制不塌陷；己方前哨按 we_red 选 outpost_r_ 或 outpost_b_ 字段（绝对红蓝字段，
 *        不能交换）。
 */
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

/**
 * @brief 组装「敌方基地 + 敌方前哨」两行数据，是 OurBuildings 的镜像版本。
 * @note 思路：base 系列是相对语义，直接用 enemy_base_*；前哨站按 we_red 取另一侧绝对字段。
 */
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
// 指令区按钮的五种视觉层级：普通幽灵 / 主操作 / 危险操作 / 页签选中 / 页签未选中。
// 思路：用枚举把配色表集中在 DeckButton 里，调用点只表达语义不写颜色值。
enum class ButtonKind { Ghost, Primary, Danger, TabOn, TabOff };

/**
 * @brief 在绝对坐标处画一个按钮并返回本帧是否被点击。
 * @note 思路：按 ButtonKind 选 fill/border/text/hover/active 五组色，临时 Push 5 个 StyleColor + 2 个 StyleVar
 *        后立刻成对 Pop；用 SetCursorScreenPos 定位，不依赖 ImGui 布局流。enabled=false 时包 BeginDisabled，仍可显示 tooltip
 *        解释为什么不可用。
 * @note 参数：p/size —— 屏幕绝对位置与尺寸；tooltip —— 悬停说明（可空）。
 * @return true 表示本帧点击。
 */
bool DeckButton(const char *label, ImVec2 p, ImVec2 size, ButtonKind kind,
                bool enabled = true, const char *tooltip = nullptr, ImU32 accent = 0) {
    namespace ui = RoboGo::ui;
    ImU32 fill = WithAlpha(0xFFFFFFFFu, 7);
    ImU32 border = kLine;
    ImU32 text = ui::kTextSecondary;
    ImU32 hover = WithAlpha(0xFFFFFFFFu, 34);
    ImU32 active = WithAlpha(0xFFFFFFFFu, 55);
    // accent 非 0 时表示「当前指挥模式的主题色」：主操作 / 选中态换成该色，
    // 普通指令按钮只让描边与悬停带一点色相；危险操作保持红色不变（语义优先）。
    switch (kind) {
        case ButtonKind::Primary: {
            const ImU32 base = accent ? accent : ui::kAccent;
            fill = Alpha(base, 45); border = Alpha(base, 90); text = ui::Tint(base, 0.3f, 0.45f);
            hover = Alpha(base, 80); active = Alpha(base, 120); break;
        }
        case ButtonKind::Danger:
            fill = Alpha(ui::kTeamRed, 33); border = Alpha(ui::kTeamRed, 78); text = ui::Tint(ui::kTeamRed, 0.3f, 0.5f);
            hover = Alpha(ui::kTeamRed, 65); active = Alpha(ui::kTeamRed, 105); break;
        case ButtonKind::TabOn: {
            const ImU32 base = accent ? accent : kPurple;
            fill = Alpha(base, 40); border = Alpha(base, 72); text = ui::Tint(base, 0.3f, 0.4f);
            hover = Alpha(base, 78); active = Alpha(base, 110); break;
        }
        case ButtonKind::TabOff:
            fill = WithAlpha(0xFFFFFFFFu, 4); border = kLine; text = ui::kTextTertiary;
            hover = WithAlpha(0xFFFFFFFFu, 22); active = WithAlpha(0xFFFFFFFFu, 40); break;
        default:  // Ghost：普通指令按钮带一点当前模式色，三个模式不再都是同一片灰
            if (accent) { border = Alpha(accent, 34); hover = Alpha(accent, 26); }
            break;
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
// 思路：把「列数/行/列/跨列」换算成像素矩形，让指令区各页签复用同一套排布；
//       col_span 允许最后一行合并列（如「金币复活」占两列）。
// 参数：columns/row/col/col_span —— 网格参数；on_click —— 点击回调（std::function）。
void GridButton(const char *label, ImVec2 origin, float width, float row_h, float gap,
                int columns, int row, int col, int col_span, ButtonKind kind,
                bool enabled, const char *tooltip,
                const std::function<void()> &on_click) {
    const float cell_w = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const float w = cell_w * static_cast<float>(col_span) + gap * static_cast<float>(col_span - 1);
    const ImVec2 p(origin.x + (cell_w + gap) * static_cast<float>(col),
                  origin.y + (row_h + gap) * static_cast<float>(row));
    // 网格按钮统一取当前模式主题色，因此三个 Tab 函数无需改签名或逐处传参。
    if (DeckButton(label, p, ImVec2(w, row_h), kind, enabled, tooltip,
                   kModeThemes[g_active_tab].accent)) {
        on_click();
    }
}

/**
 * @brief 画贯穿全宽的顶栏（身份 / 比分与阶段 / 经济 / 链路 / 返回）。
 * @param mqtt_client 用于读连接状态与退出登录；所有其它数据只读 State。
 * @note ---------------- 顶栏 ----------------
 * @note 思路：左侧身份按「是否指挥手 → 本车 ID → 未知」三档取文案；中间比分与倒计时取GameStatus；右侧按 MqttClient::get_connection_state()
 *        原子读链路状态并显示圆点；「返回」按钮对指挥手会额外清 client_id 退出登录，普通身份只退出指挥台视图。
 */
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
    AddText(dl, ImVec2(badge_x + 29.0f * s, p.y + 27.0f * s), c.dim, subtitle, 10.5f);

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
    AddTextCenteredIn(dl, cx - clock_w * 0.5f, cx + clock_w * 0.5f, p.y + 30.0f * s, c.dim, stage, 10.5f);
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
    const float link_w = TextSize(link_text, 11).x + 14.0f * s;
    const float link_x = return_x - 16.0f * s - link_w;
    dl->AddCircleFilled(ImVec2(link_x + 3.0f * s, p.y + 24.0f * s), 2.5f * s, connected ? c.green : c.red);
    AddText(dl, ImVec2(link_x + 11.0f * s, p.y + 18.0f * s), c.text2, link_text, 11);
    const float econ_w = TextSize(gold_text, 14).x + TextSize("剩余经济", 10.5f).x + 10.0f * s;
    const float econ_x = link_x - 20.0f * s - econ_w;
    AddText(dl, ImVec2(econ_x, p.y + 15.0f * s), c.gold, gold_text, 14);
    AddText(dl, ImVec2(econ_x + TextSize(gold_text, 14).x + 5.0f * s, p.y + 20.0f * s),
            c.dim, "剩余经济", 10.5f);
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

/**
 * @brief 画一侧阵容（2×2 单位块 + 「N / 4 在线」计数）。
 * @note ---------------- 左栏：我方阵容 / 建筑 / 资源 / 现场画面 ----------------
 * @note 思路：ally 与 we_red 共同决定取红号还是蓝号；本车只在己方栏高亮。未知数据的单位计入「在线」（未收到数据不等于阵亡），避免开局显示 0/4。
 */
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

/**
 * @brief 画两行建筑 + 一行控制状态徽标，并返回内容底部 y 坐标。
 * @note 思路：返回值供调用方接着排下一个分区，避免在 DrawLeft/RightColumn 里重复累加高度；只展示基地的阵地状态徽标（break），防止两行徽标挤在一起。
 */
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

/**
 * @brief 画「经济 / 科技 / 加密」与堡垒、能量机关两行摘要。
 * @note 思路：全部字段先判 has_value()；mechanism_id 1/2 分别代表己方/对方堡垒被占，只取文案不展开倒计时（由中栏「关键机制」负责显示具体时间）。
 */
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
    AddText(dl, ImVec2(p.x, p.y + kSectH * s + 3.0f * s), c.text2, line1, 11.5f);

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
    AddText(dl, ImVec2(p.x, p.y + (kSectH + kKvH) * s + 3.0f * s), c.dim, line2, 11);
}

// 现场画面：有图传时等比贴纹理；无图传时保留深色配色 + 取景环 + 四角框。
// 思路：有有效帧时按纹理与目标的宽高比算 UV 裁剪（letterbox 思路，居中裁切而非拉伸），
//       无信号时画深色底 + 取景环 + 四角框，保证控件位置始终可见。
// 参数：ready —— 由 IsVideoDisplayReady 判定的「纹理有效且 250ms 内刷新过」。
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
            has_frame ? "UDP 图传 · 已连接" : "UDP 图传 · 无信号", 10.5f);
}

/**
 * @brief 串起左栏四个分区（我方阵容 → 我方建筑 → 资源与机制 → 现场画面）。
 * @note 思路：用相对 y 累加定位， preview 高度取「剩余列高」与 170*s 的较大者，小窗口下宁可让内容溢出也不把图传压成一条缝。
 */
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

/**
 * @brief 画中栏 KPI 卡（控制度 / 总伤害 / 存活 / 剩余经济 + 双色控制度带）。
 * @note ---------------- 中栏：KPI / 事件 / 关键机制 ----------------
 * @note 思路：四个大数值横排，底部一条带按 cp_ally : cp_enemy 的比例分色；双方都为 0 时比例取 0.5，避免开局整条带变成对方色。存活数用 GetUnitHp 的known
 *        判定，未同步的单位按存活计。
 */
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
        AddText(dl, ImVec2(x, p.y + 14.0f * s), c.dim, keys[i], 10.5f);
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

/**
 * @brief 画透明事件流（最多 8 行）。
 * @note 思路：把 system_logs 与 vehicle_logs 的指针收集到一个 vector，按 sequence 倒序排序后逐行画；不拷贝日志内容，且用 y+row_h>max_y
 *        提前 break，防止小窗口越界。错误级别用红色、警告用琥珀色，重复次数>1 追加 (×N)。
 */
void DrawEvents(const State &state, ImDrawList *dl, ImVec2 p, float width, float height,
                const Colors &c, float s) {
    std::vector<const LogEntry *> entries;
    entries.reserve(state.system_logs.size() + state.vehicle_logs.size());
    for (const LogEntry &entry : state.system_logs) entries.push_back(&entry);
    for (const LogEntry &entry : state.vehicle_logs) entries.push_back(&entry);
    std::ranges::sort(entries, [](const LogEntry *a, const LogEntry *b) { return a->sequence > b->sequence; });

    if (entries.empty()) {
        AddText(dl, ImVec2(p.x, p.y + 6.0f * s), c.dim, "暂无事件", 11.5f);
        return;
    }
    const size_t count = std::min<size_t>(entries.size(), 8);
    const float row_h = 27.0f * s;
    const float max_y = p.y + height;
    for (size_t i = 0; i < count; ++i) {
        const float y = p.y + static_cast<float>(i) * row_h;
        if (y + row_h > max_y) break;
        const LogEntry &entry = *entries[i];
        const ImU32 mark = entry.type == 2 ? c.red : (entry.type == 1 ? c.amber : WithAlpha(0xFFFFFFFFu, 46));
        const ImU32 text_color = entry.type == 2 ? IM_COL32(232, 188, 188, 255)
                                : entry.type == 1 ? IM_COL32(220, 187, 150, 255) : c.text2;
        dl->AddRectFilled(ImVec2(p.x, y + 3.0f * s), ImVec2(p.x + 3.0f * s, y + 15.0f * s), mark, 2.0f * s);
        AddText(dl, ImVec2(p.x + 12.0f * s, y + 2.0f * s), c.dim, entry.time, 10.5f);
        char text[256];
        if (entry.repeat_count > 1) {
            std::snprintf(text, sizeof(text), "%s (×%u)", entry.text.c_str(), entry.repeat_count);
        } else {
            std::snprintf(text, sizeof(text), "%s", entry.text.c_str());
        }
        AddText(dl, ImVec2(p.x + 84.0f * s, y + 1.0f * s), text_color, text, 11.5f);
    }
}

/**
 * @brief 画「关键机制」卡（能量机关 / 堡垒 / 飞镖闸门 / 前哨 / 哨兵 / 护甲 …）。
 * @note 思路：每个字段先判 optional 再取，缺数据统一显示「等待数据」；底部说明只在剩余高度足够时才画，避免小窗口压住最后一行状态。
 */
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
    // 高度不足时切成「4 行 × 2 列」：地图占满宽度后下方只剩约 150px，单列会丢掉后半状态。
    // 高度充足时保持原来的单列 8 行，观感不变；列宽由调用方在 compact 模式下加宽。
    const bool two_columns = height < 220.0f * s;
    const int rows = two_columns ? 4 : 8;
    const float row_h = (two_columns ? 19.0f : 20.0f) * s;
    const float inner_w = width - 28.0f * s;
    const float col_gap = 12.0f * s;
    const float col_w = two_columns ? (inner_w - col_gap) * 0.5f : inner_w;
    const float top = p.y + 34.0f * s;
    for (int i = 0; i < 8; ++i) {
        const int col = two_columns ? i / rows : 0;
        const int row = two_columns ? i % rows : i;
        const float x = p.x + 14.0f * s + static_cast<float>(col) * (col_w + col_gap);
        const float y = top + static_cast<float>(row) * row_h;
        if (y + row_h > q.y - 30.0f * s) continue;  // 单项越界只跳过自己，不阻断另一列
        KvRow(dl, ImVec2(x, y), col_w, c, labels[i], values[i], s);
    }
    // 说明只在剩余高度够时画，避免小窗口里压住最后一行状态。
    const float note_y = top + static_cast<float>(rows) * row_h + 6.0f * s;
    if (note_y + 30.0f * s <= q.y - 6.0f * s) {
        AddText(dl, ImVec2(p.x + 14.0f * s, note_y), c.dim,
                "指挥手下发哨兵指令会覆盖自动决策；", 10.5f);
        AddText(dl, ImVec2(p.x + 14.0f * s, note_y + 15.0f * s), c.dim,
                "每次干预扣除全队金币。", 10.5f);
    }
}

// ---------------- 战术地图（项目原设计，绘制与交互完全保留） ----------------
constexpr float kMapWidthDm = 280.0f;
constexpr float kMapHeightDm = 150.0f;

// 战术地图上一个建筑锚点：场地 dm 坐标 + 标签 + 所属阵营。
struct MapAnchor {
    float x;
    float y;
    const char *label;
    bool red_side;
};
// 两个基地在 280×150 dm 场地里的固定坐标（沿用项目原设计，勿按视觉稿改动）。
constexpr MapAnchor kBases[2] = {{22.0f, 27.0f, "红基地", true}, {256.0f, 122.0f, "蓝基地", false}};
// 两个前哨站的固定坐标；与 kBases 同属绝对场地坐标，不随己方阵营翻转。
constexpr MapAnchor kOutposts[2] = {{29.0f, 78.0f, "红前哨", true}, {249.0f, 76.0f, "蓝前哨", false}};

/**
 * @brief 返回小地图底图的 UV 范围。
 * @note 思路：当前底图是完整场地俯视图，直接给 (0,0)-(1,1)；保留 texture_size 形参是为了将来若底图带留白，可在这里统一裁切而不必改所有绘制调用点。
 */
std::pair<ImVec2, ImVec2> MapUvRect(const ImVec2 &texture_size) {
    (void) texture_size;
    return {ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f)};
}

// 本地点击标记的瞬时状态：坐标 + 时间戳；valid=false 表示当前没有标记。
struct LocalMarker {
    bool valid = false;
    float x = 0.0f;
    float y = 0.0f;
    std::chrono::steady_clock::time_point time{};
};
LocalMarker g_local_marker;

/**
 * @brief 在地图上画一个菱形建筑标记（血条百分比 + 控制状态点 + 名称）。
 * @note 思路：hp==0 额外画一个叉表示已摧毁；has_control=true 时中心点用 control_color；文字用 PushFont/PopFont 临时切到 12px，
 *        避免影响调用方字体状态。
 */
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

/**
 * @brief 绘制战术地图并处理左键点击下发标记（本文件唯一的地图交互入口）。
 * @note 思路：InvisibleButton 撑出交互区 → 依次叠底图/暗化层/地形叠加层(M 键开关)/网格 →画基地与前哨标记 → 按 RadarRobotIds 顺序画雷达单位圆点（含血条与高亮圈）
 *        →画 8 秒渐隐的本地标记 → 悬停时显示十字准线与 dm 坐标，点击则 clamp 到场地范围后调 send_map_marker_now 并记本地标记。
 * @note 注意：所有坐标换算统一除以 kMapWidthDm/kMapHeightDm，服务端收到的是 dm 坐标。
 */
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

// g_active_tab 与 kModeThemes 已前移到配色区（DeckButton / GridButton 需要读取当前模式色）。

/**
 * @brief 画「哨兵」页签的 3×3 指令网格 + 确认/金币复活。
 * @note 思路：按钮的 label 与协议 command_id 一一对应，点击直接 SendSentryCtrlCommand；用 GridButton 的 col_span 让最后一行「金币复活」占两列。
 */
void DrawSentryTab(RoboGo::io::MqttClient &mqtt_client, ImDrawList *dl, ImVec2 p, float width,
                   const Colors &c, float s) {
    const float row_h = 29.0f * s;
    const float gap = 6.0f * s;
    // 一条哨兵指令：界面文案 + 协议 command_id（见 SentryCtrlCommand）。
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

/**
 * @brief 画「飞镖」页签（5 个选靶 + 闸门开关 + 确认发射）。
 * @note 思路：target_id / gate_open 是函数内 static 的本地 UI 态，若服务端下发了DartSelectTargetStatusSync 则以服务端为准覆盖；
 *        「确认发射」仅在闸门完全开启(open==2) 时可用，未开启时 disabled + tooltip 说明原因。
 */
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
    AddText(dl, ImVec2(p.x + 2.0f * s, p.y + (row_h + gap) * 3.0f + 8.0f * s), c.dim, gate_text, 11);
}

/**
 * @brief 画「空中」页签（只做状态监视）。
 * @note 思路：2027 起空中系统全自动、协议无下行指令，因此所有按钮都 disabled 并附说明，位置从 radar.robots_info(10) 取（旧 12 项布局中的空中槽位）
 *        。
 */
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
    AddText(dl, ImVec2(p.x + 2.0f * s, p.y + 2.0f * s), found ? c.green : c.dim, status, 11.5f);
    // 「仅监视」徽标：空中系统 2027 起全自动、没有下行指令，用徽标与另两个可操作模式区分。
    SoftBadge(dl, ImVec2(p.x + width - TextSize("仅监视", 9).x - 18.0f * s, p.y),
              "仅监视", kModeThemes[2].accent, c, s);

    GridButton("紧急停桨", p, width, row_h, gap, 3, 1, 0, 1, ButtonKind::Ghost, false,
               "2027 协议未提供空中系统下行指令；紧急停桨只能由遥控器执行。", []() {});
    GridButton("召回", p, width, row_h, gap, 3, 1, 1, 2, ButtonKind::Ghost, false,
               "2027 协议未提供空中系统下行指令；空中系统按规则全自动行动。", []() {});
    AddText(dl, ImVec2(p.x + 2.0f * s, p.y + (row_h + gap) * 2.0f + 10.0f * s), c.dim,
            "6 号全自动，仅做状态监视", 11);
}

/**
 * @brief 画「哨兵 / 飞镖 / 空中」的模式切换分段控件，返回本帧选中的模式索引。
 * @note 思路：与下方分离式指令按钮刻意做成不同形态 —— 一整条连通胶囊、选中段填当前模式主题色并带底部指示线、每段前有专属几何标记（菱形 / 三角 / 圆点）。这样「切换模式」和「下达指令」一眼可分，
 *        三种模式也各有身份。
 * @note 参数：p/width/h —— 控件矩形；active —— 当前模式；c/s —— 色板与 DPI 缩放。
 * @return 本帧选中的模式索引（未点击则原样返回）。
 */
int DrawModeTabs(ImDrawList *dl, ImVec2 p, float width, float h,
                 const Colors &c, int active, float s) {
    const float w = width / 3.0f;
    const ImVec2 q(p.x + width, p.y + h);
    dl->AddRectFilled(p, q, WithAlpha(0xFFFFFFFFu, 4), 8.0f * s);
    dl->AddRect(p, q, kLine, 8.0f * s, 0.5f);
    int result = active;
    for (int i = 0; i < 3; ++i) {
        const ImVec2 a(p.x + w * static_cast<float>(i), p.y);
        const ImVec2 b(p.x + w * static_cast<float>(i + 1), p.y + h);
        const bool on = (i == active);
        const ImU32 accent = kModeThemes[i].accent;
        if (on) {
            dl->AddRectFilled(a, b, Alpha(accent, 46), 7.0f * s);
            dl->AddRect(a, b, Alpha(accent, 100), 7.0f * s, 0.5f);
            dl->AddRectFilled(ImVec2(a.x + 12.0f * s, b.y - 3.0f * s),
                              ImVec2(b.x - 12.0f * s, b.y - 1.0f * s), accent, 1.0f * s);
        }
        const ImVec2 mark(a.x + 14.0f * s, p.y + h * 0.5f);
        const ImU32 mark_color = on ? accent : WithAlpha(0xFFFFFFFFu, 60);
        const float r = 3.5f * s;
        if (i == 0) {  // 哨兵：菱形
            const ImVec2 pts[4] = {
                    ImVec2(mark.x, mark.y - r), ImVec2(mark.x + r, mark.y),
                    ImVec2(mark.x, mark.y + r), ImVec2(mark.x - r, mark.y)};
            dl->AddConvexPolyFilled(pts, 4, mark_color);
        } else if (i == 1) {  // 飞镖：三角
            dl->AddTriangleFilled(ImVec2(mark.x, mark.y - r),
                                 ImVec2(mark.x - r, mark.y + r),
                                 ImVec2(mark.x + r, mark.y + r), mark_color);
        } else {  // 空中：圆点
            dl->AddCircleFilled(mark, r, mark_color);
        }
        const ImVec2 label_size = TextSize(kModeThemes[i].name, 11);
        AddText(dl, ImVec2(a.x + 24.0f * s, p.y + (h - label_size.y) * 0.5f),
                on ? RoboGo::ui::Tint(accent, 0.25f, 0.45f) : c.dim,
                kModeThemes[i].name, 11);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("mode_tab", ImVec2(w, h))) result = i;
        ImGui::PopID();
    }
    return result;
}

/**
 * @brief 画右栏指令区（模式分段控件 + 当前页内容 + 指令回执）。
 * @note 思路：g_active_tab 决定整套视觉主题；切换时页签、色带、按钮色调、回执色标一起变。回执取 State::sentry_ctrl_result，没有回执时显示占位；三页共用同一网格原点，
 *        布局高度不变。
 */
void DrawCommandDeck(const State &state, RoboGo::io::MqttClient &mqtt_client,
                     ImDrawList *dl, ImVec2 p, float width, const Colors &c, float s) {
    const ModeTheme &theme = kModeThemes[g_active_tab];
    SectionLabel(dl, p, width, c, "指挥指令", nullptr);
    char mode_hint[64];
    std::snprintf(mode_hint, sizeof(mode_hint), "%s · %s", theme.code, theme.name);
    AddTextRightAligned(dl, p.x + width, p.y + 1.0f, Alpha(theme.accent, 220), mode_hint, 10.5f);
    float y = p.y + kSectH * s;

    // 页签：连通分段控件，与下方分离式指令按钮形态刻意不同。
    g_active_tab = DrawModeTabs(dl, ImVec2(p.x, y), width, 25.0f * s, c, g_active_tab, s);
    y += 25.0f * s + 9.0f * s;

    // 模式色带：横贯指令区的一条主题色细线，切换模式时整块区域一起换色。
    dl->AddRectFilled(ImVec2(p.x, y - 5.0f * s), ImVec2(p.x + width, y - 3.0f * s),
                      Alpha(theme.accent, 130), 1.0f * s);

    const ImVec2 grid_origin(p.x, y);
    if (g_active_tab == 0) DrawSentryTab(mqtt_client, dl, grid_origin, width, c, s);
    else if (g_active_tab == 1) DrawDartTab(state, mqtt_client, dl, grid_origin, width, c, s);
    else DrawAirTab(state, dl, grid_origin, width, c, s);
    y += (29.0f + 6.0f) * 4.0f * s;

    // 指令回执
    const ImVec2 rp(p.x, y);
    const ImVec2 rq(p.x + width, y + 34.0f * s);
    Card(dl, rp, rq, c, 9.0f * s, WithAlpha(0xFFFFFFFFu, 7));
    // 回执条左侧色标：让「上一条指令」也带上当前模式的身份。
    dl->AddRectFilled(ImVec2(rp.x + 2.0f * s, rp.y + 7.0f * s),
                      ImVec2(rp.x + 5.0f * s, rq.y - 7.0f * s), Alpha(theme.accent, 170), 1.5f * s);
    char receipt[96];
    if (state.sentry_ctrl_result.has_value()) {
        std::snprintf(receipt, sizeof(receipt), "上一条指令 #%u · 结果 %u",
                      state.sentry_ctrl_result->command_id(), state.sentry_ctrl_result->result_code());
    } else {
        std::snprintf(receipt, sizeof(receipt), "暂无指令回执");
    }
    AddText(dl, ImVec2(rp.x + 14.0f * s, rp.y + 10.0f * s), c.text2, receipt, 10.5f);
    AddTextRightAligned(dl, rq.x - 12.0f * s, rp.y + 10.0f * s, c.dim, "链路正常", 10.5f);
}

/**
 * @brief 画链路状态卡（裁判系统 / MQTT / 图传 / 雷达 / 标记下发）。
 * @note 思路：连接态由 MqttClient 的原子量无锁读取，图传态由调用方传入的 video_ready 决定；底部规则说明只在剩余高度足够时画。
 */
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
        y += 21.0f * s;
    }
    KvRow(dl, ImVec2(p.x + 14.0f * s, y), width - 28.0f * s, c, "标记下发", "全队（含哨兵）", s);
    const float note_y = y + 22.0f * s;
    if (note_y + 42.0f * s <= q.y - 6.0f * s) {
        AddText(dl, ImVec2(p.x + 14.0f * s, note_y), c.dim,
                "命中固定目标不造成群体扣血；", 10.5f);
        AddText(dl, ImVec2(p.x + 14.0f * s, note_y + 15.0f * s), c.dim,
                "4 发全中会触发对方基地护甲展开。", 10.5f);
        AddText(dl, ImVec2(p.x + 14.0f * s, note_y + 30.0f * s), c.dim,
                "空中系统 2027 起全自动，仅做状态监视。", 10.5f);
    }
}

/**
 * @brief 串起右栏四个分区（敌方阵容 → 敌方建筑 → 指挥指令 → 链路状态）。
 * @note 思路：与 DrawLeftColumn 对称，链路卡高度取剩余列高并保底 140*s。
 */
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

/**
 * @brief 指挥台总入口。详见头文件。
 * @note 思路：开一个覆盖全视口、无边框无滚动条的 ImGui 窗口 → 画深色底 + 极淡扫描线 →画顶栏与左右栏 → 中栏按场地宽高比算地图高度，再依次排 KPI、事件流、关键机制。
 *        所有尺寸来自 ComputeLayout，窗口本身不接收滚动，避免操作手误触。
 * @note 参数：video_* / map_* —— 由 platform_backend 提供的纹理句柄与尺寸，均为只读快照。
 * @note 注意：整个函数不写 State，也不持有任何纹理所有权；纹理生命周期归 PlatformBackend。
 */
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
    // 场地原始宽高比 280:150。地图矩形必须严格保持这个比例：ShowTacticalMap 里底图是
    // AddImage 到整个矩形、所有建筑/雷达/点击坐标也按 map_width / map_height 分别线性映射，
    // 矩形比例一变，整张场地连同圆形标记就会被一起拉伸（点位不错位，但形状失真）。
    // 布局优先级：地图占满中栏 > 保持 1.87:1 比例 > 下方信息区高度。
    // 地图「变小」靠 ComputeLayout 加宽左右侧栏、压窄中栏来实现，而不是在地图两侧留空带；
    // KPI / 事件 / 机制区同用 mid_w，会跟着一起变窄，整列视觉对齐。
    // ShowTacticalMap 内部无需改动，所有映射自动跟随矩形。
    constexpr float kMapAspect = kMapWidthDm / kMapHeightDm;
    const float gap = 12.0f * s;
    const float min_kpi_h = 80.0f * s;    // DrawKpi 内容需要 ≥75px（标签+数值+底部控制带）
    const float min_lower_h = 120.0f * s; // 事件流/机制卡的最低可用高度
    // 只有极端宽矮屏（如 2560×1080）才会触发高度预算，此时地图等比缩小并居中，仍不失真。
    const float map_h_budget = std::max(
            layout.col_h - gap * 2.0f - min_kpi_h - min_lower_h, 180.0f * s);
    // 不再内缩留白：侧栏已加宽，中栏本身就是合适宽度，地图直接占满它。
    // 高度始终由宽度反算，1.87:1 比例与所有坐标映射不变；只有极端宽矮屏会触发
    // map_h_budget，让地图等比缩小并居中（那时才有少量边，比例依旧正确）。
    const float map_w = std::min(layout.mid_w, map_h_budget * kMapAspect);
    const float map_h = map_w / kMapAspect;
    const float map_x = mid.x + (layout.mid_w - map_w) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(map_x, mid.y));
    ShowTacticalMap(state, mqtt_client, map_texture_ref, map_texture_size,
                    map_terrain_texture_ref, map_terrain_texture_size,
                    ImVec2(map_w, map_h));
    const char *map_hint = "点击地图 = 向全队（含哨兵）下发标记";
    const ImVec2 hint_size = TextSize(map_hint, 10.5f);
    const ImVec2 hint_p(map_x + map_w - hint_size.x - 24.0f * s, mid.y + 9.0f * s);
    dl->AddRectFilled(ImVec2(hint_p.x - 7.0f * s, hint_p.y - 3.0f * s),
                      ImVec2(hint_p.x + hint_size.x + 7.0f * s, hint_p.y + hint_size.y + 3.0f * s),
                      IM_COL32(8, 10, 14, 160), 5.0f * s);
    AddText(dl, hint_p, WithAlpha(0xFFFFFFFFu, 128), map_hint, 10.5f);

    // 下方两块按剩余高度分配：KPI 取 38%（夹在 80~100），其余全给事件流与关键机制。
    const float kpi_y = mid.y + map_h + gap;
    const float remaining = mid.y + layout.col_h - kpi_y - gap;
    const float kpi_h = std::clamp(remaining * 0.38f, min_kpi_h, 100.0f * s);
    DrawKpi(state, dl, ImVec2(mid.x, kpi_y), layout.mid_w, kpi_h, colors, ours, theirs, s);

    const float lower_y = kpi_y + kpi_h + gap;
    const float lower_h = std::max(mid.y + layout.col_h - lower_y, min_lower_h);
    // 空间紧凑时加宽机制卡，让 DrawMechanisms 的 4×2 两列布局每列仍有 ≥130px。
    const bool compact = lower_h < 220.0f * s;
    const float mech_w = compact
            ? std::clamp(layout.mid_w * 0.33f, 320.0f * s, 400.0f * s)
            : std::clamp(layout.mid_w * 0.395f, 240.0f * s, 260.0f * s);
    const float ev_w = std::max(layout.mid_w - mech_w - gap, 160.0f * s);
    DrawEvents(state, dl, ImVec2(mid.x + 4.0f * s, lower_y + 4.0f * s), ev_w - 8.0f * s,
               lower_h - 8.0f * s, colors, s);
    DrawMechanisms(state, dl, ImVec2(mid.x + ev_w + 12.0f * s, lower_y),
                   mech_w, lower_h, colors, s);

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}
