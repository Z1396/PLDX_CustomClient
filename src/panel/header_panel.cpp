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

    // 我方是否红方：robot_id < 100 为红方；缺少静态状态时按红方处理（与默认配色一致）。
    // 提到这里是因为金币/控制度配色与后续红蓝槽位取值都要用。
    const bool we_are_red = !state.robot_static_status.has_value() || state.robot_static_status->robot_id() < 100;

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
    uint32_t outpost_max_health = 1000; // 前哨站最大血量(2027 规则 5.6.1：前哨站血量为 1000；被击毁后转给对方以 500 血重建)
    uint32_t base_max_health = 10000; // 基地最大血量(2027 规则 5.6.1：基地血量为 10000，2026 的 5000 已废止)
    // 血条比例计算：把当前血量折算成 0~1 的比例。
    // 说明：2027 基地可能叠护盾，理论值可超过上限；这里统一夹到 1.0，避免血条画到槽外溢出。
    auto health_ratio = [](uint32_t health, uint32_t max_health) -> float { // 入参：当前血量、上限血量
        if (max_health == 0) { // 防御性判断：上限为 0 时直接返回 0，避免除零
            return 0.0f; // 无上限信息时不填充
        } // 结束除零保护
        const float ratio = static_cast<float>(health) / static_cast<float>(max_health); // 当前值 ÷ 上限值
        return ratio > 1.0f ? 1.0f : ratio; // 超过上限(如叠护盾/数据异常)时截断为满格
    }; // 结束 health_ratio
    uint32_t red_outpost_health = 0, red_base_health = 0, blue_outpost_health = 0, blue_base_health = 0;   // 红/蓝两方的前哨站与基地血量
    uint32_t red_outpost_status = 0, blue_outpost_status = 0;   // 前哨阵地控制状态（1 我方完全 / 2 我方临时 / 3 对方临时 / 4 对方完全）
    uint32_t red_base_status = 0, blue_base_status = 0;   // 基地状态（0 拥有 100% 防御增益 / 1 无增益且护甲未展开 / 2 无增益且护甲展开）
    // 依据我方 robot_id 判断红蓝归属后取值。注意两类字段的"参照系"不同：
    //   base_health / base_status / base_shield 是"己方"，enemy_base_* 是"对方" —— 需要按我方阵营换算到红/蓝槽位；
    //   outpost_R_* / outpost_B_* 是绝对的红方前哨 / 蓝方前哨 —— 与阵营无关，不能再交换（2026 代码在此处错换，我方为蓝时两只前哨血量会颠倒）。
    if (state.global_unit_status.has_value() && state.robot_static_status.has_value()) {
        const GlobalUnitStatus &unit_status = *state.global_unit_status;   // 取单位状态只读引用，避免反复解引用
        if (we_are_red) {   // 我方为红：base_* 填入红槽位
            red_base_health = unit_status.base_health();   // 己方(红)基地血量
            red_base_status = unit_status.base_status();   // 己方(红)基地状态
            blue_base_health = unit_status.enemy_base_health();   // 对方(蓝)基地血量
            blue_base_status = unit_status.enemy_base_status();   // 对方(蓝)基地状态
        } else {   // 我方为蓝：base_* 应填入蓝槽位，红槽位取 enemy_base_*
            red_base_health = unit_status.enemy_base_health();   // 敌方(红)基地血量
            red_base_status = unit_status.enemy_base_status();   // 敌方(红)基地状态
            blue_base_health = unit_status.base_health();   // 己方(蓝)基地血量
            blue_base_status = unit_status.base_status();   // 己方(蓝)基地状态
        }   // 结束红蓝归属分支
        red_outpost_health = unit_status.outpost_r_health();   // R 前哨站恒为红方
        red_outpost_status = unit_status.outpost_r_status();   // R 前哨阵地控制状态
        blue_outpost_health = unit_status.outpost_b_health();   // B 前哨站恒为蓝方
        blue_outpost_status = unit_status.outpost_b_status();   // B 前哨阵地控制状态
    }   // 结束血量/状态取值（缺数据时保持 0，不显示血量与角标）

    // 绘制血条背景
    draw_list->AddRectFilled(ImVec2(red_outpost_health_left, building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color_translated, rounding, ImDrawFlags_RoundCornersLeft); // 红方前哨站底槽
    draw_list->AddRectFilled(ImVec2(red_base_health_left, building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color_translated, rounding, ImDrawFlags_RoundCornersLeft); // 红方基地底槽
    draw_list->AddRectFilled(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_right, building_health_bottom), blue_color_translated, rounding, ImDrawFlags_RoundCornersRight); // 蓝方前哨站底槽
    draw_list->AddRectFilled(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_right, building_health_bottom), blue_color_translated, rounding, ImDrawFlags_RoundCornersRight); // 蓝方基地底槽

    // 绘制血条填充
    // 红方血条从右端向左收缩(即缺失部分在左侧)，蓝方从左端向右收缩。
    if (red_outpost_health > 0) {
        draw_list->AddRectFilled(ImVec2(red_outpost_health_left + (red_outpost_health_right - red_outpost_health_left) * (1.0f - health_ratio(red_outpost_health, outpost_max_health)), building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color, rounding, ImDrawFlags_RoundCornersLeft); // 红方前哨站血量填充
    }   // 结束红方前哨站填充
    if (red_base_health > 0) {   // 红方基地血量大于 0 才绘制填充（为 0 时只留底槽）
        draw_list->AddRectFilled(ImVec2(red_base_health_left + (red_base_health_right - red_base_health_left) * (1.0f - health_ratio(red_base_health, base_max_health)), building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color, rounding, ImDrawFlags_RoundCornersLeft); // 红方基地血量填充
    }   // 结束红方基地填充
    if (blue_outpost_health > 0) {   // 蓝方前哨站血量大于 0 才绘制填充
        draw_list->AddRectFilled(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_left + (blue_outpost_health_right - blue_outpost_health_left) * health_ratio(blue_outpost_health, outpost_max_health), building_health_bottom), blue_color, rounding, ImDrawFlags_RoundCornersRight); // 蓝方前哨站血量填充
    }   // 结束蓝方前哨站填充
    if (blue_base_health > 0) {   // 蓝方基地血量大于 0 才绘制填充
        draw_list->AddRectFilled(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_left + (blue_base_health_right - blue_base_health_left) * health_ratio(blue_base_health, base_max_health), building_health_bottom), blue_color, rounding, ImDrawFlags_RoundCornersRight); // 蓝方基地血量填充
    }   // 结束蓝方基地填充

    // 绘制血条边框
    draw_list->AddRect(ImVec2(red_outpost_health_left, building_health_top), ImVec2(red_outpost_health_right, building_health_bottom), red_color, rounding, 1.0f, ImDrawFlags_RoundCornersLeft); // 红方前哨站描边
    draw_list->AddRect(ImVec2(red_base_health_left, building_health_top), ImVec2(red_base_health_right, building_health_bottom), red_color, rounding, 1.0f, ImDrawFlags_RoundCornersLeft); // 红方基地描边
    draw_list->AddRect(ImVec2(blue_outpost_health_left, building_health_top), ImVec2(blue_outpost_health_right, building_health_bottom), blue_color, rounding, 1.0f, ImDrawFlags_RoundCornersRight); // 蓝方前哨站描边
    draw_list->AddRect(ImVec2(blue_base_health_left, building_health_top), ImVec2(blue_base_health_right, building_health_bottom), blue_color, rounding, 1.0f, ImDrawFlags_RoundCornersRight); // 蓝方基地描边

    // 【2027 新增】阵地控制角标 + 基地防御增益角标。
    // 规则依据：2027 规则 5.6.1/5.6.2 —— 完全控制前哨阵地的一方，其基地获得 100% 防御增益。
    // 布局要点：角标会占住血条一侧的空间，所以血量数字必须在"剩余区域"内居中，否则窄窗口下会互相压住。
    struct BadgePlan {   // 单个角标的绘制计划
        const char *text = nullptr;   // 角标文字；空表示本次不画
        ImU32 color = 0;   // 文字颜色
        float width = 0.0f;   // 角标像素宽度（文字宽 + 左右内边距）
        bool at_left = true;   // true = 贴血条左端；false = 贴右端
    };   // BadgePlan 结束
    const float badge_padding = 4.0f * ImGui::GetScale();   // 角标内边距
    const float badge_rounding = 4.0f * ImGui::GetScale();   // 角标圆角
    const float badge_gap = 4.0f * ImGui::GetScale();   // 角标与血条端点的间隙
    const float number_reserve = 70.0f * ImGui::GetScale();   // 给"当前/上限"数字预留的最小宽度

    // 功能：前哨阵地控制状态码 → 角标文案/颜色（协议 2.2.4：1 我方完全 / 2 我方临时 / 3 对方临时 / 4 对方完全）。
    auto outpost_text = [](uint32_t status, ImU32 &color) -> const char * {   // 返回空指针表示不画
        switch (status) {   // 按状态码分支
            case 1:   // 我方完全控制
                color = IM_COL32(90, 255, 130, 255);   // 绿色
                return "控";   // 我方完全控制
            case 2:   // 我方临时控制
                color = IM_COL32(150, 235, 150, 255);   // 浅绿
                return "临";   // 我方临时控制
            case 3:   // 对方临时控制
                color = IM_COL32(255, 140, 140, 255);   // 浅红
                return "临";   // 对方临时控制
            case 4:   // 对方完全控制
                color = IM_COL32(255, 90, 90, 255);   // 红色
                return "控";   // 对方完全控制
            default:   // 0/未知
                return nullptr;   // 不画
        }   // 结束 switch
    };   // 结束 outpost_text

    // 功能：基地状态码 → 角标文案/颜色（协议 2.2.4 base_status：0 有 100% 防御增益 / 1 无增益且护甲未展开 / 2 无增益且护甲展开）。
    auto base_text = [](uint32_t status, ImU32 &color) -> const char * {   // 返回空指针表示不画
        switch (status) {   // 按状态码分支
            case 0:   // 拥有 100% 防御增益
                color = IM_COL32(90, 255, 130, 255);   // 绿色
                return "盾";   // 有防御增益
            case 1:   // 无增益、护甲未展开
                color = IM_COL32(255, 90, 90, 255);   // 红色
                return "无盾";   // 已失去防御增益
            case 2:   // 无增益、护甲已展开
                color = IM_COL32(255, 170, 40, 255);   // 橙色
                return "护甲";   // 护甲展开形态
            default:   // 未知状态
                return nullptr;   // 不画
        }   // 结束 switch
    };   // 结束 base_text

    ImGui::PushFont(ImGui::GetFont(), 14);   // 用角标字号量宽度，必须与实际绘制字号一致（实测 13 号不落字，用 14）
    // 功能：按"文案 + 血条宽度"生成绘制计划；血条放不下时不画，优先保证血量数字可读。
    auto build_badge = [&](const char *text, ImU32 color, bool at_left, float bar_width) -> BadgePlan {   // 入参：文案、颜色、贴合侧、血条宽度
        BadgePlan plan;   // 默认不画
        if (text == nullptr) {   // 状态未知
            return plan;   // 返回空计划
        }   // 结束未知状态
        const float width = ImGui::CalcTextSize(text).x + badge_padding * 2.0f;   // 角标总宽
        if (bar_width < width + number_reserve) {   // 血条太窄：放了角标就放不下数字
            return plan;   // 放弃画角标
        }   // 结束窄条保护
        plan.text = text;   // 记录文案
        plan.color = color;   // 记录颜色
        plan.width = width;   // 记录宽度
        plan.at_left = at_left;   // 记录贴合侧
        return plan;   // 返回计划
    };   // 结束 build_badge
    ImU32 red_outpost_color = 0, blue_outpost_color = 0, red_base_color = 0, blue_base_color = 0;   // 四个角标的文字颜色
    // 注意：必须先调用 *_text() 把颜色写进变量，再把它当实参传给 build_badge。
    // 直接写成 build_badge(outpost_text(..., color), color, ...) 会因为实参求值顺序不确定，
    // 可能先读到 color 的旧值 0（全透明），结果只画出黑底、看不到字。
    const char *red_outpost_text = outpost_text(red_outpost_status, red_outpost_color);   // 红方前哨角标文案
    const char *blue_outpost_text = outpost_text(blue_outpost_status, blue_outpost_color);   // 蓝方前哨角标文案
    const char *red_base_badge_text = base_text(red_base_status, red_base_color);   // 红方基地角标文案
    const char *blue_base_badge_text = base_text(blue_base_status, blue_base_color);   // 蓝方基地角标文案
    const BadgePlan red_outpost_badge = build_badge(red_outpost_text, red_outpost_color, true,   // 红方前哨：贴左
                                                    red_outpost_health_right - red_outpost_health_left);   // 血条宽度
    const BadgePlan blue_outpost_badge = build_badge(blue_outpost_text, blue_outpost_color, false,   // 蓝方前哨：贴右
                                                     blue_outpost_health_right - blue_outpost_health_left);   // 血条宽度
    const BadgePlan red_base_badge = build_badge(red_base_badge_text, red_base_color, true,   // 红方基地：贴左（靠近中间缺口）
                                                 red_base_health_right - red_base_health_left);   // 血条宽度
    const BadgePlan blue_base_badge = build_badge(blue_base_badge_text, blue_base_color, false,   // 蓝方基地：贴右（靠近中间缺口）
                                                  blue_base_health_right - blue_base_health_left);   // 血条宽度
    ImGui::PopFont();   // 恢复字号

    // 血条文本：为角标让出位置后再居中，避免角标压住"当前/上限"。
    ImGui::PushFont(ImGui::GetFont(), 16); // 数值文本字号
    const float red_outpost_inset = red_outpost_badge.text != nullptr ? red_outpost_badge.width + badge_gap * 2.0f : 0.0f;   // 红方前哨左侧让位
    const float blue_outpost_inset = blue_outpost_badge.text != nullptr ? blue_outpost_badge.width + badge_gap * 2.0f : 0.0f;   // 蓝方前哨右侧让位
    const float red_base_inset = red_base_badge.text != nullptr ? red_base_badge.width + badge_gap * 2.0f : 0.0f;   // 红方基地左侧让位
    const float blue_base_inset = blue_base_badge.text != nullptr ? blue_base_badge.width + badge_gap * 2.0f : 0.0f;   // 蓝方基地右侧让位
    // 功能：在血条内绘制"当前/上限"数值。
    // 思路：血条够宽就居中；血条窄又要画角标时改为"贴角标外侧对齐"，否则数字会被角标压住
    //       （1280 宽下前哨站条只有约 100px，居中放不下 7 个字符 + 角标）。
    auto draw_bar_health_text = [&](const char *begin, const char *end, float bar_left, float bar_right,
                                    bool badge_at_left, bool badge_at_right, float inset) {
        const ImVec2 text_size = ImGui::CalcTextSize(begin, end);   // 数值文本尺寸
        const bool bar_is_narrow = (bar_right - bar_left) < 170.0f * ImGui::GetScale();   // 是否属于窄条
        float text_x;   // 文本左边界
        if (bar_is_narrow && badge_at_left) {   // 窄条 + 角标在左：紧跟角标左对齐
            text_x = bar_left + inset;
        } else if (bar_is_narrow && badge_at_right) {   // 窄条 + 角标在右：贴右侧让开角标
            text_x = bar_right - inset - text_size.x;
        } else {   // 宽条：照常居中（基地条属于这种）
            text_x = bar_left + inset + (bar_right - inset - bar_left - inset - text_size.x) / 2.0f;
        }   // 结束对齐方式选择
        const float text_y = building_health_top + (building_health_bottom - building_health_top - text_size.y) / 2.0f;   // 垂直居中
        draw_list->AddText(ImVec2(text_x, text_y), IM_COL32(255, 255, 255, 255), begin, end);   // 白色绘制数值
    };   // 结束 draw_bar_health_text

    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", red_outpost_health, outpost_max_health); // 红方前哨站 当前/上限
    draw_bar_health_text(text_begin, text_end, red_outpost_health_left, red_outpost_health_right,   // 红方前哨站
                         red_outpost_badge.text != nullptr, false, red_outpost_inset);   // 角标在左
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", red_base_health, base_max_health); // 红方基地 当前/上限
    draw_bar_health_text(text_begin, text_end, red_base_health_left, red_base_health_right,   // 红方基地
                         red_base_badge.text != nullptr, false, red_base_inset);   // 角标在左
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", blue_outpost_health, outpost_max_health); // 蓝方前哨站 当前/上限
    draw_bar_health_text(text_begin, text_end, blue_outpost_health_left, blue_outpost_health_right,   // 蓝方前哨站
                         false, blue_outpost_badge.text != nullptr, blue_outpost_inset);   // 角标在右
    ImFormatStringToTempBuffer(&text_begin, &text_end, "%d/%d", blue_base_health, base_max_health); // 蓝方基地 当前/上限
    draw_bar_health_text(text_begin, text_end, blue_base_health_left, blue_base_health_right,   // 蓝方基地
                         false, blue_base_badge.text != nullptr, blue_base_inset);   // 角标在右
    ImGui::PopFont();   // 恢复字号

    // 绘制四个角标（半透明黑底 + 彩色文字，垂直居中于血条内）。
    ImGui::PushFont(ImGui::GetFont(), 14);   // 角标字号（与上面量宽度时保持一致）
    // 功能：按计划把角标画在血条对应端部。
    auto draw_badge = [&](const BadgePlan &plan, float bar_left, float bar_right) {   // 入参：计划、血条左右边界
        if (plan.text == nullptr) {   // 未规划
            return;   // 不画
        }   // 结束未规划
        const float left = plan.at_left ? bar_left + badge_gap : bar_right - plan.width - badge_gap;   // 贴左或贴右反推左边界
        const ImVec2 text_size = ImGui::CalcTextSize(plan.text);   // 文字尺寸
        draw_list->AddRectFilled(ImVec2(left, building_health_top),   // 角标左上角
                                 ImVec2(left + plan.width, building_health_bottom),   // 角标右下角
                                 IM_COL32(0, 0, 0, 150), badge_rounding);   // 半透明黑底，任何底色上都清晰
        draw_list->AddText(ImVec2(left + badge_padding,   // 文字左边界
                                  building_health_top + (building_health_bottom - building_health_top - text_size.y) / 2),   // 垂直居中
                           plan.color, plan.text);   // 绘制文字
    };   // 结束 draw_badge
    draw_badge(red_outpost_badge, red_outpost_health_left, red_outpost_health_right);   // 红方前哨站角标
    draw_badge(blue_outpost_badge, blue_outpost_health_left, blue_outpost_health_right);   // 蓝方前哨站角标
    draw_badge(red_base_badge, red_base_health_left, red_base_health_right);   // 红方基地角标
    draw_badge(blue_base_badge, blue_base_health_left, blue_base_health_right);   // 蓝方基地角标
    ImGui::PopFont();   // 恢复字号

    // 机器人血量条
    float robot_bar_top = building_health_bottom + 30 * ImGui::GetScale(); // 机器人血条顶部(建筑血条下方)
    float robot_bar_bottom = robot_bar_top + 4 * ImGui::GetScale(); // 血条本身只有 4px 高
    float red_robot_bar_left = red_health_left; // 红方机器人区左边界
    float red_robot_bar_right = red_health_right; // 红方机器人区右边界
    float blue_robot_bar_left = blue_health_left; // 蓝方机器人区左边界
    float blue_robot_bar_right = blue_health_right; // 蓝方机器人区右边界
    const char *robot_names[] = {"重装", "保留", "步兵", "步兵", "步兵", "空中系统", "哨兵", "飞镖", "雷达"}; // 按序号 1..9 的兵种名
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
        bool has_health = id != 6 && id != 8 && id != 9; // 空中系统、飞镖、雷达站没有血条
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
        bool has_health = id != 6 && id != 8 && id != 9; // 空中系统/飞镖/雷达无血条
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

// 功能：把判罚类型码翻译成中文（2027 协议 2.2.15 PenaltyInfo.penalty_type）。
// 思路：switch 覆盖 1~6，未知码返回 nullptr 由调用方兜底，避免显示错误文案。
const char *GetPenaltyTypeName(uint32_t penalty_type) {
    switch (penalty_type) {   // 按判罚类型码分支
        case 1:   // 1 = 黄牌
            return "黄牌";   // 单方黄牌
        case 2:   // 2 = 双方黄牌
            return "双方黄牌";   // 双方同时被判
        case 3:   // 3 = 红牌
            return "红牌";   // 罚下
        case 4:   // 4 = 超功率
            return "超功率";   // 底盘功率超限
        case 5:   // 5 = 超热量
            return "超热量";   // 枪管热量超限
        case 6:   // 6 = 超射速
            return "超射速";   // 射频超限
        default:   // 未定义类型码
            return nullptr;   // 交给调用方兜底
    }   // 结束 switch
}   // 结束 GetPenaltyTypeName

// 功能：绘制判罚横幅——顶部状态栏下方居中的一条无交互提示，显示判罚类型与本地倒计时。
// 思路：协议只推"判罚类型 + 持续秒数"，不推剩余时间，因此用本地接收时刻自行倒数；倒计时归零即不再绘制。
void ShowPenaltyBanner(const State &state) {
    if (!state.penalty_info.has_value()) {   // 从未收到过判罚
        return;   // 不绘制任何内容
    }   // 结束无判罚分支

    const uint32_t penalty_type = state.penalty_info->penalty_type();   // 判罚类型码
    const uint32_t effect_sec = state.penalty_info->penalty_effect_sec();   // 受罚效果总时长(秒)
    if (effect_sec == 0) {   // 时长为 0 表示没有实际效果
        return;   // 无需提示
    }   // 结束零时长分支

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(   // 从收到判罚到现在经过的秒数
            std::chrono::steady_clock::now() - state.penalty_received_time);   // 用单调时钟避免系统时间调整影响
    const long long remain_sec = static_cast<long long>(effect_sec) - static_cast<long long>(elapsed.count());   // 剩余秒数
    if (remain_sec <= 0) {   // 判罚效果已结束
        return;   // 横幅自动消失
    }   // 结束过期分支

    const char *penalty_name = GetPenaltyTypeName(penalty_type);   // 判罚中文名
    // 兜底文案：遇到协议新增但本端未识别的类型码时，直接显示原始码，便于现场判断。
    const char *fallback_name = "未知判罚";   // 未知类型时的显示名
    const bool is_known = penalty_name != nullptr;   // 是否识别成功

    // 配色：红牌用红底白字；超功率/超热量/超射速用橙底白字；黄牌用黄底黑字（黄底上白字看不清）。
    ImU32 background_color;   // 横幅底色
    ImU32 text_color;   // 文字颜色
    if (penalty_type == 3) {   // 红牌最严重
        background_color = IM_COL32(200, 30, 30, 235);   // 红色底
        text_color = IM_COL32(255, 255, 255, 255);   // 白色字
    } else if (penalty_type == 1 || penalty_type == 2) {   // 黄牌
        background_color = IM_COL32(235, 190, 40, 235);   // 黄色底
        text_color = IM_COL32(30, 30, 30, 255);   // 黑字保证对比度
    } else {   // 超功率/超热量/超射速等超限判罚
        background_color = IM_COL32(230, 120, 30, 235);   // 橙色底
        text_color = IM_COL32(255, 255, 255, 255);   // 白色字
    }   // 结束配色选择

    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |   // 不响应键盘导航、无标题栏边框
                                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |   // 不接收鼠标输入、不可拖动
                                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;   // 背景自绘、不写入 imgui.ini
    // 位置与尺寸在这里自行设定：横幅宽度取"视口 28%"与"420px"的较小值，窄屏不溢出；高度固定 34px（随缩放）。
    const ImVec2 viewport_size = ImGui::GetMainViewport()->Size;   // 主视口尺寸
    const float max_banner_width = 420.0f * ImGui::GetScale();   // 宽度上限
    const float banner_width = viewport_size.x * 0.28f < max_banner_width ? viewport_size.x * 0.28f : max_banner_width;   // 取较小值
    const float banner_height = 34.0f * ImGui::GetScale();   // 横幅高度
    ImGui::SetNextWindowPos(ImVec2((viewport_size.x - banner_width) * 0.5f,   // 水平居中
                                   viewport_size.y * 0.15f + 6.0f * ImGui::GetScale()),   // 顶部状态栏(占视口 15%)下沿再留 6px
                            ImGuiCond_Always);   // 每帧强制应用
    ImGui::SetNextWindowSize(ImVec2(banner_width, banner_height), ImGuiCond_Always);   // 固定尺寸
    if (!ImGui::Begin("PenaltyBannerWindow", nullptr, window_flags)) {   // 窗口被裁剪时
        ImGui::End();   // 保持 Begin/End 配对
        return;   // 本帧不绘制
    }   // 结束 Begin 失败分支

    const ImVec2 window_pos = ImGui::GetWindowPos();   // 横幅左上角屏幕坐标（由调用方 SetNextWindowPos 指定）
    const ImVec2 window_size = ImGui::GetWindowSize();   // 横幅尺寸
    ImDrawList *draw_list = ImGui::GetWindowDrawList();   // 当前窗口绘制列表
    const ImVec2 window_max(window_pos.x + window_size.x, window_pos.y + window_size.y);   // 右下角坐标
    const float rounding = 8.0f * ImGui::GetScale();   // 圆角半径随缩放变化

    draw_list->AddRectFilled(window_pos, window_max, background_color, rounding);   // 自绘圆角底框
    draw_list->AddRect(window_pos, window_max, IM_COL32(255, 255, 255, 90), rounding, 1.0f);   // 细白描边提升可读性

    const char *text_begin, *text_end;   // 复用的文本起止指针
    if (is_known) {   // 类型已识别
        ImFormatStringToTempBuffer(&text_begin, &text_end, "%s   剩余 %lld 秒", penalty_name, remain_sec);   // "黄牌 剩余 42 秒"
    } else {   // 未知类型码
        ImFormatStringToTempBuffer(&text_begin, &text_end, "%s(%u)   剩余 %lld 秒", fallback_name, penalty_type, remain_sec);   // 带上原始码
    }   // 结束文案选择
    AddTextInTheCenterOfRect(draw_list, text_begin, text_end, window_pos.x, window_pos.y, window_max.x, window_max.y, text_color);   // 文字居中

    ImGui::End();   // 结束窗口
}   // 结束 ShowPenaltyBanner


// 功能：绘制顶部事件提示层——把最近的重要 Event 以短暂浮层的形式堆叠显示。
// 思路：新事件排在最上方，每条显示约 3.5 秒后开始淡出、6 秒后由 PushEventToast 清理；最多同屏 6 条。
void ShowEventToasts(const State &state) {
    if (state.event_toasts.empty()) {   // 没有待显示事件
        return;   // 直接返回
    }   // 结束空队列分支

    const ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |   // 无导航、无装饰
                                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |   // 不接收输入、不可拖动
                                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;   // 背景自绘、不写 ini
    const ImVec2 viewport_size = ImGui::GetMainViewport()->Size;   // 主视口尺寸
    const float toast_height = 26.0f * ImGui::GetScale();   // 单条高度
    const float toast_gap = 4.0f * ImGui::GetScale();   // 条间距
    const float max_toast_width = viewport_size.x * 0.32f;   // 单条最大宽度，避免超长文案横贯全屏
    const float area_top = viewport_size.y * 0.15f + 46.0f * ImGui::GetScale();   // 起点：判罚横幅(34px)下方
    const int max_visible = 6;   // 同屏最多显示条数（与 PushEventToast 的队列上限一致）
    const int visible_count = static_cast<int>(state.event_toasts.size()) > max_visible   // 实际显示条数取两者较小值
                                      ? max_visible
                                      : static_cast<int>(state.event_toasts.size());
    const float area_height = static_cast<float>(visible_count) * (toast_height + toast_gap);   // 覆盖区域高度
    ImGui::SetNextWindowPos(ImVec2((viewport_size.x - max_toast_width) * 0.5f, area_top), ImGuiCond_Always);   // 水平居中
    ImGui::SetNextWindowSize(ImVec2(max_toast_width, area_height), ImGuiCond_Always);   // 固定区域尺寸
    if (!ImGui::Begin("EventToastWindow", nullptr, window_flags)) {   // 被裁剪时
        ImGui::End();   // 保持配对
        return;   // 不绘制
    }   // 结束 Begin 失败分支
    ImDrawList *draw_list = ImGui::GetWindowDrawList();   // 当前窗口绘制列表
    const ImVec2 window_pos = ImGui::GetWindowPos();   // 区域左上角
    const auto now = std::chrono::steady_clock::now();   // 当前时刻，用于计算淡出
    const float rounding = 6.0f * ImGui::GetScale();   // 圆角

    ImGui::PushFont(ImGui::GetFont(), 15);   // 提示文字字号
    int slot = 0;   // 当前绘制到的行号（新→旧，从上往下）
    for (auto it = state.event_toasts.rbegin(); it != state.event_toasts.rend() && slot < max_visible; ++it) {   // 逆序遍历：最新的先画
        const long long age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->time).count();   // 该提示已存在毫秒数
        if (age_ms > 6000) {   // 超过 6 秒（正常情况下已被清理，这里再兜一层）
            continue;   // 跳过
        }   // 结束过期判断
        // 淡出：前 3.5 秒完全不透明，之后线性衰减到 6 秒时完全透明。
        float alpha = 1.0f;   // 透明度系数
        if (age_ms > 3500) {   // 进入淡出阶段
            alpha = 1.0f - static_cast<float>(age_ms - 3500) / 2500.0f;   // 2500ms 内从 1 线性降到 0
            if (alpha < 0.0f) {   // 防御性下限
                alpha = 0.0f;   // 不超过 0
            }   // 结束下限保护
        }   // 结束淡出计算
        const ImVec2 text_size = ImGui::CalcTextSize(it->text.c_str());   // 文本尺寸
        const float padding = 10.0f * ImGui::GetScale();   // 左右内边距
        const float box_width = text_size.x + padding * 2 < max_toast_width   // 条宽取"文字宽+内边距"与区域宽度的较小值
                                        ? text_size.x + padding * 2
                                        : max_toast_width;
        const float box_left = window_pos.x + (max_toast_width - box_width) * 0.5f;   // 单条在区域内水平居中
        const float box_top = window_pos.y + static_cast<float>(slot) * (toast_height + toast_gap);   // 按行号下移
        // 按级别取底色与文字色：0 普通(深灰底白字) / 1 重要(橙底白字) / 2 危急(红底白字)
        ImU32 background_color = IM_COL32(30, 30, 30, 200);   // 普通
        ImU32 text_color = IM_COL32(255, 255, 255, 255);   // 白色文字
        if (it->level == 1) {   // 重要事件
            background_color = IM_COL32(200, 110, 20, 215);   // 橙色底
        } else if (it->level >= 2) {   // 危急事件
            background_color = IM_COL32(190, 40, 40, 225);   // 红色底
        }   // 结束配色
        // 把 alpha 施加到 RGBA 的 A 通道上，实现整体淡出。
        background_color = (background_color & 0x00FFFFFFu) | ((static_cast<ImU32>(255.0f * alpha) & 0xFFu) << 24);   // 替换 Alpha
        text_color = (text_color & 0x00FFFFFFu) | ((static_cast<ImU32>(255.0f * alpha) & 0xFFu) << 24);   // 替换 Alpha
        draw_list->AddRectFilled(ImVec2(box_left, box_top), ImVec2(box_left + box_width, box_top + toast_height), background_color, rounding);   // 底框
        draw_list->AddText(ImVec2(box_left + padding, box_top + (toast_height - text_size.y) / 2), text_color, it->text.c_str());   // 左对齐绘制文字
        ++slot;   // 下一行
    }   // 结束提示遍历
    ImGui::PopFont();   // 恢复字号
    ImGui::End();   // 结束窗口
}   // 结束 ShowEventToasts
