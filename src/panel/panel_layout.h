// 面板布局与通用 ImGui 样式/子窗口工具：供各功能面板复用统一的标题、按钮样式与一行多面板布局。
#pragma once

#include <pch.h>

namespace RoboGo::panel {

// 按钮配色与尺寸样式：常态/悬停/按下三色 + 内边距 + 圆角。
struct ButtonStyle {
    ImVec4 button_color;                   // 常态背景色
    ImVec4 hovered_color;                  // 鼠标悬停时的背景色
    ImVec4 active_color;                   // 鼠标按下时的背景色
    ImVec2 padding = ImVec2(12.0f, 8.0f);  // 按钮内边距（横向、纵向）
    float rounding = 6.0f;                 // 按钮圆角半径
};

// 一行中一个子面板的单元：是否可见 + 渲染回调（接收分配到的宽度）。
struct PanelRowItem {
    bool visible = false;               // 是否参与排版：false 时该子面板不占宽度
    std::function<void(float)> render;  // 渲染回调，参数为分配到的宽度
};

// RAII 风格的 ImGui 样式栈管理：析构时按计数统一 Pop，避免遗漏。
class ScopedStyle {
public:
    ScopedStyle() = default;
    ScopedStyle(const ScopedStyle &) = delete;
    ScopedStyle &operator=(const ScopedStyle &) = delete;
    ScopedStyle(ScopedStyle &&other) noexcept;
    ScopedStyle &operator=(ScopedStyle &&other) noexcept;
    ~ScopedStyle();

    // 功能：压入一个颜色样式（如 ImGuiCol_Text / ImGuiCol_Border），并累加颜色计数。
    // 参数 index：ImGui 颜色枚举项；color：要覆盖的 RGBA 颜色。
    void PushColor(ImGuiCol index, const ImVec4 &color);
    // 功能：压入一个标量样式变量（如圆角、边框宽度），并累加变量计数。
    // 参数 index：ImGui 样式枚举项；value：目标数值。
    void PushVar(ImGuiStyleVar index, float value);
    // 功能：压入一个二维样式变量（如内边距 ImVec2），并累加变量计数。
    // 参数 index：ImGui 样式枚举项；value：目标二维数值。
    void PushVar(ImGuiStyleVar index, const ImVec2 &value);

private:
    // 功能：按记录的计数把已压入的样式全部弹出并清零计数（析构与移动赋值时调用），防止样式栈泄漏。
    void Reset();

    int color_count_ = 0; // 已压入的颜色数量，析构时按此计数 Pop
    int var_count_ = 0;   // 已压入的样式变量数量，析构时按此计数 Pop
};

// RAII 风格的子窗口：构造 BeginChild，析构 EndChild。
class ScopedChildWindow {
public:
    ScopedChildWindow(const char *name, const ImVec2 &size, bool border = false, ImGuiWindowFlags flags = 0);
    ScopedChildWindow(const ScopedChildWindow &) = delete;
    ScopedChildWindow &operator=(const ScopedChildWindow &) = delete;
    ~ScopedChildWindow();
};

// 带标题的命名子面板：构造时先绘制彩色标题，再开启子窗口。
class ScopedTitledPanel {
public:
    ScopedTitledPanel(const char *panel_id, float panel_width, const char *title, const ImVec4 &color);

private:
    ScopedChildWindow child_;  // 底层子窗口，生命周期与本节面板绑定（构造 BeginChild、析构 EndChild）
};

// 构造仅修改文字颜色的 ScopedStyle。
ScopedStyle MakeTextColorStyle(const ImVec4 &color);
// 构造输入框（FrameBg 等）样式。
ScopedStyle MakeInputStyle();
// 按 ButtonStyle 构造按钮样式（三色 + 内边距 + 圆角）。
ScopedStyle MakeButtonStyle(const ButtonStyle &button_style);
// 构造鼠标灵敏度滑块样式。
ScopedStyle MakeMouseSensitivitySliderStyle();
// 构造窗口背景/圆角/内边距样式。
ScopedStyle MakeWindowStyle(const ImVec4 &background_color, float rounding, const ImVec2 &padding);

// 绘制一段带颜色的面板标题（文字 + 分隔线 + 间距）。
void DrawPanelTitle(const char *title, const ImVec4 &color);

// 把若干可见子面板横向排成一行，并均分宽度（隐藏不可见项）。name 为子窗口标识，frame_row_count 控制行高。
void ShowPanelRow(const char *name, float frame_row_count, std::initializer_list<PanelRowItem> panels);

}// namespace RoboGo::panel
