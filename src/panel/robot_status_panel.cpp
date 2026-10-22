// robot_status_panel.cpp：己方机器人状态面板的实现。
#include "robot_status_panel.h"

// 功能：绘制一个指定颜色与尺寸的比例进度条。
// 参数：bar_color 进度条颜色；ratio 进度比例(0~1)；width/height 尺寸。
void ShowProgressBar(const ImVec4 &bar_color, float ratio, float width, float height) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);  // 临时设置进度条颜色
    ImGui::ProgressBar(ratio, ImVec2(width, height), "");  // 空标签避免显示百分比文本
    ImGui::PopStyleColor();
    ImGui::Spacing();  // 条与条之间留白
}

// 功能：展示己方机器人血量/能量/经验/发弹量/模式指示灯/设备连接状态。详见头文件。
// 思路：优先读取静态+动态状态，缺失时回退为全零；各项用彩色文本+进度条呈现。
void ShowRobotStatusPanel(const State &state) {
    // 无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("VehicleStatusPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    // 获取可用内容区域大小
    ImVec2 content_avail = ImGui::GetContentRegionAvail();  // 内容区宽度，用于进度条满宽
    float progress_bar_height = 4.0f;  // 进度条高度

    // Panel 标题
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));  // 白色标题
    ImGui::Text("机器人状态");
    ImGui::PopStyleColor();
    ImGui::Separator();  // 分隔线
    ImGui::Spacing();

    // TODO show buffer energy bar
    uint32_t current_health, max_health, current_energy, max_energy, current_experience, experience_for_upgrade, level;
    float health_ratio, energy_ratio, experience_ratio;
    if (state.robot_static_status.has_value() && state.robot_dynamic_status.has_value()) {
        current_health = state.robot_dynamic_status->current_health();  // 当前血量
        max_health = state.robot_static_status->max_health();  // 最大血量
        health_ratio = static_cast<float>(current_health) / static_cast<float>(max_health);  // 血量比例
        current_energy = state.robot_dynamic_status->current_chassis_energy();  // 当前底盘能量
        max_energy = state.robot_static_status->max_chassis_energy();
        energy_ratio = static_cast<float>(current_energy) / static_cast<float>(max_energy);
        current_experience = state.robot_dynamic_status->current_experience();  // 当前经验
        experience_for_upgrade = state.robot_dynamic_status->experience_for_upgrade();  // 升级所需经验
        level = state.robot_static_status->level();  // 等级
        experience_ratio = static_cast<float>(current_experience) / static_cast<float>(experience_for_upgrade);
    } else {  // 状态缺失时全部置零，避免异常
        current_health = 0;
        max_health = 0;
        current_energy = 0;
        max_energy = 0;
        current_experience = 0;
        experience_for_upgrade = 0;
        level = 0;
        health_ratio = 0.0f;
        energy_ratio = 0.0f;
        experience_ratio = 0.0f;
    }

    // 血条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));  // 红色文本
    ImGui::Text("血量: %d/%d", current_health, max_health);
    ImGui::PopStyleColor();
    ShowProgressBar(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), health_ratio, content_avail.x, progress_bar_height);  // 红色血条

    // 能量条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.7f, 1.0f, 1.0f));  // 蓝色文本
    ImGui::Text("能量: %d/%d", current_energy, max_energy);
    ImGui::PopStyleColor();
    ShowProgressBar(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), energy_ratio, content_avail.x, progress_bar_height);  // 蓝色能量条

    // 经验条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));  // 金色文本
    ImGui::Text("经验: %d/%d (等级: %d)", current_experience, experience_for_upgrade, level);
    ImGui::PopStyleColor();
    ShowProgressBar(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), experience_ratio, content_avail.x, progress_bar_height);  // 金色经验条

    // 允许发弹量
    uint32_t remaining_ammo;
    if (state.robot_dynamic_status.has_value()) {
        remaining_ammo = state.robot_dynamic_status->remaining_ammo();  // 剩余可发弹数
    } else {
        remaining_ammo = 0;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 1.0f, 1.0f));  // 浅蓝文本
    ImGui::Text("允许发弹量: %d", remaining_ammo);
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // 指示灯
    bool is_auto_aim = state.auto_aim;  // 自瞄开关
    bool is_gyro = state.gyro;  // 小陀螺开关
    bool is_deploy_mode;
    if (state.deploy_mode_status_sync.has_value()) {
        is_deploy_mode = state.deploy_mode_status_sync->status() == 1;  // 部署模式(状态为 1)
    } else {
        is_deploy_mode = false;
    }

    float text_width1 = ImGui::CalcTextSize("指示灯: ").x;
    float text_width2 = ImGui::CalcTextSize("自瞄").x;
    float text_width3 = ImGui::CalcTextSize("小陀螺").x;
    float text_width4 = ImGui::CalcTextSize("部署模式").x;
    float indicator_spacing = (content_avail.x - text_width1 - text_width2 - text_width3 - text_width4) / 4.0f;  // 在四项间均分剩余宽度
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "指示灯: ");  // 标签
    ImGui::SameLine(text_width1 + indicator_spacing);  // 首项后偏移，开始排列指示灯
    ImGui::TextColored(is_auto_aim ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "自瞄");  // 开启绿/关闭灰
    ImGui::SameLine(text_width1 + text_width2 + indicator_spacing * 2);
    ImGui::TextColored(is_gyro ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "小陀螺");
    ImGui::SameLine(text_width1 + text_width2 + text_width3 + indicator_spacing * 3);
    ImGui::TextColored(is_deploy_mode ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "部署模式");
    ImGui::Spacing();

    // 设备连接状态
    const char *module_names[] = {  // 11 个模块的中文名（与下方状态数组一一对应）
            "主控", "电源管理", "图传", "RFID", "UWB",
            "装甲板", "小弹丸", "大弹丸", "超级电容", "灯条",
            "激光检测"};

    ImGui::SameLine(0, 0);  // 与标题同行紧接
    ImGui::TextColored(ImVec4(0.8f, 0.4f, 1.0f, 1.0f), "未连接设备: ");  // 紫色标签

    if (state.robot_module_status.has_value()) {
        uint32_t module_status[] = {  // 各模块连接状态（1=已连接，非 1=未连接）
                state.robot_module_status->main_controller(),
                state.robot_module_status->power_manager(),
                state.robot_module_status->video_transmission(),
                state.robot_module_status->rfid(),
                state.robot_module_status->uwb(),
                state.robot_module_status->armor(),
                state.robot_module_status->small_shooter(),
                state.robot_module_status->big_shooter(),
                state.robot_module_status->capacitor(),
                state.robot_module_status->light_strip(),
                state.robot_module_status->laser_detection_module()};
        static_assert(std::size(module_names) == std::size(module_status));  // 编译期保证两数组长度一致

        ImGui::SameLine(0, 0);
        ImGuiTextBuffer buffer;
        for (size_t i = 0; i < std::size(module_names); i++) {
            if (module_status[i] != 1) {  // 仅收集未连接的模块
                if (!buffer.empty()) {
                    buffer.append(", ");  // 多模块用逗号分隔
                }
                buffer.appendf("%s", module_names[i]);
            }
        }
        if (buffer.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "（无）");  // 全部已连接
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", buffer.c_str());  // 标红列出未连接项
        }
    } else {
        ImGui::SameLine(0, 0);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "未知");  // 无模块状态数据时
    }

    ImGui::End();
}
