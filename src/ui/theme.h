// ui/theme.h：2027 自定义终端统一视觉令牌（header-only，无 .cpp，无需改 CMake 源列表）。
//
// 设计原则：
//   1) 60/30/10 —— 大面积只用三级中性表面，降饱和语义色只用于状态与关键数值；
//   2) 低对比描边（alpha 0.08~0.15）+ 8/12 圆角，替代 1px 硬边框与直角矩形；
//   3) 纯色绘制，不加模糊/阴影/渐变（ImGui 软件绘制，帧预算 16.6ms）；
//   4) 关键数值保持高对比，赛场强光下仍要 1 秒读清。
//
// 注意：IM_COL32 打包为 ABGR，取通道时按 r=bits0-7 / g=bits8-15 / b=bits16-23 / a=bits24-31。
#pragma once

#include <pch.h>

namespace RoboGo::ui {

// ---------------- 表面与文字 ----------------
constexpr ImU32 kSurface0 = IM_COL32(11, 16, 24, 255);      // 页面底
constexpr ImU32 kSurface1 = IM_COL32(26, 34, 48, 255);      // 卡片 / 子窗口
constexpr ImU32 kSurface2 = IM_COL32(36, 46, 62, 255);      // 悬浮 / 控件 / 选中
constexpr ImU32 kTextPrimary = IM_COL32(230, 235, 242, 255);
constexpr ImU32 kTextSecondary = IM_COL32(138, 152, 171, 255);
constexpr ImU32 kTextTertiary = IM_COL32(100, 112, 130, 255);
constexpr ImU32 kBorder = IM_COL32(255, 255, 255, 38);      // alpha 0.15
constexpr ImU32 kSeparator = IM_COL32(255, 255, 255, 26);   // alpha 0.10

// ---------------- 语义色（降饱和） ----------------
constexpr ImU32 kTeamRed = IM_COL32(229, 72, 77, 255);
constexpr ImU32 kTeamBlue = IM_COL32(78, 168, 255, 255);
constexpr ImU32 kAccent = IM_COL32(72, 180, 140, 255);      // 主操作 / 在线
constexpr ImU32 kDanger = IM_COL32(200, 76, 72, 255);       // 退出登录等破坏性操作
constexpr ImU32 kWarning = IM_COL32(217, 164, 65, 255);     // 警告 / 标记
constexpr ImU32 kSentry = IM_COL32(150, 135, 210, 255);     // 哨兵 / 飞镖指令

// ---------------- 形状 ----------------
constexpr float kRadiusControl = 8.0f;
constexpr float kRadiusCard = 12.0f;

// 替换 alpha 通道，保留 RGB。constexpr 以便用于其它常量的初始化。
constexpr ImU32 WithAlpha(ImU32 color, int alpha) {
    return (color & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24);
}

// 把高饱和色向亮度靠近（desaturate），再整体提亮（lift，可为负数变暗）。
// 用于保留色相语义、去掉刺眼感：例如紫色指令按钮、彩色面板标题。
constexpr ImU32 Tint(ImU32 color, float desaturate, float lift) {
    const int r = static_cast<int>(color & 0xFFu);
    const int g = static_cast<int>((color >> 8) & 0xFFu);
    const int b = static_cast<int>((color >> 16) & 0xFFu);
    const int a = static_cast<int>((color >> 24) & 0xFFu);
    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    auto channel = [&](int value) constexpr -> int {
        const float muted = value * (1.0f - desaturate) + lum * desaturate;
        const float lifted = muted + (255.0f - muted) * lift;
        int result = static_cast<int>(lifted);
        if (result < 0) result = 0;
        if (result > 255) result = 255;
        return result;
    };
    return IM_COL32(channel(r), channel(g), channel(b), a);
}

// ImU32 -> ImVec4（ImGui 颜色样式需要 float4；此函数运行时调用，故不声明 constexpr）。
inline ImVec4 V(ImU32 color) {
    return ImGui::ColorConvertU32ToFloat4(color);
}

// 应用全局主题。必须在 StyleColorsDark() 之后、ScaleAllSizes() 之前调用，
// 这样这里设置的尺寸会一并按 DPI 缩放。
inline void ApplyGlobalTheme(ImGuiStyle &style) {
    ImVec4 *c = style.Colors;
    c[ImGuiCol_Text] = V(kTextPrimary);
    c[ImGuiCol_TextDisabled] = V(kTextTertiary);
    c[ImGuiCol_WindowBg] = V(kSurface0);
    c[ImGuiCol_ChildBg] = V(kSurface1);
    c[ImGuiCol_PopupBg] = V(kSurface1);
    c[ImGuiCol_Border] = V(kBorder);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = V(WithAlpha(kSurface2, 200));
    c[ImGuiCol_FrameBgHovered] = V(WithAlpha(kSurface2, 250));
    c[ImGuiCol_FrameBgActive] = V(kSurface2);
    c[ImGuiCol_TitleBg] = V(kSurface1);
    c[ImGuiCol_TitleBgActive] = V(kSurface2);
    c[ImGuiCol_TitleBgCollapsed] = V(kSurface1);
    c[ImGuiCol_MenuBarBg] = V(kSurface1);
    c[ImGuiCol_ScrollbarBg] = V(WithAlpha(kSurface0, 180));
    c[ImGuiCol_ScrollbarGrab] = V(WithAlpha(kSurface2, 220));
    c[ImGuiCol_ScrollbarGrabHovered] = V(kSurface2);
    c[ImGuiCol_ScrollbarGrabActive] = V(kAccent);
    c[ImGuiCol_CheckMark] = V(kAccent);
    c[ImGuiCol_SliderGrab] = V(kAccent);
    c[ImGuiCol_SliderGrabActive] = V(kAccent);
    c[ImGuiCol_Button] = V(WithAlpha(kSurface2, 140));
    c[ImGuiCol_ButtonHovered] = V(WithAlpha(kSurface2, 225));
    c[ImGuiCol_ButtonActive] = V(kSurface2);
    c[ImGuiCol_Header] = V(WithAlpha(kSurface2, 140));
    c[ImGuiCol_HeaderHovered] = V(WithAlpha(kSurface2, 220));
    c[ImGuiCol_HeaderActive] = V(kAccent);
    c[ImGuiCol_Separator] = V(kSeparator);
    c[ImGuiCol_SeparatorHovered] = V(WithAlpha(0xFFFFFFFFu, 40));
    c[ImGuiCol_SeparatorActive] = V(WithAlpha(0xFFFFFFFFu, 60));
    c[ImGuiCol_ResizeGrip] = V(WithAlpha(0xFFFFFFFFu, 30));
    c[ImGuiCol_ResizeGripHovered] = V(WithAlpha(0xFFFFFFFFu, 60));
    c[ImGuiCol_ResizeGripActive] = V(WithAlpha(0xFFFFFFFFu, 90));
    c[ImGuiCol_Tab] = V(WithAlpha(kSurface1, 200));
    c[ImGuiCol_TabHovered] = V(WithAlpha(kSurface2, 235));
    c[ImGuiCol_TabSelected] = V(kSurface2);                       // 1.92 名称（旧名 TabActive 已废弃）
    c[ImGuiCol_TabSelectedOverline] = V(WithAlpha(kAccent, 200));
    c[ImGuiCol_TabDimmed] = V(kSurface1);                         // 旧名 TabUnfocused
    c[ImGuiCol_TabDimmedSelected] = V(WithAlpha(kSurface2, 200)); // 旧名 TabUnfocusedActive
    c[ImGuiCol_TabDimmedSelectedOverline] = V(WithAlpha(kAccent, 110));
    c[ImGuiCol_PlotLines] = V(kAccent);
    c[ImGuiCol_PlotLinesHovered] = V(kWarning);
    c[ImGuiCol_PlotHistogram] = V(kAccent);
    c[ImGuiCol_PlotHistogramHovered] = V(kWarning);
    c[ImGuiCol_TableHeaderBg] = V(kSurface1);
    c[ImGuiCol_TableBorderStrong] = V(WithAlpha(0xFFFFFFFFu, 30));
    c[ImGuiCol_TableBorderLight] = V(WithAlpha(0xFFFFFFFFu, 16));
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = V(WithAlpha(kSurface1, 120));
    c[ImGuiCol_TextSelectedBg] = V(WithAlpha(kAccent, 90));
    c[ImGuiCol_TextLink] = V(kTeamBlue);
    c[ImGuiCol_TreeLines] = V(kBorder);
    c[ImGuiCol_DragDropTarget] = V(WithAlpha(kAccent, 160));
    c[ImGuiCol_DragDropTargetBg] = V(WithAlpha(kAccent, 40));
    c[ImGuiCol_UnsavedMarker] = V(kWarning);
    c[ImGuiCol_NavCursor] = V(WithAlpha(kAccent, 160));            // 1.92 名称（旧名 NavHighlight 已废弃）
    c[ImGuiCol_NavWindowingHighlight] = V(WithAlpha(0xFFFFFFFFu, 120));
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.05f, 0.07f, 0.10f, 0.65f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.04f, 0.06f, 0.09f, 0.70f);

    style.WindowRounding = kRadiusCard;
    style.ChildRounding = 10.0f;
    style.FrameRounding = kRadiusControl;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = kRadiusControl;
    style.GrabRounding = kRadiusControl;
    style.WindowBorderSize = 0.5f;
    style.ChildBorderSize = 0.5f;
    style.PopupBorderSize = 0.5f;
    style.FrameBorderSize = 0.0f;
    // 垂直节奏保持接近 ImGui 默认：开局面板是「固定高度 + 底部固定区让位」的自校准布局，
    // 内边距/行距调大会把后面的分区行挤出内容区（实测会丢掉「性能选择」）。
    // 留白感靠圆角、低对比边框和表面层级营造，不靠放大 padding。
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.GrabMinSize = 10.0f;
}

}  // namespace RoboGo::ui
