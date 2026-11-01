// header_panel.cpp：顶部状态栏面板的实现——绘制比赛阶段、倒计时、金币，以及前哨站/基地/双方各机器人的血量与允许发弹量。
// 属于「业务功能层-UI」。全部使用 ImGui 的 DrawList 直接绘制（不接收交互），由主窗口在每帧开头调用。
#include "header_panel.h"

// 顶部状态栏面板：绘制比赛阶段、倒计时、经济(金币)、前哨站/基地血条，
// 以及红蓝双方各机器人的血量与允许发弹量。全部使用 ImDrawList 直接绘制，不接收交互。

// 功能：把一段文本绘制到给定矩形的正中央（用于血条上的数值标签）。
// 思路：先用 CalcTextSize 量出文本尺寸，再以矩形中心减去半个文本尺寸得到左上角。
void AddTextInTheCenterOfRect(ImDrawList *draw_list, const char *text_begin, const char *text_end, float left, float top, float right, float bottom, ImU32 col) {
    ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end); // 计算文本宽高
    draw_list->AddText(ImVec2(left + (right - left) / 2 - text_size.x / 2, top + (bottom - top) / 2 - text_size.y / 2), col, text_begin, text_end); // 水平/垂直居中绘制
}   // 结束 AddTextInTheCenterOfRect

// 功能：把比赛结果状态码（0 平局 / 1 红胜 / 2 蓝胜）翻译为中文，未知返回 nullptr。
// 思路：用 switch 覆盖已知状态码，默认返回空指针表示无法识别。
const char *GetGameResultName(uint32_t game_result) {
    switch (game_result) {   // 按比赛结果码分支（0 平局 / 1 红方胜 / 2 蓝方胜）
        case 0:   // 结果码 0：平局
            return "平局";   // 返回平局文案
        case 1:   // 结果码 1：红方胜利
            return "红方胜利";   // 返回红方胜利文案
        case 2:   // 结果码 2：蓝方胜利
            return "蓝方胜利";   // 返回蓝方胜利文案
        default:   // 其他未定义的结果码
            return nullptr; // 未知结果码
    }   // 结束 switch 分支
}   // 结束 GetGameResultName

// 功能：绘制顶部状态栏：比赛阶段、倒计时、金币、前哨站/基地血条与己方/对方各机器人血量+弹药。
// 思路：以窗口绘制列表直接 AddText/AddRectFilled 绘制（不接收输入）；根据 robot_id 归属区分红蓝左右布局。
void ShowHeaderPanel(const State &state) {
    // 无可交互、无标题栏/边框、无背景、不可移动的覆盖层窗口标志。
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("HeaderWindow", nullptr, window_flags)) { // 窗口被裁剪/折叠时直接结束
        ImGui::End();   // 内容区被裁剪时仍要配对调用 End，否则窗口栈不平衡
        return;   // 本帧不绘制任何内容直接返回
    }   // 结束 Begin 失败分支
    const ImVec2 window_pos = ImGui::GetWindowPos(); // 窗口左上角屏幕坐标
    const ImVec2 window_size = ImGui::GetWindowSize(); // 窗口尺寸
    ImDrawList *draw_list = ImGui::GetWindowDrawList(); // 当前窗口的绘制列表
    const char *text_begin, *text_end; // 复用的临时文本起止指针

    // 从比赛状态中取出阶段、倒计时、暂停标志；无状态时用安全默认值。
    uint32_t current_stage;
    int32_t stage_countdown_sec;   // 当前阶段剩余秒数（有符号，兼容 -1 等占位值）
    bool is_paused;   // 是否处于技术暂停
    if (state.game_status.has_value()) {   // 有比赛状态数据时读取真实字段值
        current_stage = state.game_status->current_stage(); // 当前比赛阶段
        stage_countdown_sec = state.game_status->stage_countdown_sec(); // 当前阶段剩余秒数
        is_paused = state.game_status->is_paused(); // 是否技术暂停
        if (current_stage == 0) { // 未开始阶段不显示倒计时
            stage_countdown_sec = 0;   // 阶段 0（未开始）不显示倒计时，强制归零
        }   // 结束阶段 0 判断
    } else {   // 无比赛状态数据时走安全默认值
        current_stage = 0; // 无数据：视为未开始
        stage_countdown_sec = 0;   // 剩余秒数默认 0
        is_paused = false;   // 默认非暂停
    }   // 结束无数据分支

    // 比赛阶段显示
    ImGui::PushFont(ImGui::GetFont(), 14); // 小号字体
    const char *stage_text;   // 指向阶段显示文案（常量字面量或 stage_text_storage 的内部缓冲）
    std::string stage_text_storage; // 需要动态拼接的文本存储(如"比赛结算: 红方胜利")
    if (state.game_status.has_value()) {   // 有比赛状态才尝试把阶段码映射成中文
        if (is_paused) {   // 技术暂停优先级高于阶段名
            stage_text = "技术暂停中"; // 暂停优先于阶段显示
        } else {   // 未暂停：按阶段码逐一映射
            // 按阶段码映射中文描述。
            switch (current_stage) {
                case 0:   // 阶段 0：未开始
                    stage_text = "未开始比赛";   // 未开始比赛文案
                    break;   // 跳出 switch
                case 1:   // 阶段 1：准备阶段
                    stage_text = "准备阶段";   // 准备阶段文案
                    break;   // 跳出 switch
                case 2:   // 阶段 2：裁判系统自检
                    stage_text = "裁判系统自检";   // 自检阶段文案
                    break;   // 跳出 switch
                case 3:   // 阶段 3：五秒倒计时
                    stage_text = "五秒倒计时";   // 倒计时阶段文案
                    break;   // 跳出 switch
                case 4:   // 阶段 4：比赛进行中
                    stage_text = "比赛中";   // 比赛中文案
                    break;   // 跳出 switch
                case 5:   // 阶段 5：比赛结算（可能附带结果码）
                    // 结算阶段：若有结果码则拼接"比赛结算: xxx"。
                    if (state.game_status->has_game_result()) {
                        const char *game_result_name = GetGameResultName(state.game_status->game_result());   // 把结果码翻译成中文（无法识别时为 nullptr）
                        if (game_result_name != nullptr) {   // 结果码可识别才拼接带结果的文案
                            stage_text_storage = std::string("比赛结算: ") + game_result_name;   // 拼接「比赛结算: 红方胜利/蓝方胜利/平局」
                            stage_text = stage_text_storage.c_str(); // 指向拼接后的字符串
                            break;   // 使用拼接后的文案并跳出 switch
                        }   // 结束结果名判空
                    }   // 结束 has_game_result 判断
                    stage_text = "比赛结算中"; // 无结果码时的兜底
                    break;   // 无结果码时用兜底文案并跳出
                default:   // 未定义的阶段码
                    stage_text = "未知阶段";   // 未知阶段文案
                    break;   // 跳出 switch
            }   // 结束阶段映射 switch
        }   // 结束 is_paused 分支
    } else {   // 完全没有比赛状态（未连上裁判系统）
        stage_text = "未连接"; // 完全没有比赛状态数据
    }   // 结束阶段文案选择
    ImVec2 stage_text_size = ImGui::CalcTextSize(stage_text); // 阶段文本尺寸
    float stage_left = window_pos.x + (window_size.x - stage_text_size.x) / 2; // 水平居中
    float stage_top = window_pos.y + 12 * ImGui::GetScale() + 5 * ImGui::GetScale(); // 顶部留白
    draw_list->AddText(ImVec2(stage_left, stage_top), IM_COL32(255, 255, 255, 255), stage_text); // 白色绘制阶段文字
    ImGui::PopFont();   // 恢复之前的字号，避免影响后续绘制

    // 倒计时
    ImGui::PushFont(ImGui::GetFont(), 22); // 大号字体强调倒计时
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%02d:%02d", stage_countdown_sec / 60, stage_countdown_sec % 60); // 秒数格式化为 mm:ss
    ImVec2 countdown_text_size = ImGui::CalcTextSize(text_begin, text_end); // 倒计时文本尺寸
    float countdown_left = window_pos.x + (window_size.x - countdown_text_size.x) / 2; // 水平居中(左)
    float countdown_right = countdown_left + countdown_text_size.x; // 水平居中(右)
    float countdown_top = stage_top + stage_text_size.y + 10 * ImGui::GetScale(); // 位于阶段文字下方
    float countdown_bottom = countdown_top + 20 * ImGui::GetScale(); // 预留一行高度
    draw_list->AddText(ImVec2(countdown_left, countdown_top + (countdown_bottom - countdown_top - countdown_text_size.y) / 2), IM_COL32(255, 255, 255, 255), text_begin, text_end); // 垂直居中绘制
    ImGui::PopFont();   // 恢复原字号

    // 金币显示
    uint32_t gold = state.global_logistics_status.has_value() ? state.global_logistics_status->remaining_economy() : 0; // 剩余经济(金币)
    ImGui::PushFont(ImGui::GetFont(), 18); // 中号字体
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d", gold); // 数字转文本
    draw_list->AddText(ImVec2(window_pos.x + (window_size.x - ImGui::CalcTextSize(text_begin, text_end).x) / 2, countdown_bottom + 10 * ImGui::GetScale()), IM_COL32(255, 215, 0, 255), text_begin); // 金色绘制金币数
    ImGui::PopFont();   // 恢复原字号，避免影响后续绘制

    // 前哨站血条和基地血条
    ImU32 red_color = IM_COL32(250, 63, 64, 255); // 红方主色(不透明)
    ImU32 blue_color = IM_COL32(3, 187, 245, 255); // 蓝方主色(不透明)
    ImU32 red_color_translated = IM_COL32(250, 63, 64, 50); // 红方半透明(血条底槽)
    ImU32 blue_color_translated = IM_COL32(3, 187, 245, 50); // 蓝方半透明(血条底槽)
    float rounding = 15 * ImGui::GetScale(); // 圆角半径
    float base_health_horizontal_padding = 24 * ImGui::GetScale(); // 血条距屏幕边缘的内边距
    float outpost_length_ratio = 0.2f; // 前哨站血条占单侧宽度的比例
    float building_health_top = window_pos.y + 12 * ImGui::GetScale(); // 建筑血条顶部
    float building_health_bottom = building_health_top + 30 * ImGui::GetScale(); // 建筑血条底部(高度 30)
    float red_health_left = window_pos.x + base_health_horizontal_padding; // 红方血条区左边界
    float red_health_right = countdown_left - base_health_horizontal_padding; // 红方血条区右边界(倒计时左侧)
    float blue_health_left = countdown_right + base_health_horizontal_padding; // 蓝方血条区左边界(倒计时右侧)
    float blue_health_right = window_pos.x + window_size.x - base_health_horizontal_padding; // 蓝方血条区右边界
    // 红方：前哨站在最左，基地紧随其后；蓝方：基地在最左，前哨站在最右(镜像布局)。
    float red_outpost_health_left = red_health_left;
    float red_outpost_health_right = red_health_left + (red_health_right - red_health_left) * outpost_length_ratio - base_health_horizontal_padding / 2;   // 红方前哨站右边界：按比例切分后再让出半个内边距作为间隙
    float red_base_health_left = red_health_left + (red_health_right - red_health_left) * outpost_length_ratio + base_health_horizontal_padding / 2;   // 红方基地左边界：前哨站占 20% 后再加半个内边距
    float red_base_health_right = red_health_right;   // 红方基地右边界即红方区域最右
    float blue_outpost_health_left = blue_health_right - (blue_health_right - blue_health_left) * outpost_length_ratio + base_health_horizontal_padding / 2;   // 蓝方前哨站左边界（镜像：从右往左按比例切分）
    float blue_outpost_health_right = blue_health_right;   // 蓝方前哨站右边界即蓝方区域最右
    float blue_base_health_left = blue_health_left;   // 蓝方基地左边界即蓝方区域最左
    float blue_base_health_right = blue_health_right - (blue_health_right - blue_health_left) * outpost_length_ratio - base_health_horizontal_padding / 2;   // 蓝方基地右边界：扣掉前哨站所占比例与半个间隙
    uint32_t outpost_max_health = 1500; // 前哨站最大血量(固定值)
    uint32_t base_max_health = 5000; // 基地最大血量(固定值)
    uint32_t red_outpost_health, red_base_health, red_base_shield, blue_outpost_health, blue_base_health, blue_base_shield;   // 一次性声明红蓝双方的前哨站血量、基地血量与基地护盾
    // 依据我方 robot_id 判断红蓝归属：robot_id < 100 视为红方，否则蓝方；
    // 然后把我方数据与敌方数据分别填入对应颜色变量。
    if (state.global_unit_status.has_value() && state.robot_static_status.has_value()) {
        if (state.robot_static_status->robot_id() < 100) {   // robot_id < 100 视为红方：我方数据填入红色槽位
            red_outpost_health = state.global_unit_status->outpost_health(); // 我方(红)前哨站
            red_base_health = state.global_unit_status->base_health(); // 我方(红)基地血量
            red_base_shield = state.global_unit_status->base_shield(); // 我方(红)基地护盾
            blue_outpost_health = state.global_unit_status->enemy_outpost_health(); // 敌方(蓝)前哨站
            blue_base_health = state.global_unit_status->enemy_base_health(); // 敌方(蓝)基地血量
            blue_base_shield = state.global_unit_status->enemy_base_shield(); // 敌方(蓝)基地护盾
        } else {   // 我方为蓝方：红蓝槽位的数据来源互换
            red_outpost_health = state.global_unit_status->enemy_outpost_health(); // 我方为蓝：敌方数据给红
            red_base_health = state.global_unit_status->enemy_base_health();   // 敌方（红）基地血量
            red_base_shield = state.global_unit_status->enemy_base_shield();   // 敌方（红）基地护盾
            blue_outpost_health = state.global_unit_status->outpost_health(); // 我方数据给蓝
            blue_base_health = state.global_unit_status->base_health();   // 我方（蓝）基地血量
            blue_base_shield = state.global_unit_status->base_shield();   // 我方（蓝）基地护盾
        }   // 结束红蓝归属分支
    } else {   // 缺少单位状态或静态状态：无法判断归属
        // 缺少数据时全部置零(不显示血量)。
        red_outpost_health = 0;
        red_base_health = 0;   // 红方基地血量置零
        red_base_shield = 0;   // 红方基地护盾置零
        blue_outpost_health = 0;   // 蓝方前哨站血量置零
        blue_base_health = 0;   // 蓝方基地血量置零
        blue_base_shield = 0;   // 蓝方基地护盾置零
    }   // 结束血量取值

    // 绘制血条背景
    draw_list->AddRectFilled(ImVec2(red_outpost_health_left, building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color_translated, rounding, ImDrawFlags_RoundCornersLeft); // 红方前哨站底槽
    draw_list->AddRectFilled(ImVec2(red_base_health_left, building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color_translated, rounding, ImDrawFlags_RoundCornersLeft); // 红方基地底槽
    draw_list->AddRectFilled(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_right, building_health_bottom), blue_color_translated, rounding, ImDrawFlags_RoundCornersRight); // 蓝方前哨站底槽
    draw_list->AddRectFilled(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_right, building_health_bottom), blue_color_translated, rounding, ImDrawFlags_RoundCornersRight); // 蓝方基地底槽

    // 绘制血条填充
    // 红方血条从右端向左收缩(即缺失部分在左侧)，蓝方从左端向右收缩。
    if (red_outpost_health > 0) {
        draw_list->AddRectFilled(ImVec2(red_outpost_health_left + (red_outpost_health_right - red_outpost_health_left) * (1 - static_cast<float>(red_outpost_health) / static_cast<float>(outpost_max_health)), building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color, rounding, ImDrawFlags_RoundCornersLeft); // 红方前哨站血量填充
    }   // 结束红方前哨站填充
    if (red_base_health > 0) {   // 红方基地血量大于 0 才绘制填充（为 0 时只留底槽）
        draw_list->AddRectFilled(ImVec2(red_base_health_left + (red_base_health_right - red_base_health_left) * (1 - static_cast<float>(red_base_health) / static_cast<float>(base_max_health)), building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color, rounding, ImDrawFlags_RoundCornersLeft); // 红方基地血量填充
    }   // 结束红方基地填充
    if (blue_outpost_health > 0) {   // 蓝方前哨站血量大于 0 才绘制填充
        draw_list->AddRectFilled(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_left + (blue_outpost_health_right - blue_outpost_health_left) * (static_cast<float>(blue_outpost_health) / static_cast<float>(outpost_max_health)), building_health_bottom), blue_color, rounding, ImDrawFlags_RoundCornersRight); // 蓝方前哨站血量填充
    }   // 结束蓝方前哨站填充
    if (blue_base_health > 0) {   // 蓝方基地血量大于 0 才绘制填充
        draw_list->AddRectFilled(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_left + (blue_base_health_right - blue_base_health_left) * (static_cast<float>(blue_base_health) / static_cast<float>(base_max_health)), building_health_bottom), blue_color, rounding, ImDrawFlags_RoundCornersRight); // 蓝方基地血量填充
    }   // 结束蓝方基地填充

    // 绘制血条边框
    draw_list->AddRect(ImVec2(red_outpost_health_left, building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color, rounding, 1.0f, ImDrawFlags_RoundCornersLeft); // 红方前哨站描边
    draw_list->AddRect(ImVec2(red_base_health_left, building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color, rounding, 1.0f, ImDrawFlags_RoundCornersLeft); // 红方基地描边
    draw_list->AddRect(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_right, building_health_bottom), blue_color, rounding, 1.0f, ImDrawFlags_RoundCornersRight); // 蓝方前哨站描边
    draw_list->AddRect(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_right, building_health_bottom), blue_color, rounding, 1.0f, ImDrawFlags_RoundCornersRight); // 蓝方基地描边

    // 血条文本
    ImGui::PushFont(ImGui::GetFont(), 16); // 数值文本字号
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", red_outpost_health, outpost_max_health); // 红方前哨站 当前/上限
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, red_outpost_health_left, building_health_top, red_outpost_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255)); // 居中绘制
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", red_base_health + red_base_shield, base_max_health); // 红方基地 血量+护盾/上限
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, red_base_health_left, building_health_top, red_base_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));   // 红方基地「血量+护盾/上限」居中绘制
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", blue_outpost_health, outpost_max_health); // 蓝方前哨站
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, blue_outpost_health_left, building_health_top, blue_outpost_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));   // 蓝方前哨站「当前/上限」居中绘制
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", blue_base_health + blue_base_shield, base_max_health); // 蓝方基地 血量+护盾/上限
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, blue_base_health_left, building_health_top, blue_base_health_right, building_health_bottom, IM_COL32(255, 255, 255, 255));   // 蓝方基地「血量+护盾/上限」居中绘制
    ImGui::PopFont();   // 恢复原字号

    // 机器人血量条
    float robot_bar_top = building_health_bottom + 30 * ImGui::GetScale(); // 机器人血条顶部(建筑血条下方)
    float robot_bar_bottom = robot_bar_top + 4 * ImGui::GetScale(); // 血条本身只有 4px 高
    float red_robot_bar_left = red_health_left; // 红方机器人区左边界
    float red_robot_bar_right = red_health_right; // 红方机器人区右边界
    float blue_robot_bar_left = blue_health_left; // 蓝方机器人区左边界
    float blue_robot_bar_right = blue_health_right; // 蓝方机器人区右边界
    const char *robot_names[] = {"英雄", "工程", "步兵", "步兵", "步兵", "无人机", "哨兵", "飞镖", "雷达"}; // 按序号 1..9 的兵种名
    static_assert(sizeof(state.red_robots) / sizeof(state.red_robots[0]) == sizeof(robot_names) / sizeof(robot_names[0])); // 编译期校验兵种表与数组长度一致
    // 实际展示的机器人编号（不含 5 号废弃步兵；哨兵=7 飞镖=8 雷达=9 无血条只显示编号/弹药）。
    int ids[] = {1, 2, 3, 4, 6, 7, 8, 9};
    int ids_size = std::size(ids); // 展示项数量
    float robot_bar_spacing = 6.0f * ImGui::GetScale(); // 相邻血条间距
    ImU32 bg_color = IM_COL32(50, 50, 50, 255); // 血条底槽灰色
    // 单条血条宽度 = (总宽 - 所有间距) / 数量。
    float red_robot_bar_item_width = (red_robot_bar_right - red_robot_bar_left - robot_bar_spacing * static_cast<float>(ids_size - 1)) / static_cast<float>(ids_size);
    float blue_robot_bar_item_width = (blue_robot_bar_right - blue_robot_bar_left - robot_bar_spacing * static_cast<float>(ids_size - 1)) / static_cast<float>(ids_size);   // 蓝方单条血条宽度：与红方同算法（总宽减间距后按数量均分）
    ImGui::PushFont(ImGui::GetFont(), 12); // 机器人信息用小字号
    // 红方机器人：逐项绘制血条、编号+兵种名、血量、弹药。
    for (int i = 0; i < ids_size; i++) {
        int id = ids[i]; // 机器人编号(1 基)
        const RobotStatus &robot = state.red_robots[id - 1]; // 转为 0 基索引
        float bar_left = red_robot_bar_left + static_cast<float>(i) * (red_robot_bar_item_width + robot_bar_spacing); // 当前条左边界
        float bar_right = bar_left + red_robot_bar_item_width; // 当前条右边界
        bool has_health = id != 6 && id != 8 && id != 9; // 无人机、飞镖、雷达站没有血条
        bool show_health = has_health && robot.max_health > 0; // 有血条且上限有效才显示血量
        draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_right, robot_bar_bottom), bg_color, 2.0f); // 画血条底槽
        if (show_health) {   // 有血条且上限有效才按比例填充前景色
            float ratio = static_cast<float>(robot.health) / static_cast<float>(robot.max_health); // 血量比例
            if (ratio > 1.0f) { // 夹紧到 [0,1]
                ratio = 1.0f;   // 比例溢出（血量大于上限）时夹紧到 100%
            }   // 结束夹紧判断
            if (ratio > 0.0f) {   // 比例为 0 时不画填充，避免退化矩形
                draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_left + (bar_right - bar_left) * ratio, robot_bar_bottom), red_color, 2.0f); // 按比例填充红方色
            }   // 结束按比例填充
        }   // 结束 show_health 分支
        ImFormatStringToTempBuffer(&text_begin, &text_end, "%d %s", id, robot_names[id - 1]); // "编号 兵种"
        ImVec2 id_size = ImGui::CalcTextSize(text_begin, text_end);   // 量出「编号 兵种」文本尺寸用于居中
        draw_list->AddText(ImVec2(bar_left + (red_robot_bar_item_width - id_size.x) / 2, robot_bar_top - 2 - id_size.y), IM_COL32(255, 255, 255, 255), text_begin, text_end); // 血条上方居中显示编号
        if (show_health) {   // 有血量时显示「当前/上限」
            ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", robot.health, robot.max_health); // 当前/上限
        } else {   // 无血条的机体显示占位符
            ImFormatStringToTempBuffer(&text_begin, &text_end, "-"); // 无血条显示占位符
        }   // 结束血量文本选择
        ImVec2 health_size = ImGui::CalcTextSize(text_begin, text_end);   // 量出血量文本尺寸用于居中
        float health_text_y = robot_bar_bottom + 2; // 血量文本位于血条下方
        draw_list->AddText(ImVec2(bar_left + (red_robot_bar_item_width - health_size.x) / 2, health_text_y), IM_COL32(255, 255, 255, 255), text_begin, text_end); // 居中绘制血量
        // 显示允许发弹量
        ImFormatStringToTempBuffer(&text_begin, &text_end, "弹药:%d", robot.bullets); // 允许发弹量文本
        ImVec2 bullets_size = ImGui::CalcTextSize(text_begin, text_end);   // 量出弹药文本尺寸用于居中
        draw_list->AddText(ImVec2(bar_left + (red_robot_bar_item_width - bullets_size.x) / 2, health_text_y + health_size.y + 1), IM_COL32(200, 200, 200, 255), text_begin, text_end); // 灰色小字显示弹药
    }   // 结束红方机器人绘制循环
    // 蓝方机器人：逻辑与红方完全对称，仅数据源与主色不同。
    for (int i = 0; i < ids_size; i++) {
        int id = ids[i]; // 机器人编号
        const RobotStatus &robot = state.blue_robots[id - 1]; // 蓝方数据
        float bar_left = blue_robot_bar_left + static_cast<float>(i) * (blue_robot_bar_item_width + robot_bar_spacing); // 当前条左边界
        float bar_right = bar_left + blue_robot_bar_item_width; // 当前条右边界
        bool has_health = id != 6 && id != 8 && id != 9; // 无人机/飞镖/雷达无血条
        bool show_health = has_health && robot.max_health > 0;   // 蓝方同样要求有血条且上限有效
        draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_right, robot_bar_bottom), bg_color, 2.0f); // 底槽
        if (show_health) {   // 有血量才按比例填充蓝方色
            float ratio = static_cast<float>(robot.health) / static_cast<float>(robot.max_health); // 血量比例
            if (ratio > 1.0f) { // 夹紧
                ratio = 1.0f;   // 比例夹紧到 100%
            }   // 结束夹紧判断
            if (ratio > 0.0f) {   // 比例为 0 时不画填充
                draw_list->AddRectFilled(ImVec2(bar_left, robot_bar_top), ImVec2(bar_left + (bar_right - bar_left) * ratio, robot_bar_bottom), blue_color, 2.0f); // 蓝方色填充
            }   // 结束蓝方比例填充
        }   // 结束 show_health 分支
        ImFormatStringToTempBuffer(&text_begin, &text_end, "%d %s", id, robot_names[id - 1]); // "编号 兵种"
        ImVec2 id_size = ImGui::CalcTextSize(text_begin, text_end);   // 量出「编号 兵种」尺寸
        draw_list->AddText(ImVec2(bar_left + (blue_robot_bar_item_width - id_size.x) / 2, robot_bar_top - 2 - id_size.y), IM_COL32(255, 255, 255, 255), text_begin, text_end); // 编号
        if (show_health) {   // 有血量显示「当前/上限」
            ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", robot.health, robot.max_health); // 血量
        } else {   // 无血条显示占位符
            ImFormatStringToTempBuffer(&text_begin, &text_end, "-"); // 占位
        }   // 结束血量文本选择
        ImVec2 health_size = ImGui::CalcTextSize(text_begin, text_end);   // 量出血量文本尺寸
        float health_text_y = robot_bar_bottom + 2;   // 血量文本基线位于血条下方 2px
        draw_list->AddText(ImVec2(bar_left + (blue_robot_bar_item_width - health_size.x) / 2, health_text_y), IM_COL32(255, 255, 255, 255), text_begin, text_end); // 血量文本
        // 显示允许发弹量
        ImFormatStringToTempBuffer(&text_begin, &text_end, "弹药:%d", robot.bullets); // 弹药文本
        ImVec2 bullets_size = ImGui::CalcTextSize(text_begin, text_end);   // 量出弹药文本尺寸
        draw_list->AddText(ImVec2(bar_left + (blue_robot_bar_item_width - bullets_size.x) / 2, health_text_y + health_size.y + 1), IM_COL32(200, 200, 200, 255), text_begin, text_end); // 弹药文本
    }   // 结束蓝方机器人绘制循环
    ImGui::PopFont();   // 恢复原字号

    ImGui::End(); // 结束窗口
}   // 结束 ShowHeaderPanel
