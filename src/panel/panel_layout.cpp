// panel_layout.cpp：面板布局与通用 ImGui 样式/子窗口工具的实现。属于「业务功能层-UI」。
// 提供 RAII 风格的样式栈(ScopedStyle)、子窗口(ScopedChildWindow)、带标题面板(ScopedTitledPanel)，
// 以及统一的按钮/输入框/滑块样式构造器与一行多面板横向排布(ShowPanelRow)，供各功能面板复用。
#include "panel_layout.h"

namespace RoboGo::panel {   // 所有布局/样式工具都放在 RoboGo::panel 命名空间内

// 移动构造：接管对方的样式计数，并把源对象计数清零（避免重复 Pop）。
ScopedStyle::ScopedStyle(ScopedStyle &&other) noexcept
    : color_count_(other.color_count_), var_count_(other.var_count_) { // 接管对方的样式计数
    other.color_count_ = 0;                                          // 源对象计数清零，避免其析构时重复 Pop
    other.var_count_ = 0;   // 源对象变量计数清零（与 color 计数同理）
}   // 结束移动构造

// 移动赋值：先 Reset 自身，再接管对方计数。
ScopedStyle &ScopedStyle::operator=(ScopedStyle &&other) noexcept {
    if (this != &other) {                                            // 防止自赋值
        Reset();                                                     // 先弹出自身已压入的样式
        color_count_ = other.color_count_;                           // 接管对方计数
        var_count_ = other.var_count_;   // 接管对方已压入的样式变量计数
        other.color_count_ = 0;                                      // 源对象计数清零
        other.var_count_ = 0;   // 源对象变量计数清零，避免其析构时重复 Pop
    }   // 结束自赋值判断
    return *this;   // 返回自身以支持链式赋值
}   // 结束移动赋值运算符

// 析构：统一把累积的样式弹栈。
ScopedStyle::~ScopedStyle() {
    Reset();                                                        // 析构时统一弹出所有已压入的样式
}   // 结束析构函数

// 压入一个颜色样式并累加计数。
void ScopedStyle::PushColor(ImGuiCol index, const ImVec4 &color) {
    ImGui::PushStyleColor(index, color);                            // 压入一个颜色样式
    color_count_++;                                                 // 累加颜色计数
}   // 结束 PushColor

// 压入一个 float 型样式变量并累加计数。
void ScopedStyle::PushVar(ImGuiStyleVar index, float value) {
    ImGui::PushStyleVar(index, value);                              // 压入一个 float 型样式变量
    var_count_++;                                                   // 累加变量计数
}   // 结束 float 版 PushVar

// 压入一个 ImVec2 型样式变量并累加计数。
void ScopedStyle::PushVar(ImGuiStyleVar index, const ImVec2 &value) {
    ImGui::PushStyleVar(index, value);                              // 压入一个 ImVec2 型样式变量
    var_count_++;                                                   // 累加变量计数
}   // 结束 ImVec2 版 PushVar

// 按计数逆序弹出样式变量与颜色，并清零计数。
void ScopedStyle::Reset() {
    if (var_count_ > 0) {                                            // 有未弹出的样式变量
        ImGui::PopStyleVar(var_count_);                             // 一次性弹出全部变量
        var_count_ = 0;                                             // 计数清零
    }   // 结束样式变量弹出
    if (color_count_ > 0) {                                          // 有未弹出的颜色
        ImGui::PopStyleColor(color_count_);                         // 一次性弹出全部颜色
        color_count_ = 0;                                           // 计数清零
    }   // 结束颜色弹出
}   // 结束 Reset

// 构造时开启一个子窗口（固定宽度、高度自适应）。
ScopedChildWindow::ScopedChildWindow(const char *name, const ImVec2 &size, bool border, ImGuiWindowFlags flags) {
    ImGui::BeginChild(name, size, border, flags);                   // 开启子窗口
}   // 结束 ScopedChildWindow 构造

// 析构时结束子窗口。
ScopedChildWindow::~ScopedChildWindow() {
    ImGui::EndChild();                                              // 结束子窗口
}   // 结束 ScopedChildWindow 析构

// 构造时先绘制彩色标题，再以给定宽度开启子窗口。
ScopedTitledPanel::ScopedTitledPanel(const char *panel_id, float panel_width, const char *title, const ImVec4 &color)
    : child_(panel_id, ImVec2(panel_width, 0), false) {             // 先以给定宽度开启子窗口（高度自适应）
    DrawPanelTitle(title, color);                                   // 再绘制彩色标题
}   // 结束 ScopedTitledPanel 构造（先开子窗口再画标题）

// 功能：构造仅修改文字颜色的 ScopedStyle。
ScopedStyle MakeTextColorStyle(const ImVec4 &color) {
    ScopedStyle style;                                              // 默认构造空样式栈
    style.PushColor(ImGuiCol_Text, color);                          // 仅压入文字颜色
    return style;                                                   // 返回（析构时自动弹栈）
}   // 结束 MakeTextColorStyle

// 功能：构造输入框样式（FrameBg 三态颜色 + 内边距 + 圆角）。
ScopedStyle MakeInputStyle() {
    ScopedStyle style;                                              // 默认构造空样式栈
    style.PushColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.2f, 0.25f, 0.8f));          // 输入框常态背景色
    style.PushColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.25f, 0.3f, 0.9f));    // 悬停背景色
    style.PushColor(ImGuiCol_FrameBgActive, ImVec4(0.25f, 0.3f, 0.35f, 1.0f));    // 激活背景色
    style.PushVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));                // 输入框内边距
    style.PushVar(ImGuiStyleVar_FrameRounding, 4.0f);                             // 输入框圆角
    return style;                                                   // 返回（析构时自动弹栈）
}   // 结束 MakeInputStyle

// 功能：根据 ButtonStyle 构造按钮样式（三色 + 内边距 + 圆角）。
ScopedStyle MakeButtonStyle(const ButtonStyle &button_style) {
    ScopedStyle style;                                              // 默认构造空样式栈
    style.PushColor(ImGuiCol_Button, button_style.button_color);              // 按钮常态色
    style.PushColor(ImGuiCol_ButtonHovered, button_style.hovered_color);      // 按钮悬停色
    style.PushColor(ImGuiCol_ButtonActive, button_style.active_color);        // 按钮按下色
    style.PushVar(ImGuiStyleVar_FramePadding, button_style.padding);          // 按钮内边距
    style.PushVar(ImGuiStyleVar_FrameRounding, button_style.rounding);        // 按钮圆角
    return style;                                                   // 返回（析构时自动弹栈）
}   // 结束 MakeButtonStyle

// 功能：构造鼠标灵敏度滑块样式（滑块颜色 + 抓取最小尺寸 + 圆角）。
ScopedStyle MakeMouseSensitivitySliderStyle() {
    ScopedStyle style;                                              // 默认构造空样式栈
    style.PushColor(ImGuiCol_SliderGrab, ImVec4(1.0f, 0.8f, 0.3f, 0.8f));          // 滑块抓取块常态色
    style.PushColor(ImGuiCol_SliderGrabActive, ImVec4(1.0f, 0.9f, 0.4f, 1.0f));    // 滑块抓取块激活色
    style.PushVar(ImGuiStyleVar_GrabMinSize, 10.0f);                             // 抓取块最小尺寸
    style.PushVar(ImGuiStyleVar_GrabRounding, 4.0f);                             // 抓取块圆角
    return style;                                                   // 返回（析构时自动弹栈）
}   // 结束 MakeMouseSensitivitySliderStyle

// 功能：构造窗口样式（背景色 + 圆角 + 内边距）。
ScopedStyle MakeWindowStyle(const ImVec4 &background_color, float rounding, const ImVec2 &padding) {
    ScopedStyle style;                                              // 默认构造空样式栈
    style.PushColor(ImGuiCol_WindowBg, background_color);            // 窗口背景色
    style.PushVar(ImGuiStyleVar_WindowRounding, rounding);           // 窗口圆角
    style.PushVar(ImGuiStyleVar_WindowPadding, padding);             // 窗口内边距
    return style;                                                   // 返回（析构时自动弹栈）
}   // 结束 MakeWindowStyle

// 功能：绘制面板标题（彩色文字 + 分隔线 + 间距）。
void DrawPanelTitle(const char *title, const ImVec4 &color) {
    ScopedStyle title_style = MakeTextColorStyle(color);             // 按给定颜色构造文字样式
    ImGui::Text("%s", title);                                       // 渲染标题文字
    ImGui::Separator();                                             // 分隔线
    ImGui::Spacing();                                               // 间距
}   // 结束 DrawPanelTitle

// 功能：计算一行中每个子面板的宽度（按可见数量均分内容宽度，并为间隔预留 5px）。
float GetPanelWidth(int panel_count) {
    // 均分内容宽度：每项宽度 = 总宽/数量 + (数量-1)*5，5px 为面板间预留间隔。
    return panel_count > 0 ? ImGui::GetContentRegionAvail().x / static_cast<float>(panel_count) + static_cast<float>(panel_count - 1) * 5.0f : 0.0f;
}   // 结束 GetPanelWidth

// 功能：在排布下一个面板时，非首个则切换到同一行（SameLine），首个则仅标记已放置。
void PlaceNextPanel(bool &is_first) {
    if (is_first) {                                                 // 首个面板：仅标记已放置
        is_first = false;   // 标记本行已放置首个面板
    } else {                                                        // 后续面板：切到同一行
        ImGui::SameLine();   // 后续面板用 SameLine 接到同一行
    }   // 结束分支
}   // 结束 PlaceNextPanel

// 功能：把若干子面板横向排成一行（均分宽度，隐藏不可见项）。
// 思路：先统计可见数量；为 0 则整行不渲染；否则开一个行高固定的子窗口，按宽度逐个 PlaceNextPanel 后调用其渲染回调。
void ShowPanelRow(const char *name, float frame_row_count, std::initializer_list<PanelRowItem> panels) {
    int panel_count = 0;                                            // 可见面板计数
    for (const auto &panel : panels) {                              // 遍历所有候选面板
        if (panel.visible) {                                        // 仅统计可见项
            panel_count++;   // 可见项加一
        }   // 结束可见性判断
    }   // 结束统计循环
    // 没有可见面板则跳过整行，避免空子窗口。
    if (panel_count == 0) {                                         // 无可见项
        return;                                                     // 直接返回，不渲染空行
    }   // 结束无可见项判断

    ScopedChildWindow row(name,                                     // 开一个行高固定的子窗口承载整行
                          ImVec2(0, ImGui::GetFrameHeightWithSpacing() * frame_row_count + ImGui::GetStyle().SeparatorSize),   // 子窗口高度：按行数乘「行高+行间距」，再加上分隔线高度，保证内容不被裁切
                          false,                                    // 无边框
                          ImGuiWindowFlags_NoScrollbar);             // 禁止滚动条
    float panel_width = GetPanelWidth(panel_count);                 // 计算均分后的每项宽度
    bool is_first = true;                                           // 是否为本行首个面板
    for (const auto &panel : panels) {                              // 再次遍历以渲染
        if (!panel.visible) {                                       // 不可见项跳过
            continue;   // 不可见项不占位，直接跳过
        }   // 结束不可见判断
        PlaceNextPanel(is_first);                                   // 非首个则切换到同行
        panel.render(panel_width);                                  // 调用渲染回调（传入分配宽度）
    }   // 结束渲染循环
}   // 结束 ShowPanelRow

}// namespace RoboGo::panel
