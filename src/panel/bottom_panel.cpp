// bottom_panel.cpp：底部信息条面板的实现。
#include "bottom_panel.h"
#include <algorithm>   // std::clamp / std::max：对抗条分割位置与布局宽度计算
#include <cstdio>   // snprintf：格式化保留两位小数的清晰度系数  // 引入本面板对应的头文件（含函数声明与依赖）

namespace {

// 右侧栏（小地图）占全宽比例：必须与 main_window.cpp 里的 right_column_ratio 保持一致，
// 底部条用它来避开小地图，否则总伤害那一组的数字会压在地图上。
constexpr float kRightColumnRatio = 0.24f;

// 功能：算出己方在总量中的占比，决定对抗条的分割位置。
// 说明：双方都为 0 时按对半返回，避免刚开局出现一条纯色空条（看起来像"被对方占满"）。
float CalcAllyRatio(uint32_t ally, uint32_t enemy) {
    const float total = static_cast<float>(ally) + static_cast<float>(enemy);   // 总量
    if (total <= 0.0f) {   // 两边都没有数据
        return 0.5f;   // 按对半显示
    }   // 结束空数据判断
    return static_cast<float>(ally) / total;   // 己方占比
}   // 结束 CalcAllyRatio

// 功能：把当前值以固定步长推向目标值，用于比例的缓动（数据 1Hz，硬切会闪）。
float MoveTowards(float current, float target, float max_delta) {
    if (current < target) {   // 需要增大
        return std::min(current + max_delta, target);   // 逼近但不越过
    }   // 结束增大分支
    return std::max(current - max_delta, target);   // 需要减小：同样逼近不越过
}   // 结束 MoveTowards

// ---- 控制度拆分：协议只下发"累计总控制度"一个数，前哨(+1/秒)与基地(+3/秒)的贡献得自己推 ----
// 两个速率差 3 倍，混在一个总数里看不出"这块地抢得值不值"，所以拆成两个桶单独记。
// 做法：以服务器下发的 total_CP 为权威总量，把每一跳的增量按"此刻各控几个阵地"折算的权重分配；
//       首次收到（客户端接入晚于开局）或总量回落（新开一局）时，把已有总量按当前权重一次性摊开，
//       这样任何时候 前哨桶 + 基地桶 + 未知桶 ≡ 服务器总量，不会出现"分项加起来对不上总数"。
constexpr uint32_t kWeightOutpost = 1;   // 一个前哨阵地每秒的价值
constexpr uint32_t kWeightBase = 3;      // 一个基地阵地每秒的价值（规则的 3 倍）

struct ControlPointSplit {
    uint32_t ally_outpost = 0;    // 己方控制度里来自前哨阵地的部分
    uint32_t ally_base = 0;       // 己方控制度里来自基地阵地的部分
    uint32_t ally_unknown = 0;    // 涨了但当时一个阵地都没控住，归不进任何一类（模型与现场不符时兜底）
    uint32_t enemy_outpost = 0;   // 对方同上的前哨部分
    uint32_t enemy_base = 0;      // 对方同上的基地部分
    uint32_t enemy_unknown = 0;   // 对方兜底部分
    uint32_t last_ally_total = 0;   // 上一拍的己方总控制度，用来取增量
    uint32_t last_enemy_total = 0;  // 上一拍的对方总控制度
    bool initialized = false;     // 是否已经拿到过第一拍数据
};   // ControlPointSplit 定义结束

ControlPointSplit g_cp_split;   // 全局唯一实例：整个进程只维护一份

// 上一帧底部条实际占用的高度：左下角"本车状态"面板靠它让位，防止底栏多一行就压字。
float g_last_panel_height = 0.0f;

// 功能：把一跳增量按"前哨权重 : 基地权重"分到两个桶里。
// 说明：权重都为 0（双方一个阵地都没控住）却仍涨了控制度时，全归 unknown 桶，保证总量仍然对得上。
void SplitDelta(uint32_t delta, uint32_t weight_outpost, uint32_t weight_base,
                uint32_t &outpost_bucket, uint32_t &base_bucket, uint32_t &unknown_bucket) {
    if (delta == 0) {   // 没涨就不用分
        return;   // 直接返回
    }   // 结束零增量判断
    const uint32_t weight_sum = weight_outpost + weight_base;   // 总权重
    if (weight_sum == 0) {   // 没有可归因的阵地
        unknown_bucket += delta;   // 全记到兜底桶
        return;   // 结束
    }   // 结束无权重分支
    const uint32_t outpost_part = static_cast<uint32_t>(static_cast<uint64_t>(delta) * weight_outpost / weight_sum);   // 前哨应得部分（64 位中间量防溢出）
    outpost_bucket += outpost_part;   // 记入前哨桶
    base_bucket += delta - outpost_part;   // 余数全给基地桶，一个点都不丢
}   // 结束 SplitDelta

// 功能：用一帧的 GlobalUnitStatus 推进控制度拆分。
// 说明：协议里 outpost_R/B_status 是绝对位置（1/2 己方、3/4 对方），base_shield 与 enemy_base_shield
//       是"己方/对方"相对语义，所以这四块阵地不需要知道自己是红是蓝就能直接判归属。
//       只认"完全控制"（1 / 4）：临时控制意味着占领进度还没走完，按规则不计控制度。
void UpdateControlPointSplit(const GlobalUnitStatus &unit) {
    const uint32_t cur_ally = unit.total_cp_ally();    // 服务器给的己方总量
    const uint32_t cur_enemy = unit.total_cp_enemy();  // 服务器给的对方总量

    // 己方控住的阵地数（前哨两块 + 基地两块）
    // 口径：完全控制与临时控制都算"占着这块地"。基地两个字段只有"己方完全 / 对方临时"这种单向组合，
    // 若只认"完全"，我方临时控制对方基地就永远拿不到 +3，与规则里"占了就涨"的直觉相反，所以临时也算。
    const int ally_outpost_count = ((unit.outpost_r_status() == 1 || unit.outpost_r_status() == 2) ? 1 : 0) +
                                   ((unit.outpost_b_status() == 1 || unit.outpost_b_status() == 2) ? 1 : 0);
    const int ally_base_count = (unit.base_shield() == 1 ? 1 : 0) + (unit.enemy_base_shield() == 2 ? 1 : 0);
    // 对方控住的阵地数
    const int enemy_outpost_count = ((unit.outpost_r_status() == 3 || unit.outpost_r_status() == 4) ? 1 : 0) +
                                    ((unit.outpost_b_status() == 3 || unit.outpost_b_status() == 4) ? 1 : 0);
    const int enemy_base_count = (unit.base_shield() == 2 ? 1 : 0) + (unit.enemy_base_shield() == 1 ? 1 : 0);

    // 总量回落 = 新开一局（或服务器重连后重置），分项随之清零重新开始摊
    const bool reset = !g_cp_split.initialized || cur_ally < g_cp_split.last_ally_total ||
                       cur_enemy < g_cp_split.last_enemy_total;
    if (reset) {   // 归零后按当前权重把已有总量一次性摊开
        g_cp_split.ally_outpost = g_cp_split.ally_base = g_cp_split.ally_unknown = 0;
        g_cp_split.enemy_outpost = g_cp_split.enemy_base = g_cp_split.enemy_unknown = 0;
        SplitDelta(cur_ally, static_cast<uint32_t>(ally_outpost_count) * kWeightOutpost,
                   static_cast<uint32_t>(ally_base_count) * kWeightBase,
                   g_cp_split.ally_outpost, g_cp_split.ally_base, g_cp_split.ally_unknown);
        SplitDelta(cur_enemy, static_cast<uint32_t>(enemy_outpost_count) * kWeightOutpost,
                   static_cast<uint32_t>(enemy_base_count) * kWeightBase,
                   g_cp_split.enemy_outpost, g_cp_split.enemy_base, g_cp_split.enemy_unknown);
        g_cp_split.initialized = true;   // 后续走增量分支
    } else {   // 常规路径：只处理这一拍新增的部分
        SplitDelta(cur_ally - g_cp_split.last_ally_total,
                   static_cast<uint32_t>(ally_outpost_count) * kWeightOutpost,
                   static_cast<uint32_t>(ally_base_count) * kWeightBase,
                   g_cp_split.ally_outpost, g_cp_split.ally_base, g_cp_split.ally_unknown);
        SplitDelta(cur_enemy - g_cp_split.last_enemy_total,
                   static_cast<uint32_t>(enemy_outpost_count) * kWeightOutpost,
                   static_cast<uint32_t>(enemy_base_count) * kWeightBase,
                   g_cp_split.enemy_outpost, g_cp_split.enemy_base, g_cp_split.enemy_unknown);
    }   // 结束重置/增量分支

    g_cp_split.last_ally_total = cur_ally;    // 记住本拍总量，下一拍取差
    g_cp_split.last_enemy_total = cur_enemy;  // 同上
}   // 结束 UpdateControlPointSplit


// 功能：画一条"对抗带"——左段己方色、右段对方色，按比例分割，中线是 50% 刻度。
// 说明：带内不放数字。六位数会直接顶到分割线上，怎么夹紧都会压线；数字交给调用方排在带的两侧。
void DrawControlBand(ImDrawList *draw_list, ImVec2 top_left, ImVec2 size, float ally_ratio,
                     ImU32 ally_color, ImU32 enemy_color) {
    const float rounding = size.y * 0.5f;   // 圆角取半高 → 胶囊形
    const float split_x = top_left.x + size.x * std::clamp(ally_ratio, 0.0f, 1.0f);   // 分割线位置
    const float right_x = top_left.x + size.x;   // 条右边界
    const float bottom_y = top_left.y + size.y;   // 条下边界

    draw_list->AddRectFilled(top_left, ImVec2(right_x, bottom_y), IM_COL32(34, 36, 42, 230), rounding);   // 底槽
    if (split_x - top_left.x > 1.0f) {   // 己方段有宽度才画，避免 1px 碎片出现圆角毛边
        draw_list->AddRectFilled(top_left, ImVec2(split_x, bottom_y), ally_color, rounding, ImDrawFlags_RoundCornersLeft);   // 己方段
    }   // 结束己方段判断
    if (right_x - split_x > 1.0f) {   // 对方段有宽度才画
        draw_list->AddRectFilled(ImVec2(split_x, top_left.y), ImVec2(right_x, bottom_y), enemy_color, rounding, ImDrawFlags_RoundCornersRight);   // 对方段
    }   // 结束对方段判断

    // 50% 刻度：线落在哪一侧的颜色里，就说明哪一侧过半，不用心算。
    const float mid_x = top_left.x + size.x * 0.5f;   // 正中
    draw_list->AddLine(ImVec2(mid_x, top_left.y + 1.5f), ImVec2(mid_x, bottom_y - 1.5f), IM_COL32(0, 0, 0, 130), 1.0f);   // 刻度线
    draw_list->AddRect(top_left, ImVec2(right_x, bottom_y), IM_COL32(255, 255, 255, 24), rounding);   // 极淡描边，暗底上不糊边
}   // 结束 DrawControlBand

}// namespace

// 功能：在屏幕底部居中渲染一条信息条，汇总复活/装配/空中支援等状态文本。
// 思路：先根据各项状态构造带颜色的文本行，再按行数计算面板高度并贴底边绘制。
void ShowBottomPanel(const State &state) {  // state：全局状态，提供本函数所需的所有展示数据
    const ImGuiViewport *viewport = ImGui::GetMainViewport();  // 获取主视口，用于贴底定位与全宽
    ImGuiStyle &style = ImGui::GetStyle();  // 读取全局样式，用于间距与内边距计算
    float font_size = ImGui::GetFontSize();  // 获取当前字号，用于估算面板高度

    // 单行状态：文本 + 颜色，便于统一渲染
    struct StatusLine {
        std::string text;  // 该行要显示的文字内容
        ImVec4 color;      // 该行文字颜色（按状态区分，如警告橙、错误红）
    };  // StatusLine 结构体定义结束

    std::vector<StatusLine> status_lines;  // 收集所有待显示的状态行

    // 复活状态显示
    if (state.robot_respawn_status.has_value()) {  // 仅当存在复活状态时才处理
        const auto &respawn_status = *state.robot_respawn_status;  // 解引用可选复活状态
        if (respawn_status.is_pending_respawn()) {  // 仅当处于待复活阶段才显示进度
            std::string respawn_text = "复活进度: ";  // 初始化进度文本前缀
            respawn_text += std::to_string(respawn_status.current_respawn_progress());  // 追加当前进度值
            respawn_text += "/";  // 分隔符
            respawn_text += std::to_string(respawn_status.total_respawn_progress());  // 追加总进度值
            status_lines.push_back({std::move(respawn_text), ImVec4(1.0f, 1.0f, 0.3f, 1.0f)});  // 黄色高亮
        }  // 待复活分支结束
    }  // 复活状态处理结束

    // 装配状态显示
    if (state.tech_core_motion_state_sync.has_value()) {  // 仅当存在科技核心运动同步状态时才处理
        const auto &sync = *state.tech_core_motion_state_sync;  // 解引用 optional，后续直接访问字段，避免重复 has_value 判断

        // 判断是否在装配中：basic_state 为 2（运动中）或 3（已到达对应位姿）时表示在装配
        bool is_assembling = sync.basic_state() >= 2 && sync.basic_state() <= 3;

        if (is_assembling) {  // 处于装配中才构造状态文本
            std::string status_text;  // 装配状态描述文本

            // 根据 basic_state 构建状态描述
            if (sync.basic_state() == 2) {  // basic_state==2 表示移动阶段
                status_text = "科技核心移动中";  // 移动阶段提示
            } else if (sync.basic_state() == 3) {  // basic_state==3 表示已到位，展示子步骤
                // 显示当前装配步骤状态
                std::vector<std::string> steps;  // 收集已完成的装配子步骤
                if (sync.putin_state() == 1) steps.push_back("已放入");  // 放入完成
                if (sync.move_state() == 1) steps.push_back("已平移");   // 平移完成
                if (sync.rotate_state() == 1) steps.push_back("已旋转"); // 旋转完成

                if (steps.empty()) {  // 尚未开始任一子步骤
                    status_text = "等待装配";  // 尚未装配提示
                } else {  // 已至少完成一个子步骤，进入步骤链拼接
                    // 用「->」连接各已完成步骤，形成步骤链路
                    status_text = "装配中: " + std::accumulate(steps.begin(), steps.end(), std::string(),
                        [](const std::string &a, const std::string &b) {  // 累积 lambda：a 为已拼接结果，b 为下一个步骤名
                            return a.empty() ? b : a + " -> " + b;  // 用「->」拼接相邻步骤
                        });  // accumulate 结束，得到「A -> B -> C」形式的文本
                }  // 步骤拼接分支结束

                // 显示剩余时间
                if (sync.remain_time_step() > 0) {  // 存在剩余时间才追加
                    status_text += " (" + std::to_string(sync.remain_time_step()) + "s)";  // 追加剩余秒数
                }  // 剩余时间追加结束
            }  // basic_state==3 分支结束

            status_lines.push_back({"重装装配状态: " + status_text, ImVec4(0.3f, 0.8f, 1.0f, 1.0f)});  // 蓝色
        }  // 装配中分支结束
    }  // 装配状态处理结束


    // 【2027 新增】累计控制度 / 累计总伤害：一局打满且双方基地均未被击毁时，先比控制度、再比全队总伤害。
    // 属于胜负判据，常驻显示在底部条第一行；放在这里而不是顶部状态栏，是因为顶部 15% 高度的中央列只有约
    // 108px 宽（两侧是机器人血条），塞两行数字会和血条打架，底部条全宽、居中，空间充足。
    const bool show_score_line = state.global_unit_status.has_value();  // 有单位状态才显示
    // 控制度构成行：总量还是 0（还没开始涨）时没内容可拆，不占高度，避免空出一行。
    const bool show_split_line = show_score_line &&
                                 (state.global_unit_status->total_cp_ally() + state.global_unit_status->total_cp_enemy()) > 0;
    // 高度不再用"行数 × 字高"估算：对抗条那一行比普通文字高（条高 16×缩放），按实际高度相加才不会被裁掉。
    const float score_row_height = std::max(font_size, 16.0f * ImGui::GetScale());   // 控制度/伤害行高
    const size_t status_line_count = status_lines.size();   // 其余状态文字行数

    float panel_height = style.WindowPadding.y * 2.0f;   // 上下内边距
    if (show_score_line) {   // 有对抗条行
        panel_height += score_row_height;   // 加上该行高度
    }   // 结束对抗条行累加
    if (show_split_line) {   // 有一行控制度构成
        panel_height += style.ItemSpacing.y + font_size;   // 行间距 + 一行字高
    }   // 结束构成行累加
    if (status_line_count > 0) {   // 有状态文字行
        if (show_score_line) {   // 前面已有一行，补一个行间距
            panel_height += style.ItemSpacing.y;   // 行间距
        }   // 结束间距补偿
        panel_height += font_size * static_cast<float>(status_line_count)   // 文字行总高
                        + style.ItemSpacing.y * static_cast<float>(status_line_count - 1);   // 行间竖直间距
    }   // 结束状态行累加
    if (!show_score_line && status_line_count == 0) {   // 什么内容都没有时也要留一行高
        panel_height += font_size;   // 保底一行
    }   // 结束保底判断

    // 无导航/无装饰/不可移动/无背景/不保存设置的浮动窗口
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;  // 组合标志：布局不写入 imgui.ini，且不绘制窗口背景（覆盖层效果）
    ImGui::SetNextWindowPos(ImVec2(0, viewport->Size.y - panel_height), ImGuiCond_Always);  // 固定在屏幕底部
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, panel_height), ImGuiCond_Always);  // 全宽

    if (!ImGui::Begin("BottomPanel", nullptr, window_flags)) {  // 窗口创建失败则结束
        ImGui::End();  // 配对 End
        return;  // 不绘制
    }  // 窗口被折叠的分支结束

    // 第一行：控制度 / 总伤害 —— 用"对抗条"代替纯数字。
    // 纯数字要心算才知道谁领先；条形把比例直接摆在眼前，数字仍留在条内，不丢精度。
    if (show_score_line) {
        const GlobalUnitStatus &unit_status = *state.global_unit_status;   // 单位状态引用
        UpdateControlPointSplit(unit_status);   // 每帧推进一次控制度拆分（总量没变时是空操作）
        // 我方是否红方：robot_id < 100 为红方；缺数据时按红方取色（与顶部状态栏一致）。
        const bool we_are_red = !state.robot_static_status.has_value() || state.robot_static_status->robot_id() < 100;
        const float scale = ImGui::GetScale();   // DPI 缩放，保证不同分辨率下比例一致
        // 单条对抗带的宽度不再写死：按窗口宽度自适应（见下方 band_width），窄屏也不会挤出去。
        const float bar_height = 16.0f * scale;   // 条高：正好放下一行 12px 数字
        const float label_gap = 6.0f * scale;     // 标签与条之间的间距
        const float segment_gap = 40.0f * scale;  // 两组数据之间的间距：要比组内间距大一圈，否则"26040"和"总伤害"会连成一串
        const ImU32 label_color = IM_COL32(170, 176, 186, 235);   // 标签灰

        // 段色比纯阵营色暗一档：白色数字压在饱和色上会发糊，压暗后对比更稳。
        const ImU32 ally_bar_color = we_are_red ? IM_COL32(198, 48, 50, 235) : IM_COL32(6, 128, 186, 235);   // 己方段
        const ImU32 enemy_bar_color = we_are_red ? IM_COL32(6, 128, 186, 235) : IM_COL32(198, 48, 50, 235);   // 对方段

        // 比例缓动：数据 1Hz 更新，直接切比例会"啪"地闪一下，这里按固定速度推过去（约 0.8 秒到位）。
        static float cp_ratio_shown = 0.5f;   // 控制度条当前显示的比例
        static float dmg_ratio_shown = 0.5f;  // 总伤害条当前显示的比例
        const float ratio_step = ImGui::GetIO().DeltaTime * 1.2f;   // 本帧允许变化的比例
        cp_ratio_shown = MoveTowards(cp_ratio_shown,
                                     CalcAllyRatio(unit_status.total_cp_ally(), unit_status.total_cp_enemy()), ratio_step);
        dmg_ratio_shown = MoveTowards(dmg_ratio_shown,
                                      CalcAllyRatio(unit_status.total_damage_ally(), unit_status.total_damage_enemy()), ratio_step);

        // 文字用 std::string 拼：ImFormatStringToTempBuffer 只有 3 个轮转缓冲，连续调用会互相覆盖，这里必须避开。
        const std::string cp_ally_text = std::to_string(unit_status.total_cp_ally());   // 己方控制度
        const std::string cp_enemy_text = std::to_string(unit_status.total_cp_enemy());  // 对方控制度
        const std::string dmg_ally_text = std::to_string(unit_status.total_damage_ally());   // 己方总伤害
        const std::string dmg_enemy_text = std::to_string(unit_status.total_damage_enemy());  // 对方总伤害

        // 图传清晰度：2027 规则 5.6.1，己方基地掉血会让图传按 λ² 降采样（含自定义终端画面）。
        // 顺带显示在这里，避免操作手把"画面变糊"误判成解码故障；放在同一行也省地方。
        std::string quality_text;   // 清晰度提示文本，空表示不显示
        ImVec4 quality_color(0.70f, 0.70f, 0.70f, 1.0f);   // 默认浅灰
        if (unit_status.base_health() < 10000) {   // 基地满血时 λ=1，不必提示
            const float lambda = 1.0f - static_cast<float>(10000 - unit_status.base_health()) / 20000.0f;   // λ = 1 − Δ/20000
            if (lambda <= 0.98f) {   // 损失不足 2% 时画质几乎无差别
                char quality_buffer[32];   // 格式化缓冲
                std::snprintf(quality_buffer, sizeof(quality_buffer), "图传 %.2f", lambda);   // 例："图传 0.88"
                quality_text = quality_buffer;   // 转成 std::string 便于算宽度
                if (lambda < 0.5f) {   // 严重降质
                    quality_color = ImVec4(0.95f, 0.30f, 0.30f, 1.0f);   // 红
                } else if (lambda < 0.75f) {   // 中度降质
                    quality_color = ImVec4(0.95f, 0.60f, 0.15f, 1.0f);   // 橙
                }   // 结束颜色选择
            }   // 结束轻微损失判断
        }   // 结束基地掉血判断

        // 整行用手工排版而不是 TextColored/SameLine：带要按比例分割，必须自己拿到绘制列表。
        ImGui::PushFont(ImGui::GetFont(), 12);   // 条高只有 16px，桌面字号放不下，统一降到 12px
        const char *cp_label = "控制度";   // 控制度标签
        const char *dmg_label = "总伤害";  // 总伤害标签
        const float label_w = std::max(ImGui::CalcTextSize(cp_label).x, ImGui::CalcTextSize(dmg_label).x);   // 两个标签取同宽，两组数据的带才能对齐
        const float number_slot = ImGui::CalcTextSize("0000000").x + 2.0f * scale;   // 数字槽按 7 位数预留：位数一跳，带不会左右抖
        const float gap = label_gap;   // 同一组内的元素间距
        const float quality_w = quality_text.empty() ? 0.0f : ImGui::CalcTextSize(quality_text.c_str()).x;   // 清晰度段宽
        // 固定部分 = 两组（标签 + 两个数字槽 + 三处间距）+ 组间隔 + 清晰度；剩下的宽度平分给两条带。
        const float fixed_width = 2.0f * (label_w + number_slot * 2.0f + gap * 3.0f)   // 两组标签/数字槽/间距
                                  + segment_gap                                       // 组间隔
                                  + (quality_text.empty() ? 0.0f : segment_gap + quality_w);   // 清晰度段（若有）
        // 底部条本身是通栏的，但内容不能压到右下的小地图上，所以居中区域扣掉右侧栏。
        const float content_width = viewport->Size.x * (1.0f - kRightColumnRatio);
        // 下限 80、上限 210：窄屏时带自己缩短，宽屏时也不至于长成一条进度条。
        const float band_width = std::clamp((content_width * 0.96f - fixed_width) * 0.5f, 80.0f * scale, 210.0f * scale);
        const float total_width = fixed_width + band_width * 2.0f;   // 整行宽

        ImGui::SetCursorPosX(std::max((content_width - total_width) * 0.5f, 0.0f));   // 在小地图左侧的内容区里居中（挤不下时贴左）
        const ImVec2 row_origin = ImGui::GetCursorScreenPos();   // 行起点（屏幕坐标）
        const float row_height = std::max(bar_height, ImGui::GetTextLineHeight());   // 行高取条高与字高较大者
        const float text_offset_y = (row_height - ImGui::GetTextLineHeight()) * 0.5f;   // 文字垂直居中补偿
        const float bar_offset_y = (row_height - bar_height) * 0.5f;   // 条形垂直居中补偿
        ImDrawList *draw_list = ImGui::GetWindowDrawList();   // 窗口绘制列表
        // 数字跟着阵营取色：白色压在饱和色上看不清，用同色系的提亮版本，扫一眼就知道左右是谁的。
        const ImU32 ally_number_color = we_are_red ? IM_COL32(240, 138, 138, 245) : IM_COL32(126, 200, 244, 245);
        const ImU32 enemy_number_color = we_are_red ? IM_COL32(126, 200, 244, 245) : IM_COL32(240, 138, 138, 245);

        // 一组数据 = 标签 + 己方数字（槽内右对齐、紧贴带左）+ 对抗带 + 对方数字（紧贴带右）。
        // 数字放在带外侧而不是带里：控制度是四位数、总伤害能到六位数，塞进 200px 的带里必然压线。
        const auto draw_score_group = [&](float x, const char *label, const std::string &ally_text,
                                          const std::string &enemy_text, float ratio) {
            draw_list->AddText(ImVec2(x, row_origin.y + text_offset_y), label_color, label);   // 标签
            x += label_w + gap;   // 让开标签
            const float ally_text_w = ImGui::CalcTextSize(ally_text.c_str()).x;   // 己方数字宽
            draw_list->AddText(ImVec2(x + number_slot - ally_text_w, row_origin.y + text_offset_y),
                               ally_number_color, ally_text.c_str());   // 己方数字：槽内右对齐
            x += number_slot + gap;   // 让开数字槽
            DrawControlBand(draw_list, ImVec2(x, row_origin.y + bar_offset_y), ImVec2(band_width, bar_height),
                            ratio, ally_bar_color, enemy_bar_color);   // 对抗带
            x += band_width + gap;   // 让开整条
            draw_list->AddText(ImVec2(x, row_origin.y + text_offset_y), enemy_number_color, enemy_text.c_str());   // 对方数字
            return x + number_slot;   // 本组右边界（含对方数字槽）
        };

        float cursor_x = draw_score_group(row_origin.x, cp_label, cp_ally_text, cp_enemy_text, cp_ratio_shown);   // 控制度组
        // 两组之间加一根极淡的竖线：这是两个不同量纲的数据（控制度是分钟级累积，伤害是六位数），
        // 光靠留白还是有被误读成一串的风险，一根分割线最省事。
        draw_list->AddLine(ImVec2(cursor_x + segment_gap * 0.5f, row_origin.y + 3.0f * scale),   // 竖线上端
                           ImVec2(cursor_x + segment_gap * 0.5f, row_origin.y + row_height - 3.0f * scale),   // 竖线下端
                           IM_COL32(150, 156, 168, 50), 1.0f);   // 颜色：中性灰、低透明度，不抢数字
        cursor_x += segment_gap;   // 组间隔
        cursor_x = draw_score_group(cursor_x, dmg_label, dmg_ally_text, dmg_enemy_text, dmg_ratio_shown);   // 总伤害组

        if (!quality_text.empty()) {   // 需要提示时才画
            cursor_x += segment_gap;   // 与前一组数据留间隔
            draw_list->AddText(ImVec2(cursor_x, row_origin.y + text_offset_y),
                               ImGui::ColorConvertFloat4ToU32(quality_color), quality_text.c_str());   // 输出清晰度
        }   // 结束清晰度输出

        ImGui::Dummy(ImVec2(total_width, row_height));   // 占位：推进光标，让后续状态行正常往下排

        if (show_split_line) {   // 第二行：把总控制度拆成"前哨贡献 / 基地贡献"
            // 这一行是推出来的，不是协议直接给的：服务器只下发 total_CP 一个数。
            // 两个速率差 3 倍，不拆开就看不出"基地阵地抢下来值不值"，所以单独列一行。
            const float split_y = row_origin.y + row_height + style.ItemSpacing.y;   // 紧贴上面一行
            char ally_buf[96];    // 己方构成文本
            char enemy_buf[96];   // 对方构成文本
            if (g_cp_split.ally_unknown > 0) {   // 有归不进阵地的增量时才多显示一段
                std::snprintf(ally_buf, sizeof(ally_buf), "前哨 %u · 基地 %u · 其他 %u",
                              g_cp_split.ally_outpost, g_cp_split.ally_base, g_cp_split.ally_unknown);
            } else {   // 正常情况：只有前哨与基地两段
                std::snprintf(ally_buf, sizeof(ally_buf), "前哨 %u · 基地 %u",
                              g_cp_split.ally_outpost, g_cp_split.ally_base);
            }   // 结束己方文本分支
            if (g_cp_split.enemy_unknown > 0) {   // 对方同理
                std::snprintf(enemy_buf, sizeof(enemy_buf), "前哨 %u · 基地 %u · 其他 %u",
                              g_cp_split.enemy_outpost, g_cp_split.enemy_base, g_cp_split.enemy_unknown);
            } else {   // 正常情况
                std::snprintf(enemy_buf, sizeof(enemy_buf), "前哨 %u · 基地 %u",
                              g_cp_split.enemy_outpost, g_cp_split.enemy_base);
            }   // 结束对方文本分支

            draw_list->AddText(ImVec2(row_origin.x, split_y), label_color, "构成");   // 行首标签，和上一行的"控制度/总伤害"左对齐
            float split_x = row_origin.x + label_w + gap;   // 与上方己方数字槽同一起点
            draw_list->AddText(ImVec2(split_x, split_y), ally_number_color, ally_buf);   // 己方构成
            split_x += ImGui::CalcTextSize(ally_buf).x + segment_gap;   // 让开己方构成，留出与上一行相同的组间距
            draw_list->AddText(ImVec2(split_x, split_y), label_color, "对方");   // 对方标签
            split_x += ImGui::CalcTextSize("对方").x + gap;   // 让开标签
            draw_list->AddText(ImVec2(split_x, split_y), enemy_number_color, enemy_buf);   // 对方构成
            ImGui::Dummy(ImVec2(total_width, ImGui::GetTextLineHeight()));   // 占位：让后续状态行往下排
        }   // 结束控制度构成行

        ImGui::PopFont();   // 恢复字号
    }   // 结束控制度/总伤害行

    for (const auto &line: status_lines) {  // 逐行绘制状态文本
        ImVec2 text_size = ImGui::CalcTextSize(line.text.c_str());  // 测量文本宽度以居中
        ImGui::SetCursorPosX((viewport->Size.x - text_size.x) / 2.0f);  // 水平居中定位光标
        ImGui::PushStyleColor(ImGuiCol_Text, line.color);  // 临时设置该行文本颜色
        ImGui::Text("%s", line.text.c_str());  // 绘制文本
        ImGui::PopStyleColor();  // 恢复文本颜色样式
    }  // 状态行遍历结束

    ImGui::End();  // 结束窗口绘制
    g_last_panel_height = panel_height;   // 记下本帧高度，供左下角本车状态面板让位
}  // 函数结束

// 功能：返回底部条上一帧占用的高度（供其它贴底面板让位）。
float GetBottomPanelHeight() {
    return g_last_panel_height;   // 首次调用（还没画过）返回 0，不影响布局
}   // 结束 GetBottomPanelHeight
