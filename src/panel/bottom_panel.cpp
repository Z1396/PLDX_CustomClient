// bottom_panel.cpp：底部信息条面板的实现。
#include "bottom_panel.h"

// 功能：在屏幕底部居中渲染一条信息条，汇总复活/装配/空中支援等状态文本。
// 思路：先根据各项状态构造带颜色的文本行，再按行数计算面板高度并贴底边绘制。
void ShowBottomPanel(const State &state) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();  // 获取主视口，用于贴底定位与全宽
    ImGuiStyle &style = ImGui::GetStyle();  // 读取全局样式，用于间距与内边距计算
    float font_size = ImGui::GetFontSize();  // 获取当前字号，用于估算面板高度

    // 单行状态：文本 + 颜色，便于统一渲染
    struct StatusLine {
        std::string text;  // 该行要显示的文字内容
        ImVec4 color;      // 该行文字颜色（按状态区分，如警告橙、错误红）
    };

    std::vector<StatusLine> status_lines;  // 收集所有待显示的状态行

    // 复活状态显示
    if (state.robot_respawn_status.has_value()) {
        const auto &respawn_status = *state.robot_respawn_status;  // 解引用可选复活状态
        if (respawn_status.is_pending_respawn()) {  // 仅当处于待复活阶段才显示进度
            std::string respawn_text = "复活进度: ";  // 拼接「当前/总」进度文本
            respawn_text += std::to_string(respawn_status.current_respawn_progress());
            respawn_text += "/";
            respawn_text += std::to_string(respawn_status.total_respawn_progress());
            status_lines.push_back({std::move(respawn_text), ImVec4(1.0f, 1.0f, 0.3f, 1.0f)});  // 黄色高亮
        }
    }

    // 装配状态显示
    if (state.tech_core_motion_state_sync.has_value()) {
        const auto &sync = *state.tech_core_motion_state_sync;

        // 判断是否在装配中：basic_state 为 2（运动中）或 3（已到达对应位姿）时表示在装配
        bool is_assembling = sync.basic_state() >= 2 && sync.basic_state() <= 3;

        if (is_assembling) {
            std::string status_text;

            // 根据 basic_state 构建状态描述
            if (sync.basic_state() == 2) {
                status_text = "科技核心移动中";  // 移动阶段
            } else if (sync.basic_state() == 3) {
                // 显示当前装配步骤状态
                std::vector<std::string> steps;
                if (sync.putin_state() == 1) steps.push_back("已放入");  // 放入完成
                if (sync.move_state() == 1) steps.push_back("已平移");   // 平移完成
                if (sync.rotate_state() == 1) steps.push_back("已旋转"); // 旋转完成

                if (steps.empty()) {
                    status_text = "等待装配";  // 尚未开始任一子步骤
                } else {
                    // 用「->」连接各已完成步骤，形成步骤链路
                    status_text = "装配中: " + std::accumulate(steps.begin(), steps.end(), std::string(),
                        [](const std::string &a, const std::string &b) {
                            return a.empty() ? b : a + " -> " + b;
                        });
                }

                // 显示剩余时间
                if (sync.remain_time_step() > 0) {
                    status_text += " (" + std::to_string(sync.remain_time_step()) + "s)";  // 追加剩余秒数
                }
            }

            status_lines.push_back({"工程装配状态: " + status_text, ImVec4(0.3f, 0.8f, 1.0f, 1.0f)});  // 蓝色
        }
    }

    if (state.air_support_status_sync.has_value()) {
        const auto &sync = *state.air_support_status_sync;  // 解引用可选空中支援状态
        std::string air_support_text = "空中支援: ";
        air_support_text += GetAirSupportStatusName(sync.airsupport_status());  // 支援主状态名
        air_support_text += " | 免费剩余 ";
        air_support_text += std::to_string(sync.left_time());  // 免费剩余时间
        air_support_text += "s | 激光";
        air_support_text += (sync.is_being_targeted() != 0 ? "照射中" : "未照射");  // 激光照射状态
        air_support_text += " | 发射机构";
        air_support_text += GetAirSupportShooterStatusName(sync.shooter_status());  // 发射机构状态名
        if (sync.cost_coins() > 0) {  // 仅在花费金币时追加显示
            air_support_text += " | 已花费 ";
            air_support_text += std::to_string(sync.cost_coins());
            air_support_text += " 金币";
        }

        ImVec4 color = ImVec4(0.7f, 0.75f, 0.85f, 1.0f);  // 默认灰色调
        if (sync.is_being_targeted() != 0 || sync.shooter_status() == 0 || sync.shooter_status() == 2) {
            color = ImVec4(1.0f, 0.55f, 0.25f, 1.0f);  // 被照射/待机/故障 -> 橙色警示
        } else if (sync.airsupport_status() == 1) {
            color = ImVec4(0.2f, 0.85f, 1.0f, 1.0f);  // 进行中 -> 青蓝色
        }
        status_lines.push_back({std::move(air_support_text), color});
    }

    // 计算空中支援反制冷却倒计时（基于自定义控制字节与当前时刻）
    const uint8_t air_support_counter_countdown =
            GetAirSupportCounterCooldownByte(state.air_support_custom_control, std::chrono::steady_clock::now());
    if (air_support_counter_countdown != kInactiveAirSupportCounterCooldown) {  // 仅在冷却激活时显示
        std::string counter_text = "空中支援反制倒计时: ";
        counter_text += std::to_string(air_support_counter_countdown);
        counter_text += "s";
        status_lines.push_back({std::move(counter_text), ImVec4(1.0f, 0.78f, 0.25f, 1.0f)});  // 橙色
    }

    size_t line_count = std::max<size_t>(1, status_lines.size());  // 至少保留 1 行高度
    float panel_height = font_size * static_cast<float>(line_count)
                         + style.ItemSpacing.y * static_cast<float>(line_count - 1)
                         + style.WindowPadding.y * 2;  // 由行数/间距/内边距累加得到面板总高

    // 无导航/无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
    ImGui::SetNextWindowPos(ImVec2(0, viewport->Size.y - panel_height), ImGuiCond_Always);  // 固定在屏幕底部
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, panel_height), ImGuiCond_Always);  // 全宽

    if (!ImGui::Begin("BottomPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    for (const auto &line: status_lines) {
        ImVec2 text_size = ImGui::CalcTextSize(line.text.c_str());  // 测量文本宽度以居中
        ImGui::SetCursorPosX((viewport->Size.x - text_size.x) / 2.0f);  // 水平居中
        ImGui::PushStyleColor(ImGuiCol_Text, line.color);  // 临时设置该行文本颜色
        ImGui::Text("%s", line.text.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::End();
}
