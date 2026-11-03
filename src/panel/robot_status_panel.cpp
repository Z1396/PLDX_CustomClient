// robot_status_panel.cpp：己方机器人状态面板的实现。
#include "robot_status_panel.h"

// 功能：绘制一个指定颜色与尺寸的比例进度条。
// 参数：bar_color 进度条颜色；ratio 进度比例(0~1)；width/height 尺寸。
void ShowProgressBar(const ImVec4 &bar_color, float ratio, float width, float height) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);  // 临时设置进度条颜色
    ImGui::ProgressBar(ratio, ImVec2(width, height), "");  // 空标签避免显示百分比文本
    ImGui::PopStyleColor();   // 恢复进度条配色，避免影响后续控件
    ImGui::Spacing();  // 条与条之间留白
}   // 结束 ShowProgressBar

// 功能：展示己方机器人血量/能量/经验/发弹量/模式指示灯/设备连接状态。详见头文件。
// 思路：优先读取静态+动态状态，缺失时回退为全零；各项用彩色文本+进度条呈现。
void ShowRobotStatusPanel(const State &state) {
    // 无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings; // 浮动覆盖层：无交互背景
    if (!ImGui::Begin("VehicleStatusPanel", nullptr, window_flags)) { // 开窗口（被裁剪则跳过）
        ImGui::End();                                                // 结束窗口
        return;                                                      // 提前返回
    }   // 结束 Begin 失败分支

    // 获取可用内容区域大小
    ImVec2 content_avail = ImGui::GetContentRegionAvail();  // 内容区宽度，用于进度条满宽
    float progress_bar_height = 4.0f;  // 进度条高度（统一）

    // Panel 标题
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));  // 白色标题
    ImGui::Text("机器人状态");   // 面板标题文字
    ImGui::PopStyleColor();   // 恢复默认文字颜色
    ImGui::Separator();  // 分隔线
    ImGui::Spacing();   // 标题与内容之间的留白

    // TODO show buffer energy bar
    uint32_t current_health, max_health, current_energy, max_energy, current_experience, experience_for_upgrade, level; // 待填充的血/能量/经验/等级
    float health_ratio, energy_ratio, experience_ratio;               // 三项比例（0~1）
    if (state.robot_static_status.has_value() && state.robot_dynamic_status.has_value()) {   // 静态状态（上限/等级）与动态状态（当前值）都有才计算比例
        current_health = state.robot_dynamic_status->current_health();  // 当前血量
        max_health = state.robot_static_status->max_health();  // 最大血量
        health_ratio = static_cast<float>(current_health) / static_cast<float>(max_health);  // 血量比例
        current_energy = 0;  // 2027 底盘能量字段已移除，置 0
        max_energy = state.robot_static_status->max_chassis_energy();   // 底盘能量上限
        energy_ratio = static_cast<float>(current_energy) / static_cast<float>(max_energy);   // 底盘能量比例 = 当前 / 上限
        current_experience = state.robot_dynamic_status->current_experience();  // 当前经验
        experience_for_upgrade = state.robot_dynamic_status->experience_for_upgrade();  // 升级所需经验
        level = state.robot_static_status->level();  // 等级
        experience_ratio = static_cast<float>(current_experience) / static_cast<float>(experience_for_upgrade);   // 经验比例 = 当前经验 / 升级所需经验
    } else {  // 状态缺失时全部置零，避免异常
        current_health = 0;                                          // 血量置零
        max_health = 0;                                              // 最大血量置零
        current_energy = 0;                                          // 能量置零
        max_energy = 0;                                              // 最大能量置零
        current_experience = 0;                                      // 经验置零
        experience_for_upgrade = 0;                                  // 升级所需经验置零
        level = 0;                                                   // 等级置零
        health_ratio = 0.0f;                                         // 血量比例置零
        energy_ratio = 0.0f;                                         // 能量比例置零
        experience_ratio = 0.0f;                                     // 经验比例置零
    }   // 结束状态读取分支

    // 血条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));  // 红色文本
    ImGui::Text("血量: %d/%d", current_health, max_health);   // 血量数值文本（当前/上限）
    ImGui::PopStyleColor();   // 恢复默认文字颜色
    ShowProgressBar(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), health_ratio, content_avail.x, progress_bar_height);  // 红色血条

    // 能量条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.7f, 1.0f, 1.0f));  // 蓝色文本
    ImGui::Text("能量: %d/%d", current_energy, max_energy);   // 能量数值文本
    ImGui::PopStyleColor();   // 恢复默认文字颜色
    ShowProgressBar(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), energy_ratio, content_avail.x, progress_bar_height);  // 蓝色能量条

    // 经验条
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));  // 金色文本
    ImGui::Text("经验: %d/%d (等级: %d)", current_experience, experience_for_upgrade, level);   // 经验数值文本，附带当前等级
    ImGui::PopStyleColor();   // 恢复默认文字颜色
    ShowProgressBar(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), experience_ratio, content_avail.x, progress_bar_height);  // 金色经验条

    // 允许发弹量
    uint32_t remaining_ammo;                                        // 剩余可发弹数（待填充）
    if (state.robot_dynamic_status.has_value()) {                     // 有动态状态时
        remaining_ammo = state.robot_dynamic_status->remaining_ammo();  // 剩余可发弹数
    } else {   // 无动态状态时无弹药数据
        remaining_ammo = 0;   // 允许发弹量按 0 处理
    }   // 结束弹药读取分支
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 1.0f, 1.0f));  // 浅蓝文本
    ImGui::Text("允许发弹量: %d", remaining_ammo);   // 允许发弹量数值文本
    ImGui::PopStyleColor();   // 恢复默认文字颜色
    ImGui::Spacing();   // 与下方指示灯区留白

    // 指示灯
    bool is_auto_aim = state.auto_aim;  // 自瞄开关
    bool is_gyro = state.gyro;  // 小陀螺开关
    float text_width1 = ImGui::CalcTextSize("指示灯: ").x;            // "指示灯: " 文本宽度
    float text_width2 = ImGui::CalcTextSize("自瞄").x;                // "自瞄" 文本宽度
    float text_width3 = ImGui::CalcTextSize("小陀螺").x;              // "小陀螺" 文本宽度
    float indicator_spacing = (content_avail.x - text_width1 - text_width2 - text_width3) / 3.0f;  // 在三项间均分剩余宽度
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "指示灯: ");  // 标签
    ImGui::SameLine(text_width1 + indicator_spacing);  // 首项后偏移，开始排列指示灯
    ImGui::TextColored(is_auto_aim ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "自瞄");  // 开启绿/关闭灰
    ImGui::SameLine(text_width1 + text_width2 + indicator_spacing * 2);   // 「自瞄」之后的第三个位置：累计三个已用宽度 + 3 份间距
    ImGui::TextColored(is_gyro ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5, 0.5, 0.5, 1), "小陀螺");   // 小陀螺指示灯：开启绿、关闭灰
    ImGui::Spacing();   // 指示灯与设备状态之间留白

    // 设备连接状态
    const char *module_names[] = {  // 10 个模块的中文名（与下方状态数组一一对应）
            "主控", "电源管理", "图传", "RFID", "UWB",   // 模块名第 1 行（与状态数组前 5 项对应）
            "装甲板", "小弹丸", "大弹丸", "超级电容", "灯条"};   // 模块名第 2 行（对应第 6~10 项）

    ImGui::SameLine(0, 0);  // 与标题同行紧接
    ImGui::TextColored(ImVec4(0.8f, 0.4f, 1.0f, 1.0f), "未连接设备: ");  // 紫色标签

    if (state.robot_module_status.has_value()) {   // 已收到模块连接状态
        uint32_t module_status[] = {  // 各模块连接状态（1=已连接，非 1=未连接）
                state.robot_module_status->main_controller(),   // 主控连接状态
                state.robot_module_status->power_manager(),   // 电源管理连接状态
                state.robot_module_status->video_transmission(),   // 图传连接状态
                state.robot_module_status->rfid(),   // RFID 连接状态
                state.robot_module_status->uwb(),   // UWB 定位连接状态
                state.robot_module_status->armor(),   // 装甲板连接状态
                state.robot_module_status->small_shooter(),   // 小弹丸发射机构连接状态
                state.robot_module_status->big_shooter(),   // 大弹丸发射机构连接状态
                state.robot_module_status->capacitor(),   // 超级电容连接状态
                state.robot_module_status->light_strip()};   // 灯条连接状态
        static_assert(std::size(module_names) == std::size(module_status));  // 编译期保证两数组长度一致

        ImGui::SameLine(0, 0);   // 紧跟标签同行继续输出未连接列表
        ImGuiTextBuffer buffer;                                      // 拼接未连接模块名的临时缓冲区
        for (size_t i = 0; i < std::size(module_names); i++) {   // 遍历全部 10 个模块
            if (module_status[i] == 1) {  // 状态 1 表示在线，跳过
                continue;   // 已连接模块不列入
            }   // 结束在线判断
            if (!buffer.empty()) {   // 不是第一个异常项时先补分隔符
                buffer.append(", ");  // 多模块用逗号分隔
            }   // 结束分隔符判断
            if (module_status[i] == 2) {   // 状态 2 表示"因安装不规范被视为离线"，与真正离线区分开
                buffer.appendf("%s(安装不规范)", module_names[i]);   // 标注安装不规范，便于现场排查装配问题
            } else {   // 状态 0 表示确实离线
                buffer.appendf("%s", module_names[i]);   // 把未连接模块名追加到缓冲区
            }   // 结束三态区分
        }   // 结束模块遍历
        if (buffer.empty()) {   // 没有任何未连接模块
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "（无）");  // 全部已连接
        } else {   // 存在未连接模块
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", buffer.c_str());  // 标红列出未连接项
        }   // 结束未连接列表输出
    } else {   // 尚未收到模块状态
        ImGui::SameLine(0, 0);   // 紧跟标签同行输出
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "未知");  // 无模块状态数据时
    }   // 结束模块状态分支

    // 2027 协议 2.2.12 第 11 项：modular_42mm = 是否拥有模块化 42mm 发射机构安装权限（仅重装有效，0 未拥有 / 1 拥有）。
    // 语义与其它 10 个模块的"在线/离线"不同（这里是权限而非连接状态），因此单独一行显示，不并入上面的未连接列表。
    if (state.robot_module_status.has_value()) {   // 已收到模块状态
        if (state.robot_module_status->modular_42mm() == 1) {   // 1 = 已获得安装权限
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "模块化42mm装配权限: 已获得");  // 绿色：可以装 42mm
        } else {   // 0 = 未获得（或该机器人不是重装）
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "模块化42mm装配权限: 未获得");  // 灰色：尚未解锁
        }   // 结束权限判断
    } else {   // 尚未收到模块状态
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "模块化42mm装配权限: 未知");  // 无数据时不臆测
    }   // 结束权限数据分支

    ImGui::End();                                                    // 结束窗口
}   // 结束 ShowRobotStatusPanel
