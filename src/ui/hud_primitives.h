// hud_primitives.h：主 HUD 自绘的共享度量/文字/配色工具（header-only）。
// 定稿里的顶部条、队伍条、本车卡、控制度带都用 ImDrawList 直接画，
// 这些「量文字 → 画文字 → 取色」的样板以前会在各面板各写一份，统一收在这里。
#pragma once

#include <pch.h>
#include "ui/theme.h"

namespace RoboGo::ui::hud {

// 统一色板：与全局主题同源，只补几块 HUD 专用的半透明表面/底槽。
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
inline ImVec2 TextSize(const char *text, float font_size) {
    ImGui::PushFont(ImGui::GetFont(), font_size);
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    return size;
}

inline void AddText(ImDrawList *draw_list, const ImVec2 &pos, ImU32 color,
                    const char *text, float font_size) {
    ImGui::PushFont(ImGui::GetFont(), font_size);
    draw_list->AddText(pos, color, text);
    ImGui::PopFont();
}

inline void AddTextRightAligned(ImDrawList *draw_list, float right, float y, ImU32 color,
                                const char *text, float font_size) {
    const ImVec2 size = TextSize(text, font_size);
    AddText(draw_list, ImVec2(right - size.x, y), color, text, font_size);
}

inline void AddTextCenteredIn(ImDrawList *draw_list, float left, float right, float y, ImU32 color,
                              const char *text, float font_size) {
    const ImVec2 size = TextSize(text, font_size);
    AddText(draw_list, ImVec2(left + (right - left - size.x) * 0.5f, y), color, text, font_size);
}

}  // namespace RoboGo::ui::hud
