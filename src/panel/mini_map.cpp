// mini_map.cpp：战场小地图面板的实现。
#include "mini_map.h"
#include "keyboard_mouse_state_manager.h"   // 引入标记模式的全局开关查询接口（is_marker_mode 等）

// 无名命名空间：小地图内部使用的坐标/尺寸常量、边框取色、UV 裁剪等辅助函数，不对外暴露。
namespace {

// 地图逻辑尺寸（与雷达坐标系一致，单位为地图坐标），用于坐标换算
constexpr float kMapWidth = 280.0f;
constexpr float kMapHeight = 150.0f;   // 地图逻辑高度：雷达坐标系 y 方向跨度，用于屏幕 Y 线性映射

constexpr ImU32 kTeamRedColor = IM_COL32(250, 63, 64, 255);   // 红方单位主色（不透明），用于图标描边
constexpr ImU32 kTeamBlueColor = IM_COL32(3, 187, 245, 255);   // 蓝方单位主色（不透明），用于图标描边
constexpr ImU32 kTeamRedFillColor = IM_COL32(250, 63, 64, 72);   // 红方填充色（alpha 72，半透明，套在描边内形成色环）
constexpr ImU32 kTeamBlueFillColor = IM_COL32(3, 187, 245, 72);   // 蓝方填充色（同上半透明）

constexpr ImU32 kMapBackgroundColor = IM_COL32(24, 24, 28, 235);   // 地图底色：近黑的深灰，保证贴图缺失时也有可读底
constexpr ImU32 kMapOverlayColor = IM_COL32(12, 22, 30, 72);   // 底图之上的暗色蒙层（alpha 72），压低底图亮度突出单位
constexpr ImU32 kMapInnerBorderRedColor = IM_COL32(250, 63, 64, 52);   // 内容区内边框——红方视角
constexpr ImU32 kMapInnerBorderBlueColor = IM_COL32(92, 175, 236, 52);   // 内容区内边框——蓝方视角
constexpr ImU32 kMapInnerBorderUnknownColor = IM_COL32(170, 178, 190, 52);   // 内容区内边框——阵营未知时的中性灰
constexpr ImU32 kMapOuterBorderRedColor = IM_COL32(250, 63, 64, 220);   // 外边框——红方视角（alpha 220，比内框更醒目）
constexpr ImU32 kMapOuterBorderBlueColor = IM_COL32(64, 170, 245, 220);   // 外边框——蓝方视角
constexpr ImU32 kMapOuterBorderUnknownColor = IM_COL32(170, 178, 190, 220);   // 外边框——阵营未知时的中性灰
constexpr ImU32 kMapMarkerBorderColor = IM_COL32(255, 176, 64, 235);   // 标记模式下的外边框色（琥珀色，提示当前处于打点状态）

constexpr ImU32 kUnitCoreColor = IM_COL32(26, 28, 34, 255);   // 单位内芯色（深色实心圆，与外圈组成圆环效果）
constexpr ImU32 kUnitTextColor = IM_COL32(255, 255, 255, 255);   // 单位编号文字色（白色）
constexpr ImU32 kHighlightColor = IM_COL32(255, 215, 0, 255);  // 高亮（被锁定）标识色
constexpr ImU32 kPathColor = IM_COL32(255, 153, 51, 220);  // 哨兵路径色

// 功能：返回地图内边框颜色，未识别阵营时给中性灰。
ImU32 GetMapInnerBorderColor(bool team_known, bool is_red_team) {
    if (!team_known) {                                            // 尚未确定己方阵营
        return kMapInnerBorderUnknownColor;  // 阵营未知
    }   // 结束 team_known 判断
    return is_red_team ? kMapInnerBorderRedColor : kMapInnerBorderBlueColor;  // 按己方阵营上色
}   // 结束 GetMapInnerBorderColor

// 功能：返回地图外边框颜色，未识别阵营时给中性灰。
ImU32 GetMapOuterBorderColor(bool team_known, bool is_red_team) {
    if (!team_known) {                                            // 尚未确定己方阵营
        return kMapOuterBorderUnknownColor;  // 阵营未知
    }   // 结束 team_known 判断
    return is_red_team ? kMapOuterBorderRedColor : kMapOuterBorderBlueColor; // 按己方阵营上色
}   // 结束 GetMapOuterBorderColor

// 功能：从完整地图纹理中裁剪出小地图区域对应的 UV 矩形。
// 思路：用固定的纹理像素坐标除以纹理尺寸得到归一化 UV，并 clamp 到 [0,1]。
std::pair<ImVec2, ImVec2> GetMiniMapUvRect(const ImVec2 &texture_size) {
    constexpr float kMiniMapTextureLeft = 45.0f;  // 小地图在纹理中的左边界（像素）
    constexpr float kMiniMapTextureTop = 31.0f;   // 小地图在纹理中的上边界（像素）
    constexpr float kMiniMapTextureRight = 1226.0f;   // 小地图在纹理中的右边界（像素）
    constexpr float kMiniMapTextureBottom = 683.0f;   // 小地图在纹理中的下边界（像素）

    const float width = std::max(texture_size.x, 1.0f);  // 防除零
    const float height = std::max(texture_size.y, 1.0f); // 防除零（纹理高度）
    const ImVec2 uv_min(std::clamp(kMiniMapTextureLeft / width, 0.0f, 1.0f),   // 裁剪区左上角 UV：像素坐标除以纹理宽度并夹紧到 [0,1]
                        std::clamp(kMiniMapTextureTop / height, 0.0f, 1.0f));  // 裁剪区左上角 UV
    const ImVec2 uv_max(std::clamp(kMiniMapTextureRight / width, 0.0f, 1.0f),   // 裁剪区右下角 UV：同理按纹理宽高归一化
                        std::clamp(kMiniMapTextureBottom / height, 0.0f, 1.0f));  // 裁剪区右下角 UV
    return {uv_min, uv_max};   // 把左上/右下 UV 作为 pair 返回给调用方贴图
}   // 结束 GetMiniMapUvRect

}// namespace

// 功能：绘制小地图（背景/单位/路径），并处理标记模式点击下发坐标。详见头文件。
// 思路：用窗口 DrawList 绘制底图与单位，外边框用前景 DrawList；坐标按 kMap 尺寸线性映射到屏幕。
void ShowMiniMap(const State &state,
                 RoboGo::io::MqttClient &mqtt_client,   // MQTT 客户端：标记模式下点击地图需要用它下发坐标
                 const ImTextureRef mini_map_texture_ref,   // 小地图底图纹理引用（ImTextureRef，可为无效表示尚未加载）
                 const ImVec2 &mini_map_texture_size) {   // 底图纹理的像素尺寸，用于把裁剪像素换算成 UV
    // 无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings; // 浮动覆盖层：无装饰/不可移动/无背景
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));  // 取消内边距，让地图铺满窗口

    if (!ImGui::Begin("MiniMap", nullptr, window_flags)) {           // 开窗口（被裁剪则跳过）
        ImGui::PopStyleVar();  // 提前退出也要恢复样式
        ImGui::End();                                                // 结束窗口
        return;                                                      // 提前返回
    }   // 结束 Begin 失败分支

    ImDrawList *draw_list = ImGui::GetWindowDrawList();  // 窗口层绘制列表（绘制地图内容）
    ImDrawList *foreground_draw_list = ImGui::GetForegroundDrawList();  // 前景层，用于绘制外边框使其不被遮挡
    ImVec2 window_pos = ImGui::GetWindowPos();                        // 窗口左上角屏幕坐标
    ImVec2 window_size = ImGui::GetWindowSize();                      // 窗口尺寸
    ImVec2 map_min = window_pos;  // 地图区域左上角
    ImVec2 map_max(window_pos.x + window_size.x, window_pos.y + window_size.y);  // 地图区域右下角
    const float scale = ImGui::GetScale();  // DPI 缩放，保证不同缩放下视觉一致
    const bool marker_mode_active = is_marker_mode();  // 是否处于标记模式
    // 判断己方阵营：通过 robot_static_status 的 robot_id 判断（1-99 为红方，100+ 为蓝方）
    bool is_red_team = false;                                        // 是否红方（默认否）
    bool team_known = false;                                         // 是否已确定阵营（默认否）

    if (state.robot_static_status.has_value() && state.robot_static_status->has_robot_id()) { // 已有 robot_id 信息
        is_red_team = state.robot_static_status->robot_id() < 100;  // 小于 100 为红方
        team_known = true;  // 已能确定阵营
    }   // 结束阵营判断

    // 添加不可见按钮使小地图可交互
    ImGui::PushID("MiniMapInteractive");  // 隔离交互控件 ID，避免冲突
    ImGui::InvisibleButton("minimap_area", window_size);  // 覆盖整个地图，捕获鼠标事件
    ImGui::PopID();   // 弹出交互控件的 ID 作用域，恢复外层 ID 栈

    // 绘制地图背景
    float outer_border_gap = 3.0f * scale;  // 外边框与内容间的间隙
    float outer_border_thickness = 3.0f * scale;  // 外边框线宽
    ImVec2 outer_border_min(map_min.x - outer_border_gap - outer_border_thickness * 0.5f,   // 外边框左上角：内容区左上再外扩间隙与半个线宽（线宽以中心线绘制）
                            map_min.y - outer_border_gap - outer_border_thickness * 0.5f);  // 外边框左上（含线宽补偿）
    ImVec2 outer_border_max(map_max.x + outer_border_gap + outer_border_thickness * 0.5f,   // 外边框右下角：同理右下外扩
                            map_max.y + outer_border_gap + outer_border_thickness * 0.5f); // 外边框右下（含线宽补偿）

    draw_list->AddRectFilled(map_min, map_max, kMapBackgroundColor);  // 先铺底色
    if (mini_map_texture_ref.GetTexID() != ImTextureID_Invalid && mini_map_texture_size.x > 0.0f && mini_map_texture_size.y > 0.0f) { // 底图有效才贴图
        const auto [uv_min, uv_max] = GetMiniMapUvRect(mini_map_texture_size);  // 取底图裁剪 UV
        draw_list->AddImage(mini_map_texture_ref, map_min, map_max, uv_min, uv_max);  // 贴底图
        draw_list->AddRectFilled(map_min, map_max, kMapOverlayColor);  // 叠加半透明暗色，提升前景可读性
    }   // 结束底图绘制条件
    draw_list->AddRect(map_min, map_max, GetMapInnerBorderColor(team_known, is_red_team));  // 内容区内边框
    // 外边框绘制在地图内容区域之外，避免改变地图坐标映射和点击标记范围。
    foreground_draw_list->AddRect(outer_border_min,
                                  outer_border_max,   // 外边框右下角（含线宽补偿）
                                  marker_mode_active ? kMapMarkerBorderColor : GetMapOuterBorderColor(team_known, is_red_team),  // 标记模式高亮边框
                                  0.0f,                                  // 圆角（无）
                                  outer_border_thickness,                 // 线宽
                                  0);                                    // 线段数（默认）

    // 绘制单位位置
    float map_content_width = window_size.x;  // 内容区宽，用于坐标映射
    float map_content_height = window_size.y;                       // 内容区高，用于坐标映射
    const char *text_begin, *text_end;  // 文本绘制区间指针

    // 只有确定己方阵营后才显示机器人位置
    if (team_known && state.radar_info_to_client.has_value()) {
        const auto &radar_info = *state.radar_info_to_client;   // 取雷达信息的常量引用，避免多次拷贝

        // robots_info 按顺序：对方 1,2,3,4,6,7 号 (索引 0-5)，己方 1,2,3,4,6,7 号 (索引 6-11)
        // 如果己方是红方，则对方是蓝方(100+)，如果己方是蓝方，则对方是红方(1-99)
        const std::array<uint32_t, 12> robot_ids = is_red_team
            ? std::array<uint32_t, 12>{101, 102, 103, 104, 106, 107, 1, 2, 3, 4, 6, 7}  // 红方视角：对方蓝方在前，己方红方在后
            : std::array<uint32_t, 12>{1, 2, 3, 4, 6, 7, 101, 102, 103, 104, 106, 107}; // 蓝方视角：对方红方在前，己方蓝方在后

        for (int i = 0; i < radar_info.robots_info_size(); ++i) {   // 遍历雷达上报的每个机器人槽位（数量应为 12）
            const auto &robot_info = radar_info.robots_info(i);  // 第 i 个机器人雷达信息
            uint32_t robot_id = robot_ids[i];  // 映射到实际 robot_id

            // 跳过未定位的机器人
            if (robot_info.target_pos_x() == 0 && robot_info.target_pos_y() == 0) {  // 坐标为 (0,0) 视为未定位
                continue;   // 未定位的单位直接跳过，不画图标
            }   // 结束未定位判断

            float screen_x = map_min.x + static_cast<float>(robot_info.target_pos_x()) / kMapWidth * map_content_width;  // 地图坐标 -> 屏幕 X
            float screen_y = map_min.y + static_cast<float>(robot_info.target_pos_y()) / kMapHeight * map_content_height;  // 地图坐标 -> 屏幕 Y

            ImU32 unit_color = robot_id < 100 ? kTeamRedColor : kTeamBlueColor;  // 红方/蓝方描边色
            ImU32 unit_fill_color = robot_id < 100 ? kTeamRedFillColor : kTeamBlueFillColor;  // 填充色
            float unit_radius = 10.0f * scale;  // 单位半径（随缩放）
            ImVec2 unit_center(screen_x, screen_y);                 // 单位在屏幕上的圆心

            // 绘制单位图标
            draw_list->AddCircleFilled(unit_center, unit_radius, unit_fill_color);  // 外圈实心
            draw_list->AddCircleFilled(unit_center, unit_radius - 2.0f * scale, kUnitCoreColor);  // 内芯深色，形成环

            // 特殊标识（高亮）显示
            if (robot_info.is_high_light() > 0) {  // 被高亮（如被锁定）时加金圈
                draw_list->AddCircle(unit_center, unit_radius + 3.0f * scale, kHighlightColor, 0, 2.0f * scale);   // 在被锁定/高亮的单位外再套一圈金色描边作为强调
            }   // 结束高亮判断
            draw_list->AddCircle(unit_center, unit_radius, unit_color, 0, 2.0f * scale);  // 外描边

            // 绘制编号
            ImFormatStringToTempBuffer(&text_begin, &text_end, "%d", robot_id < 100 ? robot_id : robot_id - 100);  // 编号去除阵营百位
            ImGui::PushFont(ImGui::GetFont(), 12);  // 固定 12px 字号显示编号
            ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end); // 编号文本尺寸
            draw_list->AddText(ImVec2(screen_x - text_size.x / 2, screen_y - text_size.y / 2), kUnitTextColor, text_begin, text_end);  // 居中绘制编号
            ImGui::PopFont();   // 恢复字号，避免影响后续文本绘制
        }   // 结束单个单位绘制循环
    }   // 结束单位位置绘制（需阵营已知且有雷达数据）

    // 绘制哨兵路径
    if (state.robot_path_plan_info.has_value()) {
        auto current_x = static_cast<float>(state.robot_path_plan_info->start_pos_x());  // 路径起点 X
        auto current_y = static_cast<float>(state.robot_path_plan_info->start_pos_y());  // 路径起点 Y
        float next_x;                                                 // 下一点 X（由起点累加偏移）
        float next_y;                                                 // 下一点 Y（由起点累加偏移）
        int point_count = std::min(state.robot_path_plan_info->offset_x_size(),   // 路径由起点+若干相对偏移组成，取两组偏移的较小长度防越界
                                   state.robot_path_plan_info->offset_y_size());  // 取两组偏移中较小者，避免越界
        for (int i = 0; i < point_count; i++) {   // 逐段遍历路径偏移点
            next_x = current_x + static_cast<float>(state.robot_path_plan_info->offset_x(i));  // 累加偏移得到下一点
            next_y = current_y + static_cast<float>(state.robot_path_plan_info->offset_y(i));   // 下一点 Y = 当前点 Y + 本段 y 偏移（相对路径累加）

            float x1 = map_min.x + current_x / kMapWidth * map_content_width;  // 当前点屏幕坐标
            float y1 = map_min.y + current_y / kMapHeight * map_content_height;   // 当前点屏幕 Y：地图坐标按内容区高度线性映射
            float x2 = map_min.x + next_x / kMapWidth * map_content_width;  // 下一点屏幕坐标
            float y2 = map_min.y + next_y / kMapHeight * map_content_height;   // 下一点屏幕 Y：同样按内容区高度映射

            draw_list->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), kPathColor, 2.0f * scale);  // 逐段连线绘制路径

            current_x = next_x;  // 推进到下一节点
            current_y = next_y;   // 当前点推进到下一点，作为下一段的起点
        }   // 结束路径分段循环
    }   // 结束哨兵路径绘制

    // 处理标记模式下的点击事件
    if (is_marker_mode() && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {  // 标记模式 + 悬停 + 左键
        ImGuiIO &io = ImGui::GetIO();   // 取 ImGui IO，读取全局鼠标状态
        ImVec2 mouse_pos = io.MousePos;   // 鼠标在屏幕空间的坐标（ImGui 坐标系与窗口坐标系一致）

        // 将屏幕坐标转换为地图坐标
        float map_x = (mouse_pos.x - map_min.x) / map_content_width * kMapWidth;  // 屏幕 -> 地图 X
        float map_y = (mouse_pos.y - map_min.y) / map_content_height * kMapHeight;  // 屏幕 -> 地图 Y

        send_map_marker(mqtt_client, map_x, map_y);  // 经 MQTT 下发标记坐标
    }   // 结束标记模式点击处理

    ImGui::End();   // 结束小地图窗口（与 Begin 配对）
    ImGui::PopStyleVar();  // 恢复窗口内边距样式
}   // 结束 ShowMiniMap
