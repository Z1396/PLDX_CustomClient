// 面板布局与通用 ImGui 样式/子窗口工具：供各功能面板复用统一的标题、按钮样式与一行多面板布局。
#pragma once

#include <pch.h>  // 预编译头，集中提供 ImGui/标准库依赖
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
// [/pch-split]

namespace RoboGo::panel {  // 面板工具统一放在 RoboGo::panel 命名空间

// 按钮层级：一屏只保留一个 Primary（实心主操作），其余为 Secondary（幽灵按钮）。
// Selected 用于队伍/机器人选中态（保留红蓝色相但降饱和），Danger 仅用于退出登录等破坏性操作。
enum class ButtonVariant {
    Secondary,
    Selected,
    Primary,
    Danger,
};

// 按钮配色与尺寸样式：常态/悬停/按下三色 + 内边距 + 圆角 + 层级。
struct ButtonStyle {
    ImVec4 button_color;                   // 常态背景色
    ImVec4 hovered_color;                  // 鼠标悬停时的背景色
    ImVec4 active_color;                   // 鼠标按下时的背景色
    ImVec2 padding = ImVec2(12.0f, 8.0f);  // 按钮内边距（横向、纵向）
    float rounding = 6.0f;                 // 按钮圆角半径（统一主题下由 kRadiusControl 接管）
    ButtonVariant variant = ButtonVariant::Secondary;  // 层级；带默认值，旧常量的聚合初始化无需改动
};

// 一行中一个子面板的单元：是否可见 + 渲染回调（接收分配到的宽度）。
struct PanelRowItem {
    bool visible = false;               // 是否参与排版：false 时该子面板不占宽度
    std::function<void(float)> render;  // 渲染回调，参数为分配到的宽度
};

// RAII 风格的 ImGui 样式栈管理：析构时按计数统一 Pop，避免遗漏。
class ScopedStyle {
public:  // 公有接口：外部通过这些方法压栈
    ScopedStyle() = default;  // 默认构造：计数为 0，不压入任何样式
    ScopedStyle(const ScopedStyle &) = delete;  // 禁止拷贝：计数与 ImGui 样式栈一一对应，拷贝会导致重复 Pop
    ScopedStyle &operator=(const ScopedStyle &) = delete;  // 禁止拷贝赋值，原因同上
    ScopedStyle(ScopedStyle &&other) noexcept;  // 移动构造：转移计数并把源对象清零，保证只会 Pop 一次
    ScopedStyle &operator=(ScopedStyle &&other) noexcept;  // 移动赋值：先弹出自身已压样式，再接管对方计数
    ~ScopedStyle();  // 析构：按计数自动 Pop，提前 return 也不会泄漏样式栈

    // 功能：压入一个颜色样式（如 ImGuiCol_Text / ImGuiCol_Border），并累加颜色计数。
    // 参数 index：ImGui 颜色枚举项；color：要覆盖的 RGBA 颜色。
    void PushColor(ImGuiCol index, const ImVec4 &color);
    // 功能：压入一个标量样式变量（如圆角、边框宽度），并累加变量计数。
    // 参数 index：ImGui 样式枚举项；value：目标数值。
    void PushVar(ImGuiStyleVar index, float value);
    // 功能：压入一个二维样式变量（如内边距 ImVec2），并累加变量计数。
    // 参数 index：ImGui 样式枚举项；value：目标二维数值。
    void PushVar(ImGuiStyleVar index, const ImVec2 &value);

private:  // 私有实现细节
    // 功能：按记录的计数把已压入的样式全部弹出并清零计数（析构与移动赋值时调用），防止样式栈泄漏。
    void Reset();

    int color_count_ = 0; // 已压入的颜色数量，析构时按此计数 Pop
    int var_count_ = 0;   // 已压入的样式变量数量，析构时按此计数 Pop
};

// RAII 风格的子窗口：构造 BeginChild，析构 EndChild。
class ScopedChildWindow {
public:  // 公有接口
    ScopedChildWindow(const char *name, const ImVec2 &size, bool border = false, ImGuiWindowFlags flags = 0);  // 构造即 BeginChild：border 控制边框，flags 透传给 ImGui
    ScopedChildWindow(const ScopedChildWindow &) = delete;  // 禁止拷贝：子窗口必须严格配对 Begin/End
    ScopedChildWindow &operator=(const ScopedChildWindow &) = delete;  // 禁止拷贝赋值，原因同上
    ~ScopedChildWindow();  // 析构即 EndChild，保证与构造配对
};

// 带标题的命名子面板：构造时先绘制彩色标题，再开启子窗口。
class ScopedTitledPanel {
public:  // 公有接口
    ScopedTitledPanel(const char *panel_id, float panel_width, const char *title, const ImVec4 &color);  // 参数：panel_id 子窗口标识；panel_width 面板宽度；title 标题；color 标题颜色

private:  // 私有成员
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

// 估算一行（含 frame_row_count 个「行高+行间距」再加上一条分隔线）的像素高度。
// 供 ShowPanelRow 等需要把"N 行内容塞进一个固定高子窗口"时使用，避免各处手算 GetFrameHeightWithSpacing 造成偏差。
float EstimateRowHeight(float frame_row_count);

// 取单行「行高+行间距」的像素高度（ImGui 当前字号/缩放下的标准控件行高）。
float EstimateLineHeight();

}  // namespace RoboGo::panel
