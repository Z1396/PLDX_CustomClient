#include "panel_layout.h"

namespace RoboGo::panel {

// 移动构造：接管对方的样式计数，并把源对象计数清零（避免重复 Pop）。
ScopedStyle::ScopedStyle(ScopedStyle &&other) noexcept
    : color_count_(other.color_count_), var_count_(other.var_count_) {
    other.color_count_ = 0;
    other.var_count_ = 0;
}

// 移动赋值：先 Reset 自身，再接管对方计数。
ScopedStyle &ScopedStyle::operator=(ScopedStyle &&other) noexcept {
    if (this != &other) {
        Reset();
        color_count_ = other.color_count_;
        var_count_ = other.var_count_;
        other.color_count_ = 0;
        other.var_count_ = 0;
    }
    return *this;
}

// 析构：统一把累积的样式弹栈。
ScopedStyle::~ScopedStyle() {
    Reset();
}

// 压入一个颜色样式并累加计数。
void ScopedStyle::PushColor(ImGuiCol index, const ImVec4 &color) {
    ImGui::PushStyleColor(index, color);
    color_count_++;
}

// 压入一个 float 型样式变量并累加计数。
void ScopedStyle::PushVar(ImGuiStyleVar index, float value) {
    ImGui::PushStyleVar(index, value);
    var_count_++;
}

// 压入一个 ImVec2 型样式变量并累加计数。
void ScopedStyle::PushVar(ImGuiStyleVar index, const ImVec2 &value) {
    ImGui::PushStyleVar(index, value);
    var_count_++;
}

// 按计数逆序弹出样式变量与颜色，并清零计数。
void ScopedStyle::Reset() {
    if (var_count_ > 0) {
        ImGui::PopStyleVar(var_count_);
        var_count_ = 0;
    }
    if (color_count_ > 0) {
        ImGui::PopStyleColor(color_count_);
        color_count_ = 0;
    }
}

// 构造时开启一个子窗口（固定宽度、高度自适应）。
ScopedChildWindow::ScopedChildWindow(const char *name, const ImVec2 &size, bool border, ImGuiWindowFlags flags) {
    ImGui::BeginChild(name, size, border, flags);
}

// 析构时结束子窗口。
ScopedChildWindow::~ScopedChildWindow() {
    ImGui::EndChild();
}

// 构造时先绘制彩色标题，再以给定宽度开启子窗口。
ScopedTitledPanel::ScopedTitledPanel(const char *panel_id, float panel_width, const char *title, const ImVec4 &color)
    : child_(panel_id, ImVec2(panel_width, 0), false) {
    DrawPanelTitle(title, color);
}

// 功能：构造仅修改文字颜色的 ScopedStyle。
ScopedStyle MakeTextColorStyle(const ImVec4 &color) {
    ScopedStyle style;
    style.PushColor(ImGuiCol_Text, color);
    return style;
}

// 功能：构造输入框样式（FrameBg 三态颜色 + 内边距 + 圆角）。
ScopedStyle MakeInputStyle() {
    ScopedStyle style;
    style.PushColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.2f, 0.25f, 0.8f));
    style.PushColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.25f, 0.3f, 0.9f));
    style.PushColor(ImGuiCol_FrameBgActive, ImVec4(0.25f, 0.3f, 0.35f, 1.0f));
    style.PushVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
    style.PushVar(ImGuiStyleVar_FrameRounding, 4.0f);
    return style;
}

// 功能：根据 ButtonStyle 构造按钮样式（三色 + 内边距 + 圆角）。
ScopedStyle MakeButtonStyle(const ButtonStyle &button_style) {
    ScopedStyle style;
    style.PushColor(ImGuiCol_Button, button_style.button_color);
    style.PushColor(ImGuiCol_ButtonHovered, button_style.hovered_color);
    style.PushColor(ImGuiCol_ButtonActive, button_style.active_color);
    style.PushVar(ImGuiStyleVar_FramePadding, button_style.padding);
    style.PushVar(ImGuiStyleVar_FrameRounding, button_style.rounding);
    return style;
}

// 功能：构造鼠标灵敏度滑块样式（滑块颜色 + 抓取最小尺寸 + 圆角）。
ScopedStyle MakeMouseSensitivitySliderStyle() {
    ScopedStyle style;
    style.PushColor(ImGuiCol_SliderGrab, ImVec4(1.0f, 0.8f, 0.3f, 0.8f));
    style.PushColor(ImGuiCol_SliderGrabActive, ImVec4(1.0f, 0.9f, 0.4f, 1.0f));
    style.PushVar(ImGuiStyleVar_GrabMinSize, 10.0f);
    style.PushVar(ImGuiStyleVar_GrabRounding, 4.0f);
    return style;
}

// 功能：构造窗口样式（背景色 + 圆角 + 内边距）。
ScopedStyle MakeWindowStyle(const ImVec4 &background_color, float rounding, const ImVec2 &padding) {
    ScopedStyle style;
    style.PushColor(ImGuiCol_WindowBg, background_color);
    style.PushVar(ImGuiStyleVar_WindowRounding, rounding);
    style.PushVar(ImGuiStyleVar_WindowPadding, padding);
    return style;
}

// 功能：绘制面板标题（彩色文字 + 分隔线 + 间距）。
void DrawPanelTitle(const char *title, const ImVec4 &color) {
    ScopedStyle title_style = MakeTextColorStyle(color);
    ImGui::Text("%s", title);
    ImGui::Separator();
    ImGui::Spacing();
}

// 功能：计算一行中每个子面板的宽度（按可见数量均分内容宽度，并为间隔预留 5px）。
float GetPanelWidth(int panel_count) {
    return panel_count > 0 ? ImGui::GetContentRegionAvail().x / static_cast<float>(panel_count) + static_cast<float>(panel_count - 1) * 5.0f : 0.0f;
}

// 功能：在排布下一个面板时，非首个则切换到同一行（SameLine），首个则仅标记已放置。
void PlaceNextPanel(bool &is_first) {
    if (is_first) {
        is_first = false;
    } else {
        ImGui::SameLine();
    }
}

// 功能：把若干子面板横向排成一行（均分宽度，隐藏不可见项）。
// 思路：先统计可见数量；为 0 则整行不渲染；否则开一个行高固定的子窗口，按宽度逐个 PlaceNextPanel 后调用其渲染回调。
void ShowPanelRow(const char *name, float frame_row_count, std::initializer_list<PanelRowItem> panels) {
    int panel_count = 0;
    for (const auto &panel : panels) {
        if (panel.visible) {
            panel_count++;
        }
    }
    // 没有可见面板则跳过整行，避免空子窗口。
    if (panel_count == 0) {
        return;
    }

    ScopedChildWindow row(name,
                          ImVec2(0, ImGui::GetFrameHeightWithSpacing() * frame_row_count + ImGui::GetStyle().SeparatorSize),
                          false,
                          ImGuiWindowFlags_NoScrollbar);
    float panel_width = GetPanelWidth(panel_count);
    bool is_first = true;
    for (const auto &panel : panels) {
        if (!panel.visible) {
            continue;
        }
        PlaceNextPanel(is_first);
        panel.render(panel_width);
    }
}

}// namespace RoboGo::panel
