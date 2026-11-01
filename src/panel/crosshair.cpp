// crosshair.cpp：准星绘制面板的实现。
#include "crosshair.h"  // 引入本面板对应的头文件（含函数声明与依赖）

// 功能：在屏幕中心绘制十字准星，并以热量环实时反映枪管热量比例。
// 思路：利用窗口 DrawList 画线（十字）与圆弧（热量环），颜色随热量由绿经黄转红。
void ShowCrosshair(const State &state) {  // state：全局状态（含动态/静态机器人状态）
    // 无导航/无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("CrosshairWindow", nullptr, window_flags)) {  // 窗口创建失败则结束
        ImGui::End();  // 配对 End
        return;  // 不绘制
    }  // 结束 Begin 失败分支

    ImDrawList *draw_list = ImGui::GetWindowDrawList();  // 获取当前窗口绘制列表（底层图元）
    ImVec2 window_pos = ImGui::GetWindowPos();  // 窗口左上角的屏幕坐标
    ImVec2 window_size = ImGui::GetWindowSize();  // 窗口尺寸
    ImVec2 center = ImVec2(window_pos.x + window_size.x / 2, window_pos.y + window_size.y / 2);  // 窗口中心即准星中心

    // 绘制十字准星
    float crosshair_size = 10.0f;  // 十字半长（向两侧延伸长度）
    float crosshair_thickness = 1.0f;  // 线宽
    ImU32 crosshair_color = IM_COL32(255, 255, 255, 255);  // 白色（32 位 RGBA 打包）

    // 水平线
    draw_list->AddLine(
            ImVec2(center.x - crosshair_size, center.y),  // 起点：中心左侧
            ImVec2(center.x + crosshair_size, center.y),  // 终点：中心右侧
            crosshair_color, crosshair_thickness);  // 颜色与线宽

    // 垂直线
    draw_list->AddLine(
            ImVec2(center.x, center.y - crosshair_size),  // 起点：中心上侧
            ImVec2(center.x, center.y + crosshair_size),  // 终点：中心下侧
            crosshair_color, crosshair_thickness);  // 颜色与线宽

    // 绘制热量环
    float ring_radius = 40.0f;  // 热量环半径
    float ring_thickness = 4.0f;  // 环线宽
    float heat_percentage;  // 热量比例（0~1）
    if (state.robot_dynamic_status.has_value() && state.robot_static_status.has_value() && state.robot_static_status->max_heat() != 0) {  // 动态与静态状态均有效且最大热量非零才可计算比例（避免除零）
        heat_percentage = state.robot_dynamic_status->current_heat() / static_cast<float>(state.robot_static_status->max_heat());  // 当前热量/最大热量
    } else {  // 数据缺失或无热量上限
        heat_percentage = 0.0f;  // 数据缺失或无热量上限时视为 0
    }  // 结束热量比例计算的三元分支

    if (heat_percentage > 0.01f) {  // 仅在热量可感知时绘制，避免空环
        // 根据热量比例计算颜色（绿色 -> 黄色 -> 红色）
        ImU32 heat_color;  // 热量环颜色（32 位 RGBA）
        if (heat_percentage < 0.5f) {  // 低热量段：绿到黄
            // 绿色到黄色
            float ratio = heat_percentage * 2.0f;  // 0~0.5 映射到 0~1
            int r = static_cast<int>(255 * ratio);  // 红色分量随热量上升
            int g = 255;  // 绿色保持满
            int b = 0;  // 蓝色为 0
            heat_color = IM_COL32(r, g, b, 255);  // 打包颜色
            // 低热量段结束，下面转入高热量段
        } else {  // 高热量段：黄到红
            // 黄色到红色
            float ratio = (heat_percentage - 0.5f) * 2.0f;  // 0.5~1 映射到 0~1
            int r = 255;  // 红色保持满
            int g = static_cast<int>(255 * (1.0f - ratio));  // 绿色分量随热量上升而下降
            int b = 0;  // 蓝色为 0
            heat_color = IM_COL32(r, g, b, 255);  // 打包颜色
        }  // 高热量段分支结束（与上方 if 配对完毕）

        // 绘制热量环
        float end_angle = heat_percentage * 2.0f * 3.14159f;  // 按热量比例计算圆弧终止角（0~2π）
        draw_list->PathArcTo(center, ring_radius, 0.0f, end_angle, 32);  // 构建圆弧路径（32 段近似）
        draw_list->PathStroke(heat_color, ring_thickness, ImDrawFlags_None);  // 描边成环
    }  // 结束“热量可感知”绘制分支

    ImGui::End();  // 结束窗口绘制
}  // 结束 ShowCrosshair
