// crosshair.cpp：准星绘制面板的实现。
#include "crosshair.h"

// 功能：在屏幕中心绘制十字准星，并以热量环实时反映枪管热量比例。
// 思路：利用窗口 DrawList 画线（十字）与圆弧（热量环），颜色随热量由绿经黄转红。
void ShowCrosshair(const State &state) {
    // 无导航/无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("CrosshairWindow", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    ImDrawList *draw_list = ImGui::GetWindowDrawList();  // 获取当前窗口绘制列表
    ImVec2 window_pos = ImGui::GetWindowPos();
    ImVec2 window_size = ImGui::GetWindowSize();
    ImVec2 center = ImVec2(window_pos.x + window_size.x / 2, window_pos.y + window_size.y / 2);  // 窗口中心即准星中心

    // 绘制十字准星
    float crosshair_size = 10.0f;  // 十字半长
    float crosshair_thickness = 1.0f;  // 线宽
    ImU32 crosshair_color = IM_COL32(255, 255, 255, 255);  // 白色

    // 水平线
    draw_list->AddLine(
            ImVec2(center.x - crosshair_size, center.y),
            ImVec2(center.x + crosshair_size, center.y),
            crosshair_color, crosshair_thickness);

    // 垂直线
    draw_list->AddLine(
            ImVec2(center.x, center.y - crosshair_size),
            ImVec2(center.x, center.y + crosshair_size),
            crosshair_color, crosshair_thickness);

    // 绘制热量环
    float ring_radius = 40.0f;  // 热量环半径
    float ring_thickness = 4.0f;  // 环线宽
    float heat_percentage;
    if (state.robot_dynamic_status.has_value() && state.robot_static_status.has_value() && state.robot_static_status->max_heat() != 0) {
        heat_percentage = state.robot_dynamic_status->current_heat() / static_cast<float>(state.robot_static_status->max_heat());  // 当前热量/最大热量
    } else {
        heat_percentage = 0.0f;  // 数据缺失或无热量上限时视为 0
    }

    if (heat_percentage > 0.01f) {  // 仅在热量可感知时绘制，避免空环
        // 根据热量比例计算颜色（绿色 -> 黄色 -> 红色）
        ImU32 heat_color;
        if (heat_percentage < 0.5f) {
            // 绿色到黄色
            float ratio = heat_percentage * 2.0f;  // 0~0.5 映射到 0~1
            int r = static_cast<int>(255 * ratio);  // 红色分量随热量上升
            int g = 255;
            int b = 0;
            heat_color = IM_COL32(r, g, b, 255);
        } else {
            // 黄色到红色
            float ratio = (heat_percentage - 0.5f) * 2.0f;  // 0.5~1 映射到 0~1
            int r = 255;
            int g = static_cast<int>(255 * (1.0f - ratio));  // 绿色分量随热量上升而下降
            int b = 0;
            heat_color = IM_COL32(r, g, b, 255);
        }

        // 绘制热量环
        float end_angle = heat_percentage * 2.0f * 3.14159f;  // 按热量比例计算圆弧终止角（0~2π）
        draw_list->PathArcTo(center, ring_radius, 0.0f, end_angle, 32);  // 构建圆弧路径
        draw_list->PathStroke(heat_color, ring_thickness, ImDrawFlags_None);  // 描边成环
    }

    ImGui::End();
}
