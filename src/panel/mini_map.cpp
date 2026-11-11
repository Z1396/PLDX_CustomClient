// mini_map.cpp：战场小地图面板的实现。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_internal.h"
// [/pch-split]
#include "mini_map.h"
#include "keyboard_mouse_state_manager.h"   // 标记模式开关（is_marker_mode）与指挥台状态
#include "app/team_util.h"                  // 共享阵营/雷达工具：WeAreRed / RadarRobotIds
#include "ui/theme.h"                       // 2027 统一视觉令牌

// 无名命名空间：小地图内部使用的坐标/尺寸常量、边框取色、UV 裁剪等辅助函数，不对外暴露。
namespace {

// 地图逻辑尺寸（与雷达坐标系一致，单位为地图坐标），用于坐标换算
constexpr float kMapWidth = 280.0f;
constexpr float kMapHeight = 150.0f;   // 地图逻辑高度：雷达坐标系 y 方向跨度，用于屏幕 Y 线性映射

// 2027 UI 改版：队色降饱和；外框 alpha 220→70（去掉刺眼红框）；底图蒙层加深，
// 让灰白 CAD 底图与深色界面统一到同一套语言。
constexpr ImU32 kTeamRedColor = RoboGo::ui::kTeamRed;                       // 红方单位描边
constexpr ImU32 kTeamBlueColor = RoboGo::ui::kTeamBlue;                      // 蓝方单位描边
constexpr ImU32 kTeamRedFillColor = RoboGo::ui::WithAlpha(RoboGo::ui::kTeamRed, 72);
constexpr ImU32 kTeamBlueFillColor = RoboGo::ui::WithAlpha(RoboGo::ui::kTeamBlue, 72);

constexpr ImU32 kMapBackgroundColor = RoboGo::ui::kSurface0;                // 贴图缺失时的底
constexpr ImU32 kMapOverlayColor = IM_COL32(8, 12, 18, 96);                 // 压暗底图（原 alpha 72）
constexpr ImU32 kMapInnerBorderRedColor = RoboGo::ui::WithAlpha(RoboGo::ui::kTeamRed, 40);
constexpr ImU32 kMapInnerBorderBlueColor = RoboGo::ui::WithAlpha(RoboGo::ui::kTeamBlue, 40);
constexpr ImU32 kMapInnerBorderUnknownColor = IM_COL32(170, 178, 190, 40);
constexpr ImU32 kMapOuterBorderRedColor = RoboGo::ui::WithAlpha(RoboGo::ui::kTeamRed, 70);
constexpr ImU32 kMapOuterBorderBlueColor = RoboGo::ui::WithAlpha(RoboGo::ui::kTeamBlue, 70);
constexpr ImU32 kMapOuterBorderUnknownColor = IM_COL32(170, 178, 190, 70);
constexpr ImU32 kMapMarkerBorderColor = RoboGo::ui::kWarning;                // 标记模式高亮边框

constexpr ImU32 kUnitCoreColor = RoboGo::ui::kSurface0;                      // 单位内芯色
constexpr ImU32 kUnitTextColor = RoboGo::ui::kTextPrimary;                    // 单位编号文字色
constexpr ImU32 kHighlightColor = RoboGo::ui::kWarning;                       // 被锁定标识色

// 标记点相关配色：与"标记模式"边框同属警示语义，统一降饱和。
constexpr ImU32 kMarkerPinTextColor = IM_COL32(232, 220, 196, 255);          // 标记点坐标文字色
constexpr ImU32 kMarkerHintBackground = IM_COL32(32, 28, 20, 210);          // 标记模式提示条底色
constexpr ImU32 kMarkerHintTextColor = RoboGo::ui::kWarning;                  // 标记模式提示条文字色
constexpr ImU32 kMarkerCrosshairColor = RoboGo::ui::WithAlpha(RoboGo::ui::kWarning, 190); // 标记模式准星色

// 标记点保留时长与淡出时长：下发后留在地图上 8 秒，最后 2 秒渐隐，避免"点完就什么都不剩"。
constexpr auto kMarkerVisibleDuration = std::chrono::seconds(8);
constexpr auto kMarkerFadeDuration = std::chrono::seconds(2);

// 最近一次本地下发的标记点。放在小地图内部而不是放进 State：
// 它是纯 UI 反馈（"我刚才点的那一下发到哪了"），不参与任何业务判断，也不该被网络线程影响。
struct LocalMapMarker {
    bool valid = false;                                   // 是否有点过标记
    float map_x = 0.0f;                                   // 地图坐标 X（0~kMapWidth）
    float map_y = 0.0f;                                   // 地图坐标 Y（0~kMapHeight）
    std::chrono::steady_clock::time_point send_time{};     // 下发时刻（用于计时淡出）
};

LocalMapMarker g_local_map_marker;   // 全局唯一实例：本地最近一次标记点

/**
 * @brief 返回地图内边框颜色，未识别阵营时给中性灰。
 */
ImU32 GetMapInnerBorderColor(bool team_known, bool is_red_team) {
    if (!team_known) {                                            // 尚未确定己方阵营
        return kMapInnerBorderUnknownColor;  // 阵营未知
    }
    return is_red_team ? kMapInnerBorderRedColor : kMapInnerBorderBlueColor;  // 按己方阵营上色
}

/**
 * @brief 返回地图外边框颜色，未识别阵营时给中性灰。
 */
ImU32 GetMapOuterBorderColor(bool team_known, bool is_red_team) {
    if (!team_known) {                                            // 尚未确定己方阵营
        return kMapOuterBorderUnknownColor;  // 阵营未知
    }
    return is_red_team ? kMapOuterBorderRedColor : kMapOuterBorderBlueColor; // 按己方阵营上色
}

/**
 * @brief 返回小地图底图的 UV 矩形。
 * @note 说明：底图已换成 2027 官方场地资源包里的标准图（2800×1500，100 px/m，整幅正好 28×15 m、无白边），所以 UV 直接取满整张纹理，不需要任何裁剪。原来那张
 *        1285×711 的 2026 旧图带黑边，只能靠 (45,31)-(1226,683) 四个手调像素常量硬裁；裁出来 1181×652，比例 1.811，而小地图窗口按 28:15=1.867
 *        排——底图被纵向拉伸 3%，地形特征和 map_x/map_y 坐标对不上，全场高度方向上边缘能差约 0.45 m。换图后这类"手调数字 + 隐式变形"全部消失。
 */
std::pair<ImVec2, ImVec2> GetMiniMapUvRect(const ImVec2 &texture_size) {
    (void) texture_size;  // 整幅取满，不再依赖纹理尺寸
    return {ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f)};  // 左上 → 右下，覆盖整张底图
}

}  // namespace

/**
 * @brief 绘制小地图（背景/单位/路径），并处理标记模式点击下发坐标。详见头文件。
 * @note 思路：用窗口 DrawList 绘制底图与单位，外边框用前景 DrawList；坐标按 kMap 尺寸线性映射到屏幕。
 */
namespace {

// 地图区域：屏幕坐标下的左上角与尺寸。
struct MapView {
    ImVec2 min;
    ImVec2 size;
};

// 小地图用到的两张纹理。
struct MapTextures {
    ImTextureRef base;
    ImVec2 base_size;
    ImTextureRef terrain;
    ImVec2 terrain_size;
};

// 底图 + 地形层 + 内外边框。
void DrawMapLayers(ImDrawList *draw_list, ImDrawList *foreground_draw_list, const MapView &view,
                   const MapTextures &textures, float scale, bool terrain_enabled,
                   bool team_known, bool is_red_team, bool marker_mode_active) {
    const ImVec2 map_min = view.min;
    const ImVec2 map_max(view.min.x + view.size.x, view.min.y + view.size.y);

    const float outer_border_gap = 3.0f * scale;            // 外边框与内容间的间隙
    const float outer_border_thickness = 1.5f * scale;      // 外边框线宽（由 3px 收细，配合低 alpha 去掉刺眼红框）
    const ImVec2 outer_border_min(map_min.x - outer_border_gap - outer_border_thickness * 0.5f,
                                  map_min.y - outer_border_gap - outer_border_thickness * 0.5f);
    const ImVec2 outer_border_max(map_max.x + outer_border_gap + outer_border_thickness * 0.5f,
                                  map_max.y + outer_border_gap + outer_border_thickness * 0.5f);

    draw_list->AddRectFilled(map_min, map_max, kMapBackgroundColor);  // 先铺底色
    if (textures.base.GetTexID() != ImTextureID_Invalid
        && textures.base_size.x > 0.0f && textures.base_size.y > 0.0f) {
        const auto [uv_min, uv_max] = GetMiniMapUvRect(textures.base_size);
        draw_list->AddImage(textures.base, map_min, map_max, uv_min, uv_max);
        draw_list->AddRectFilled(map_min, map_max, kMapOverlayColor);  // 叠加半透明暗色，提升前景可读性
        // 地形分层画在暗色蒙层之上，颜色才不会被压灰。叠加层自带 alpha、地面区域透明，直接贴满即可。
        if (terrain_enabled
            && textures.terrain.GetTexID() != ImTextureID_Invalid
            && textures.terrain_size.x > 0.0f && textures.terrain_size.y > 0.0f) {
            draw_list->AddImage(textures.terrain, map_min, map_max,
                                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                IM_COL32(255, 255, 255, 90));  // 降不透明度，避免高地橙黄盖过底图
        }
    }
    draw_list->AddRect(map_min, map_max, GetMapInnerBorderColor(team_known, is_red_team));
    // 外边框画在内容区之外，避免改变地图坐标映射与点击标记范围。
    foreground_draw_list->AddRect(outer_border_min, outer_border_max,
                                  marker_mode_active ? kMapMarkerBorderColor
                                                     : GetMapOuterBorderColor(team_known, is_red_team),
                                  0.0f, outer_border_thickness, 0);
}

// 雷达单位：圆环 + 编号，被高亮的再加一圈金边。
void DrawUnits(const State &state, ImDrawList *draw_list, const MapView &view, float scale,
               bool team_known, bool is_red_team) {
    if (!team_known || !state.radar_info_to_client.has_value()) {
        return;  // 阵营未知或没有雷达数据时不画单位
    }
    const auto &radar_info = *state.radar_info_to_client;
    // 槽位映射集中维护在 team_util::RadarRobotIds（与指挥台完全一致），按我方红蓝决定排序。
    const std::vector<uint32_t> robot_ids = RoboGo::app::team_util::RadarRobotIds(state, is_red_team);

    const char *text_begin, *text_end;
    for (int i = 0; i < radar_info.robots_info_size() && i < static_cast<int>(robot_ids.size()); ++i) {
        const auto &robot_info = radar_info.robots_info(i);
        const uint32_t robot_id = robot_ids[i];

        if (robot_info.target_pos_x() == 0 && robot_info.target_pos_y() == 0) {
            continue;  // 坐标 (0,0) 视为未定位
        }

        const float screen_x = view.min.x + static_cast<float>(robot_info.target_pos_x()) / kMapWidth * view.size.x;
        const float screen_y = view.min.y + static_cast<float>(robot_info.target_pos_y()) / kMapHeight * view.size.y;

        const bool is_red = RoboGo::app::team_util::IsRedRobotId(robot_id);
        const ImU32 unit_color = is_red ? kTeamRedColor : kTeamBlueColor;
        const ImU32 unit_fill_color = is_red ? kTeamRedFillColor : kTeamBlueFillColor;
        const float unit_radius = 10.0f * scale;
        const ImVec2 unit_center(screen_x, screen_y);

        draw_list->AddCircleFilled(unit_center, unit_radius, unit_fill_color);            // 外圈实心
        draw_list->AddCircleFilled(unit_center, unit_radius - 2.0f * scale, kUnitCoreColor);  // 内芯深色，形成环

        if (robot_info.is_high_light() > 0) {  // 被高亮（如被锁定）时加金圈
            draw_list->AddCircle(unit_center, unit_radius + 3.0f * scale, kHighlightColor, 0, 2.0f * scale);
        }
        draw_list->AddCircle(unit_center, unit_radius, unit_color, 0, 2.0f * scale);  // 外描边

        ImFormatStringToTempBuffer(&text_begin, &text_end, "%d",
                                   robot_id < 100 ? robot_id : robot_id - 100);  // 编号去掉阵营百位
        ImGui::PushFont(ImGui::GetFont(), 12);
        const ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end);
        draw_list->AddText(ImVec2(screen_x - text_size.x / 2, screen_y - text_size.y / 2),
                           kUnitTextColor, text_begin, text_end);
        ImGui::PopFont();
    }
}

// 本地标记点：把「刚才那一击发到哪了」画在地图上，8 秒后线性淡出。
// 之前只有网络发送、没有绘制，操作手点完看到「什么都没发生」，会误以为打点坏了。
void DrawLocalMarker(ImDrawList *draw_list, const MapView &view, float scale) {
    if (!g_local_map_marker.valid) {
        return;
    }
    const auto marker_age = std::chrono::steady_clock::now() - g_local_map_marker.send_time;
    if (marker_age > kMarkerVisibleDuration) {
        g_local_map_marker.valid = false;  // 顺手复位，避免每帧重复判断
        return;
    }

    float alpha = 1.0f;
    if (marker_age > kMarkerVisibleDuration - kMarkerFadeDuration) {
        const auto elapsed_in_fade = marker_age - (kMarkerVisibleDuration - kMarkerFadeDuration);
        alpha = 1.0f - static_cast<float>(elapsed_in_fade.count())
                        / static_cast<float>(kMarkerFadeDuration.count());
        alpha = std::clamp(alpha, 0.0f, 1.0f);
    }
    const int pin_alpha = static_cast<int>(alpha * 255.0f);
    const float pin_x = view.min.x + g_local_map_marker.map_x / kMapWidth * view.size.x;
    const float pin_y = view.min.y + g_local_map_marker.map_y / kMapHeight * view.size.y;
    const ImVec2 pin_center(pin_x, pin_y);
    // 标记点要「小到不挡地形」：地图本身只有 200 多像素高，半径 6px 会比单位图标还大、盖住半个通道。
    // 收到 2.5px，靠「亮点 + 细准线」表达位置而不是靠面积。
    const float pin_radius = 2.5f * scale;

    draw_list->AddCircleFilled(pin_center, pin_radius + 2.0f * scale, IM_COL32(255, 176, 64, pin_alpha / 5));
    draw_list->AddCircleFilled(pin_center, pin_radius, IM_COL32(255, 176, 64, pin_alpha));
    draw_list->AddCircle(pin_center, pin_radius + 1.5f * scale, IM_COL32(255, 214, 130, pin_alpha), 0, 1.0f * scale);
    draw_list->AddLine(ImVec2(pin_x - 6.0f * scale, pin_y), ImVec2(pin_x + 6.0f * scale, pin_y),
                       IM_COL32(255, 214, 130, pin_alpha), 1.0f * scale);
    draw_list->AddLine(ImVec2(pin_x, pin_y - 6.0f * scale), ImVec2(pin_x, pin_y + 6.0f * scale),
                       IM_COL32(255, 214, 130, pin_alpha), 1.0f * scale);

    const char *text_begin, *text_end;
    ImFormatStringToTempBuffer(&text_begin, &text_end, "标记 %d,%d",
                               static_cast<int>(g_local_map_marker.map_x),
                               static_cast<int>(g_local_map_marker.map_y));
    ImGui::PushFont(ImGui::GetFont(), 12);
    const ImVec2 pin_text_size = ImGui::CalcTextSize(text_begin, text_end);
    float pin_text_x = pin_x + 7.0f * scale;  // 默认画在标记点右侧
    if (pin_text_x + pin_text_size.x > view.min.x + view.size.x) {
        pin_text_x = pin_x - 7.0f * scale - pin_text_size.x;  // 右侧放不下就翻到左侧
    }
    draw_list->AddText(ImVec2(pin_text_x, pin_y - pin_text_size.y * 0.5f),
                       IM_COL32(255, 232, 190, pin_alpha), text_begin, text_end);
    ImGui::PopFont();
}

// 标记模式提示条 + 悬停准星：让「现在处于打点状态」这件事看得见。
void DrawMarkerHints(ImDrawList *draw_list, const MapView &view, float scale, bool marker_mode_active) {
    if (!marker_mode_active) {
        return;
    }
    const ImVec2 map_min = view.min;
    const ImVec2 map_max(view.min.x + view.size.x, view.min.y + view.size.y);
    const char *text_begin, *text_end;

    const float hint_height = ImGui::GetTextLineHeight() + 6.0f * scale;
    draw_list->AddRectFilled(map_min, ImVec2(map_max.x, map_min.y + hint_height), kMarkerHintBackground);
    ImFormatStringToTempBuffer(&text_begin, &text_end, "标记模式：点击地图下发坐标，在面板点\"退出\"结束");
    const ImVec2 hint_size = ImGui::CalcTextSize(text_begin, text_end);
    draw_list->AddText(ImVec2(map_min.x + (view.size.x - hint_size.x) * 0.5f, map_min.y + 3.0f * scale),
                       kMarkerHintTextColor, text_begin, text_end);

    if (!ImGui::IsItemHovered()) {
        return;  // 鼠标不在图上就不画准星
    }
    const ImVec2 hover_pos = ImGui::GetIO().MousePos;
    draw_list->AddLine(ImVec2(map_min.x, hover_pos.y), ImVec2(map_max.x, hover_pos.y), kMarkerCrosshairColor, 1.0f);
    draw_list->AddLine(ImVec2(hover_pos.x, map_min.y), ImVec2(hover_pos.x, map_max.y), kMarkerCrosshairColor, 1.0f);
    const float hover_map_x = (hover_pos.x - map_min.x) / view.size.x * kMapWidth;
    const float hover_map_y = (hover_pos.y - map_min.y) / view.size.y * kMapHeight;
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d,%d",
                               static_cast<int>(hover_map_x), static_cast<int>(hover_map_y));
    draw_list->AddText(ImVec2(hover_pos.x + 8.0f * scale, hover_pos.y + 6.0f * scale),
                       kMarkerPinTextColor, text_begin, text_end);
}

// 标记模式下点击地图：屏幕坐标 -> 地图坐标 -> MQTT 下发，并立刻记录本地标记点。
// 发送成功与否由网络层记日志，这里负责「点完马上看得见」。
void HandleMarkerClick(RoboGo::io::MqttClient &mqtt_client, const MapView &view) {
    const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
    const float map_x = (mouse_pos.x - view.min.x) / view.size.x * kMapWidth;
    const float map_y = (mouse_pos.y - view.min.y) / view.size.y * kMapHeight;

    send_map_marker(mqtt_client, map_x, map_y);

    g_local_map_marker.valid = true;
    g_local_map_marker.map_x = std::clamp(map_x, 0.0f, kMapWidth);
    g_local_map_marker.map_y = std::clamp(map_y, 0.0f, kMapHeight);
    g_local_map_marker.send_time = std::chrono::steady_clock::now();  // 用于 8 秒后自动淡出
}

}  // namespace

void ShowMiniMap(const State &state,
                 RoboGo::io::MqttClient &mqtt_client,   // 标记模式下点击地图需要用它下发坐标
                 const ImTextureRef mini_map_texture_ref,
                 const ImVec2 &mini_map_texture_size,
                 const ImTextureRef mini_map_terrain_texture_ref,
                 const ImVec2 &mini_map_terrain_texture_size) {
    // 无装饰/不可移动/无背景/不保存设置的浮动窗口。
    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration
                                          | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground
                                          | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));  // 取消内边距，让地图铺满窗口

    if (!ImGui::Begin("MiniMap", nullptr, window_flags)) {
        ImGui::PopStyleVar();  // 提前退出也要恢复样式
        ImGui::End();
        return;
    }

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImDrawList *foreground_draw_list = ImGui::GetForegroundDrawList();  // 前景层画外边框，避免被遮挡
    const float scale = ImGui::GetScale();

    MapView view;
    view.min = ImGui::GetWindowPos();
    view.size = ImGui::GetWindowSize();

    // 地形分层开关：M 键在主循环统一处理（HUD 小地图与指挥台战术地图共用同一份状态）。
    const bool terrain_overlay_enabled = is_terrain_overlay_enabled();
    const bool marker_mode_active = is_marker_mode();

    // 阵营未知时不画单位（避免把敌我画反）。
    const bool team_known = state.robot_static_status.has_value()
                            && state.robot_static_status->has_robot_id();
    const bool is_red_team = team_known && RoboGo::app::team_util::WeAreRed(state);

    // 不可见按钮覆盖整个地图，用于捕获鼠标事件（后面的 IsItemHovered 依赖它）。
    ImGui::PushID("MiniMapInteractive");
    ImGui::InvisibleButton("minimap_area", view.size);
    ImGui::PopID();

    const MapTextures textures{mini_map_texture_ref, mini_map_texture_size,
                               mini_map_terrain_texture_ref, mini_map_terrain_texture_size};
    DrawMapLayers(draw_list, foreground_draw_list, view, textures, scale, terrain_overlay_enabled,
                  team_known, is_red_team, marker_mode_active);

    // 【2027 调整】哨兵轨迹（2.2.16）不再画在地图上：形状完全由数据决定，画在 200px 的小图上
    // 既容易和标记点混淆，对实际操作也没帮助。数据仍照常解析进 state，需要时随时可以恢复绘制。
    DrawUnits(state, draw_list, view, scale, team_known, is_red_team);
    DrawLocalMarker(draw_list, view, scale);
    DrawMarkerHints(draw_list, view, scale, marker_mode_active);

    // 标记模式 + 悬停 + 左键：下发打点坐标。
    if (marker_mode_active && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        HandleMarkerClick(mqtt_client, view);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}
