// panel_layout.cpp：面板布局与通用 ImGui 样式/子窗口工具的实现。属于「业务功能层-UI」。
// 提供 RAII 风格的样式栈(ScopedStyle)、子窗口(ScopedChildWindow)、带标题面板(ScopedTitledPanel)，
// 以及统一的按钮/输入框/滑块样式构造器与一行多面板横向排布(ShowPanelRow)，供各功能面板复用。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
// [/pch-split]
#include "panel_layout.h"
#include "ui/theme.h"   // 统一视觉令牌：三级表面、低对比描边、降饱和语义色

namespace RoboGo::panel {   // 所有布局/样式工具都放在 RoboGo::panel 命名空间内

// 移动构造：接管对方的样式计数，并把源对象计数清零（避免重复 Pop）。
ScopedStyle::ScopedStyle(ScopedStyle &&other) noexcept
    : color_count_(other.color_count_), var_count_(other.var_count_) { // 接管对方的样式计数
    other.color_count_ = 0;                                          // 源对象计数清零，避免其析构时重复 Pop
    other.var_count_ = 0;   // 源对象变量计数清零（与 color 计数同理）
}

// 移动赋值：先 Reset 自身，再接管对方计数。
ScopedStyle &ScopedStyle::operator=(ScopedStyle &&other) noexcept {
    if (this != &other) {                                            // 防止自赋值
        Reset();                                                     // 先弹出自身已压入的样式
        color_count_ = other.color_count_;                           // 接管对方计数
        var_count_ = other.var_count_;   // 接管对方已压入的样式变量计数
        other.color_count_ = 0;                                      // 源对象计数清零
        other.var_count_ = 0;   // 源对象变量计数清零，避免其析构时重复 Pop
    }
    return *this;   // 返回自身以支持链式赋值
}

// 析构：统一把累积的样式弹栈。
ScopedStyle::~ScopedStyle() {
    Reset();                                                        // 析构时统一弹出所有已压入的样式
}

// 压入一个颜色样式并累加计数。
void ScopedStyle::PushColor(ImGuiCol index, const ImVec4 &color) {
    ImGui::PushStyleColor(index, color);                            // 压入一个颜色样式
    color_count_++;                                                 // 累加颜色计数
}

// 压入一个 float 型样式变量并累加计数。
void ScopedStyle::PushVar(ImGuiStyleVar index, float value) {
    ImGui::PushStyleVar(index, value);                              // 压入一个 float 型样式变量
    var_count_++;                                                   // 累加变量计数
}

// 压入一个 ImVec2 型样式变量并累加计数。
void ScopedStyle::PushVar(ImGuiStyleVar index, const ImVec2 &value) {
    ImGui::PushStyleVar(index, value);                              // 压入一个 ImVec2 型样式变量
    var_count_++;                                                   // 累加变量计数
}

// 按计数逆序弹出样式变量与颜色，并清零计数。
void ScopedStyle::Reset() {
    if (var_count_ > 0) {                                            // 有未弹出的样式变量
        ImGui::PopStyleVar(var_count_);                             // 一次性弹出全部变量
        var_count_ = 0;                                             // 计数清零
    }
    if (color_count_ > 0) {                                          // 有未弹出的颜色
        ImGui::PopStyleColor(color_count_);                         // 一次性弹出全部颜色
        color_count_ = 0;                                           // 计数清零
    }
}

// 构造时开启一个子窗口（固定宽度、高度自适应）。
ScopedChildWindow::ScopedChildWindow(const char *name, const ImVec2 &size, bool border, ImGuiWindowFlags flags) {
    ImGui::BeginChild(name, size, border, flags);                   // 开启子窗口
}

// 析构时结束子窗口。
ScopedChildWindow::~ScopedChildWindow() {
    ImGui::EndChild();
}

// 构造时先绘制彩色标题，再以给定宽度开启子窗口。
ScopedTitledPanel::ScopedTitledPanel(const char *panel_id, float panel_width, const char *title, const ImVec4 &color)
    : child_(panel_id, ImVec2(panel_width, 0), false) {             // 先以给定宽度开启子窗口（高度自适应）
    DrawPanelTitle(title, color);                                   // 再绘制彩色标题
}   // 结束 ScopedTitledPanel 构造（先开子窗口再画标题）

/**
 * @brief 构造仅修改文字颜色的 ScopedStyle。
 */
ScopedStyle MakeTextColorStyle(const ImVec4 &color) {
    ScopedStyle style;                                              // 默认构造空样式栈
    style.PushColor(ImGuiCol_Text, color);                          // 仅压入文字颜色
    return style;                                                   // 返回（析构时自动弹栈）
}

/**
 * @brief 构造输入框样式（统一主题：surface2 + 8px 圆角）。
 */
ScopedStyle MakeInputStyle() {
    ScopedStyle style;                                              // 默认构造空样式栈
    namespace ui = RoboGo::ui;
    style.PushColor(ImGuiCol_FrameBg, ui::V(ui::WithAlpha(ui::kSurface2, 190)));
    style.PushColor(ImGuiCol_FrameBgHovered, ui::V(ui::WithAlpha(ui::kSurface2, 240)));
    style.PushColor(ImGuiCol_FrameBgActive, ui::V(ui::kSurface2));
    style.PushVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
    style.PushVar(ImGuiStyleVar_FrameRounding, ui::kRadiusControl);
    return style;                                                   // 返回（析构时自动弹栈）
}

/**
 * @brief 按 ButtonStyle 的层级构造按钮样式。【2027 UI 改版】颜色不再由每个调用点各自定义，收敛成四类：Secondary(默认) 幽灵按钮：半透明中性面 + 0.5px
 *        低对比描边 + 主文字色；Selected        选中态：沿用传入色相但降饱和压暗，保留红/蓝队伍语义；Primary         一屏唯一的实心主操作（低饱和青绿）
 *        ；Danger          退出登录等破坏性操作（低饱和红）。
 */
ScopedStyle MakeButtonStyle(const ButtonStyle &button_style) {
    ScopedStyle style;                                              // 默认构造空样式栈
    namespace ui = RoboGo::ui;
    switch (button_style.variant) {
        case ButtonVariant::Primary:
            style.PushColor(ImGuiCol_Button, ui::V(ui::kAccent));
            style.PushColor(ImGuiCol_ButtonHovered, ui::V(ui::Tint(ui::kAccent, 0.0f, 0.12f)));
            style.PushColor(ImGuiCol_ButtonActive, ui::V(ui::Tint(ui::kAccent, 0.0f, -0.14f)));
            style.PushColor(ImGuiCol_Text, ui::V(ui::kTextPrimary));
            break;
        case ButtonVariant::Danger:
            style.PushColor(ImGuiCol_Button, ui::V(ui::WithAlpha(ui::kDanger, 215)));
            style.PushColor(ImGuiCol_ButtonHovered, ui::V(ui::kDanger));
            style.PushColor(ImGuiCol_ButtonActive, ui::V(ui::Tint(ui::kDanger, 0.0f, -0.15f)));
            style.PushColor(ImGuiCol_Text, ui::V(ui::kTextPrimary));
            break;
        case ButtonVariant::Selected: {
            auto mute = [](const ImVec4 &color) {                 // 向亮度靠拢，去掉高饱和刺眼感
                const float lum = 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
                constexpr float k = 0.45f;
                return ImVec4(color.x * (1 - k) + lum * k, color.y * (1 - k) + lum * k,
                              color.z * (1 - k) + lum * k, 1.0f);
            };
            const ImVec4 base = mute(button_style.button_color);
            const ImVec4 hover = mute(button_style.hovered_color);
            style.PushColor(ImGuiCol_Button, ImVec4(base.x * 0.72f, base.y * 0.72f, base.z * 0.72f, 0.95f));
            style.PushColor(ImGuiCol_ButtonHovered, ImVec4(hover.x * 0.84f, hover.y * 0.84f, hover.z * 0.84f, 1.0f));
            style.PushColor(ImGuiCol_ButtonActive, ImVec4(base.x * 0.58f, base.y * 0.58f, base.z * 0.58f, 1.0f));
            style.PushColor(ImGuiCol_Text, ui::V(ui::kTextPrimary));
            break;
        }
        case ButtonVariant::Secondary:
        default:
            style.PushColor(ImGuiCol_Button, ui::V(ui::WithAlpha(ui::kSurface2, 150)));
            style.PushColor(ImGuiCol_ButtonHovered, ui::V(ui::WithAlpha(ui::kSurface2, 205)));
            style.PushColor(ImGuiCol_ButtonActive, ui::V(ui::WithAlpha(ui::kSurface2, 255)));
            style.PushColor(ImGuiCol_Text, ui::V(ui::kTextPrimary));
            break;
    }
    style.PushColor(ImGuiCol_Border, ui::V(ui::kBorder));         // 0.5px 低对比描边
    style.PushVar(ImGuiStyleVar_FrameBorderSize, 0.5f);
    style.PushVar(ImGuiStyleVar_FramePadding, button_style.padding);
    style.PushVar(ImGuiStyleVar_FrameRounding, ui::kRadiusControl);  // 统一 8px，忽略各常量里的旧圆角
    return style;                                                   // 返回（析构时自动弹栈）
}

/**
 * @brief 构造鼠标灵敏度滑块样式（统一主题的低饱和警示色 + 8px 圆角）。
 */
ScopedStyle MakeMouseSensitivitySliderStyle() {
    ScopedStyle style;                                              // 默认构造空样式栈
    namespace ui = RoboGo::ui;
    style.PushColor(ImGuiCol_SliderGrab, ui::V(ui::kWarning));                    // 滑块抓取块常态色
    style.PushColor(ImGuiCol_SliderGrabActive, ui::V(ui::Tint(ui::kWarning, 0.0f, 0.15f))); // 激活稍亮
    style.PushVar(ImGuiStyleVar_GrabMinSize, 10.0f);                            // 抓取块最小尺寸
    style.PushVar(ImGuiStyleVar_GrabRounding, ui::kRadiusControl);              // 抓取块圆角
    return style;                                                   // 返回（析构时自动弹栈）
}

/**
 * @brief 构造窗口样式（背景色 + 圆角 + 内边距）。
 */
ScopedStyle MakeWindowStyle(const ImVec4 &background_color, float rounding, const ImVec2 &padding) {
    ScopedStyle style;                                              // 默认构造空样式栈
    style.PushColor(ImGuiCol_WindowBg, background_color);            // 窗口背景色
    style.PushVar(ImGuiStyleVar_WindowRounding, rounding);           // 窗口圆角
    style.PushVar(ImGuiStyleVar_WindowPadding, padding);             // 窗口内边距
    return style;                                                   // 返回（析构时自动弹栈）
}

/**
 * @brief 绘制一段面板标题（柔和文字 + 低对比分隔线 + 间距）。2027 UI 改版：调用点仍传红/绿/黄/橙等分区色，这里统一降饱和提亮，避免同一屏出现五六种高饱和彩色标题打架。
 */
void DrawPanelTitle(const char *title, const ImVec4 &color) {
    const ImVec4 soft = ImVec4(0.45f * color.x + 0.34f, 0.45f * color.y + 0.34f,
                               0.45f * color.z + 0.34f, 1.0f);
    ScopedStyle title_style = MakeTextColorStyle(soft);             // 按柔和后的颜色构造文字样式
    ImGui::Text("%s", title);                                       // 渲染标题文字
    ImGui::Separator();                                             // 分隔线（全局主题已改为 alpha 0.08）
    ImGui::Spacing();                                               // 间距
}

/**
 * @brief 计算一行中每个子面板的宽度（按可见数量均分内容宽度，并为间隔预留 5px）。
 */
float GetPanelWidth(int panel_count) {
    // 均分内容宽度：每项宽度 = 总宽/数量 + (数量-1)*5，5px 为面板间预留间隔。
    return panel_count > 0 ? ImGui::GetContentRegionAvail().x / static_cast<float>(panel_count) + static_cast<float>(panel_count - 1) * 5.0f : 0.0f;
}

/**
 * @brief 在排布下一个面板时，非首个则切换到同一行（SameLine），首个则仅标记已放置。
 */
void PlaceNextPanel(bool &is_first) {
    if (is_first) {                                                 // 首个面板：仅标记已放置
        is_first = false;   // 标记本行已放置首个面板
    } else {                                                        // 后续面板：切到同一行
        ImGui::SameLine();   // 后续面板用 SameLine 接到同一行
    }
}

/**
 * @brief 估算一行内容的像素高度（frame_row_count 个标准行高 + 一条分隔线），用于固定高子窗口，避免内容被裁切或空出过多。
 */
float EstimateRowHeight(float frame_row_count) {
    return ImGui::GetFrameHeightWithSpacing() * frame_row_count + ImGui::GetStyle().SeparatorSize;  // 行数乘「行高+行间距」再加分隔线高度
}

/**
 * @brief 取当前主题下单行「行高+行间距」的像素高度（ImGui 标准控件行高，随字号/缩放变化）。
 */
float EstimateLineHeight() {
    return ImGui::GetFrameHeightWithSpacing();  // 单行标准高度
}

/**
 * @brief 把若干子面板横向排成一行（均分宽度，隐藏不可见项）。
 * @note 思路：先统计可见数量；为 0 则整行不渲染；否则开一个行高固定的子窗口，按宽度逐个 PlaceNextPanel 后调用其渲染回调。
 */
void ShowPanelRow(const char *name, float frame_row_count, std::initializer_list<PanelRowItem> panels) {
    int panel_count = 0;                                            // 可见面板计数
    for (const auto &panel : panels) {                              // 遍历所有候选面板
        if (panel.visible) {                                        // 仅统计可见项
            panel_count++;   // 可见项加一
        }
    }
    // 没有可见面板则跳过整行，避免空子窗口。
    if (panel_count == 0) {                                         // 无可见项
        return;                                                     // 直接返回，不渲染空行
    }

    ScopedChildWindow row(name,                                     // 开一个行高固定的子窗口承载整行
                          ImVec2(0, EstimateRowHeight(frame_row_count)),   // 子窗口高度：用统一估算函数，按行数乘「行高+行间距」再加分隔线，保证内容不被裁切
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
    }
}

}  // namespace RoboGo::panel
