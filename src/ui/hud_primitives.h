// hud_primitives.h：主 HUD 自绘的共享度量/文字/配色工具（header-only）。
//
// 集中提供统一色板 Colors、按字号量文字的 TextSize，以及左对齐 / 右对齐 /
// 区间居中三种文字绘制原语。顶部条、队伍条、本车卡、控制度带、指挥台都用它。
// 为什么集中：这些「量文字 → 画文字 → 取色」的样板以前在各面板各写一份，
// 字号一旦不一致就会出现「量出来的宽度和画出来的宽度对不上」的错位；
// 收在这里后所有面板共享同一套契约。
// 关键契约（改动前务必理解）：
// · font_size 是 PushFont 的「未缩放基准值」，DPI 缩放由 ImGui 统一施加；
// 度量（TextSize）与绘制（AddText）必须用同一个 font_size，否则对齐会漂；
// · 每个原语内部都成对 PushFont / PopFont，调用方的字体状态不受影响；
// · Colors 全部来自 ui/theme.h 的全局视觉令牌，只补 HUD 专用的半透明表面/底槽。
// 依赖：ImGui（imgui_internal.h 提供 GetScale）、ui/theme.h。
// 注意：本头文件被大量面板 include，任何函数签名变更都会触发大范围重编；
// 新增能力优先加新函数，不要改已有原语的参数顺序。
// 定稿里的顶部条、队伍条、本车卡、控制度带都用 ImDrawList 直接画，
// 这些「量文字 → 画文字 → 取色」的样板以前会在各面板各写一份，统一收在这里。
#pragma once

#include <pch.h>
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
// [/pch-split]
#include "ui/theme.h"

namespace RoboGo::ui::hud {

// 统一色板：与全局主题同源，只补几块 HUD 专用的半透明表面/底槽。
// 字段按用途分组：surface/border（卡片）、text/text2/dim（三级字阶）、
// red/blue/amber/gold/green（语义色）、slot/chip（底槽与小片背景）。
struct Colors {
    ImU32 surface;
    ImU32 border;
    ImU32 text;
    ImU32 text2;
    ImU32 dim;
    ImU32 red;
    ImU32 blue;
    ImU32 amber;
    ImU32 gold;
    ImU32 green;
    ImU32 slot;
    ImU32 chip;
};

/**
 * @brief 构造一份 HUD 色板。
 * @note 思路：全部从 ui/theme.h 常量派生，surface/slot/chip 用 WithAlpha 叠透明度，gold 是 HUD 专用的固定值。返回聚合初始化的值类型，
 *        无动态分配。
 */
inline Colors MakeColors() {
    return Colors{
            RoboGo::ui::WithAlpha(RoboGo::ui::kSurface1, 215),
            RoboGo::ui::kBorder,
            RoboGo::ui::kTextPrimary,
            RoboGo::ui::kTextSecondary,
            RoboGo::ui::kTextTertiary,
            RoboGo::ui::kTeamRed,
            RoboGo::ui::kTeamBlue,
            RoboGo::ui::kWarning,
            IM_COL32(230, 180, 85, 255),
            RoboGo::ui::kAccent,
            RoboGo::ui::WithAlpha(0xFFFFFFFFu, 18),
            RoboGo::ui::WithAlpha(0xFFFFFFFFu, 9),
    };
}

// 字号走 PushFont 的「未缩放基准值」，DPI 缩放由 ImGui 施加；度量与绘制必须用同一字号。
// 思路：临时 PushFont(font_size) 后 CalcTextSize，再 PopFont；
//       必须与 AddText 使用同一 font_size，否则右对齐/居中会算错位置。
// 返回值：含 DPI 缩放后的 ImVec2 尺寸。
inline ImVec2 TextSize(const char *text, float font_size) {
    ImGui::PushFont(ImGui::GetFont(), font_size);
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    return size;
}

/**
 * @brief 在绝对坐标处按指定字号画一段文字。
 * @param pos 屏幕绝对左上角；color —— ImU32 打包色；font_size —— 未缩放基准字号。
 * @note 注意：内部 PushFont/PopFont 成对，调用方字体状态不受影响。
 */
inline void AddText(ImDrawList *draw_list, const ImVec2 &pos, ImU32 color,
                    const char *text, float font_size) {
    ImGui::PushFont(ImGui::GetFont(), font_size);
    draw_list->AddText(pos, color, text);
    ImGui::PopFont();
}

/**
 * @brief 右对齐画文字（right 为右边界 x）。
 * @note 思路：先用 TextSize 量宽，再把左上角设为 (right - width, y)，复用 AddText。
 */
inline void AddTextRightAligned(ImDrawList *draw_list, float right, float y, ImU32 color,
                                const char *text, float font_size) {
    const ImVec2 size = TextSize(text, font_size);
    AddText(draw_list, ImVec2(right - size.x, y), color, text, font_size);
}

/**
 * @brief 在 [left, right] 区间内水平居中画文字。
 * @note 思路：x = left + (right-left-size.x)*0.5；竖直方向仍由调用方给的 y 决定。
 */
inline void AddTextCenteredIn(ImDrawList *draw_list, float left, float right, float y, ImU32 color,
                              const char *text, float font_size) {
    const ImVec2 size = TextSize(text, font_size);
    AddText(draw_list, ImVec2(left + (right - left - size.x) * 0.5f, y), color, text, font_size);
}

}  // namespace RoboGo::ui::hud
