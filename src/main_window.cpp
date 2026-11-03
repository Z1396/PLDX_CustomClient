// main_window.cpp：主窗口实现——MQTT 下行消息分发与状态刷新、各面板布局调度、自定义控制包下发，以及本车/系统日志的维护。
#include "main_window.h"// 本模块头文件：引入 State 定义与 ShowMainWindow/HandleMqttMessage 等函数声明
#include "keyboard_mouse_state_manager.h"// 键鼠状态管理器：准星面板据此读取操作手的鼠标位移/按键状态
#include "logging/logging.h"// 日志封装：提供 SPDLOG_INFO/WARN/ERROR/DEBUG 等宏(spdlog 的薄封装)
#include "panel/bottom_panel.h"// 底部信息条面板：显示操作提示/状态摘要
#include "panel/buff_panel.h"// 增益面板：显示当前生效的 Buff 列表
#include "panel/commander_panel.h"// 指挥手综合指挥台：全屏地图 + 双方信息 + 哨兵/飞镖/空中指令
#include "panel/crosshair.h"// 准星面板：比赛中在全屏覆盖层上绘制自瞄准星
#include "panel/header_panel.h"// 顶部状态栏：比分/阶段/倒计时
#include "panel/log_panel.h"// 日志面板：滚动显示本车日志与系统日志
#include "panel/mini_map.h"// 小地图面板：绘制场地、自身与友军/敌方位置
#include "panel/robot_status_panel.h"// 本车状态面板：血条/热量/弹量等
#include "panel/start_panel.h"// 开局面板：赛前选型、连接与开始比赛入口

// 无名命名空间：本文件内部使用的 UI 辅助函数集合（字符串解析、日志缓存、视频缩放、标尺绘制），不对外暴露。
namespace {// 匿名命名空间开始：内部符号仅在当前翻译单元可见，避免与其他文件的同名函数冲突

    // 系统日志去重窗口：同一条文本在该时间窗内再次到达，视为发送端重复投递，折叠成 ×N 而不新增一行。
    // 取值依据：实测发送端会在同一毫秒内连发 4 份同一条判罚，且断流补发时整轮重复，2 秒足够覆盖这类突发。
    constexpr auto kSystemLogDedupeWindow = std::chrono::seconds(2);

    // 功能：把形如 "a,b" 的字符串按首个逗号拆成两段的键值对。
    // 思路：用 string::find 定位首个逗号，分别取左右子串；找不到逗号则返回空。
    std::optional<std::pair<std::string, std::string>> SplitByComma(const std::string &param) {// 声明：输入整串，成功时返回(左段,右段)二元组
        size_t comma_pos = param.find(',');// 在整串中查找第一个英文逗号的位置，找不到返回 std::string::npos
        if (comma_pos != std::string::npos) {// 找到逗号才继续拆分，否则直接走下面的兜底返回
            return std::make_pair(param.substr(0, comma_pos), param.substr(comma_pos + 1));// 逗号左侧为键、右侧为值(跳过逗号本身 1 个字符)
        }// if 结束
        return std::nullopt;// 无逗号(格式不符合预期)时返回空 optional，调用方用 has_value() 判断
    }// SplitByComma 结束

    // 功能：构造一条带本地时间戳与自增序号的日志并追加到指定缓冲区。
    // 思路：取当前时间格式化为 "HH:MM:SS"，分配自增 sequence，超 50 条则丢弃最旧一条以限长。
    void AppendLogBuffer(State &state, std::vector<LogEntry> &buffer, int type, const std::string &text) {// buffer 由调用方指定(本车日志或系统日志)
        LogEntry log_entry;// 在栈上先构造一条待填充的日志记录
        time_t now = time(nullptr);// 取当前 UTC 时间戳(自 1970 起的秒数)
        tm *timeinfo = localtime(&now);// 转成本地时间的分解结构 tm(注意返回静态缓冲，仅本函数内借用)
        strftime(log_entry.time, sizeof(log_entry.time), "%H:%M:%S", timeinfo);// 格式化为 8 字符 "HH:MM:SS"，长度刚好与 char time[9] 匹配(含结尾 '\0')
        log_entry.type = type;// 记录日志级别(0 INFO / 1 WARNING / 2 ERROR)，供面板着色
        log_entry.text = text;// 拷贝日志正文
        log_entry.sequence = state.next_log_sequence++;// 分配全局自增序号(后置 ++ 先取值再加一)，两个日志列表共用同一计数器
        buffer.push_back(log_entry);// 追加到目标缓冲区末尾
        if (buffer.size() > 50) {// 缓冲区超过 50 条即限长，避免内存无限增长(UI 也只展示最近若干条)
            buffer.erase(buffer.begin());// 丢弃最旧的一条(队首)，整体前移
        }// if 结束
    }// AppendLogBuffer 结束

    // 功能：把一条 Event 事件推进"顶部提示队列"，只保留需要在几秒内被看见的事件。
    // 思路：按 event_id 定紧要程度(0普通/1重要/2危急)，先清掉超过 6 秒的旧提示，再追加并限制总条数。
    void PushEventToast(State &state, uint32_t event_id, const std::string &text) {// 入参：全局状态、事件号、已翻译好的中文描述
        int level = -1;// -1 表示不提示(纯日志事件)
        switch (event_id) {// 按事件号决定是否需要弹提示
            case 1: level = 0; break;// 击杀：普通提示
            case 2: level = 2; break;// 前哨站被摧毁：危急
            case 5: case 6: case 7: case 8: level = 1; break;// 阵地控制变更：重要
            case 9: level = 2; break;// 飞镖命中：危急(2027 命中会致盲并大幅扣血)
            case 10: level = 1; break;// 对方飞镖闸门开启：重要
            case 11: level = 2; break;// 基地遭到攻击：危急
            case 12: level = 1; break;// 对方前哨站停转：重要
            case 13: level = 1; break;// 对方基地护甲展开：重要
            case 14: level = 2; break;// 重装战亡进入缓冲期：危急
            case 15: level = 1; break;// 装配结果：重要
            case 16: level = 1; break;// 模块化42mm安装成功：重要
            default: break;// 其余事件(能量机关等)只进日志，不弹提示
        }// switch 结束
        if (level < 0) {// 不需要提示的事件
            return;// 直接返回
        }// if 结束
        const auto now = std::chrono::steady_clock::now();// 当前单调时钟时刻
        // 先剔除超过 6 秒的旧提示(与渲染端的淡出时长一致)，避免队列越积越长。
        state.event_toasts.erase(// 用 remove_if + erase 做条件删除
                std::remove_if(state.event_toasts.begin(), state.event_toasts.end(),// 遍历全部提示
                               [&now](const EventToast &toast) {// 判定：是否已过期
                                   return std::chrono::duration_cast<std::chrono::milliseconds>(now - toast.time).count() > 6000;// 超过 6 秒视为过期
                               }),// remove_if 结束
                state.event_toasts.end());// 真正删除被移到尾部的元素
        state.event_toasts.push_back(EventToast{text, now, level});// 追加新提示(正文/时刻/级别)
        if (state.event_toasts.size() > 6) {// 同时最多保留 6 条
            state.event_toasts.erase(state.event_toasts.begin());// 丢弃最旧的一条
        }// if 结束
    }// PushEventToast 结束

    // 功能：按日志类型把文案输出到终端（spdlog），便于现场调试。
    // 思路：根据 type(0/1/2) 分别走 INFO/WARN/ERROR 级别，其余按 INFO 处理。
    void EmitUiLog(int type, const char *scope, const std::string &text) {// scope 为前缀常量("本车日志: "/"系统日志: ")
        switch (type) {// 按日志级别选择 spdlog 的输出宏
            case 0:// 0 = INFO
                SPDLOG_INFO("{}{}", scope, text);// 用两个 {} 占位符依次拼接前缀与正文，避免额外的字符串拼接开销
                break;// 跳出 switch
            case 1:// 1 = WARNING
                SPDLOG_WARN("{}{}", scope, text);// 警告级日志(通常是黄色高亮)
                break;// 跳出 switch
            case 2:// 2 = ERROR
                SPDLOG_ERROR("{}{}", scope, text);// 错误级日志(通常是红色高亮)
                break;// 跳出 switch
            default:// 未知级别，退化按 INFO 输出
                SPDLOG_INFO("{}{}", scope, text);// 兜底：保证日志不丢
                break;// 跳出 switch
        }// switch 结束
    }// EmitUiLog 结束

    // 【2027 变更】原 FitImageSize（自定义视频等比居中显示）已随"双路主画面切换"一起删除：
    // 背景层现在固定铺 UDP 图传，直接拉伸铺满视口；次画面面板有自己的等比缩放实现。

}// namespace// 匿名命名空间结束

// 功能：追加本车日志（操作手主动动作/状态）。
// 思路：写入车辆日志缓冲区并打印终端日志。
void AppendVehicleLog(State &state, int type, const std::string &text) {// 对外接口：type 为级别，text 为日志正文
    AppendLogBuffer(state, state.vehicle_logs, type, text);// 写入本车日志缓冲区(带时间戳/自增序号，自动限长 50 条)
    EmitUiLog(type, "本车日志: ", text);// 同步输出到终端日志(spdlog)，便于离屏排查
}// AppendVehicleLog 结束

// 功能：追加系统日志（裁判系统事件/判罚）。
// 思路：写入系统日志缓冲区并打印终端日志。
void AppendSystemLog(State &state, int type, const std::string &text) {// 对外接口：用于裁判系统事件、判罚、解析异常等
    // 去重：发送端在断流补发/整轮广播时会把同一条消息连发多份（实测同一毫秒 4 条判罚），
    // 若每条都记一行，日志面板会被无意义的重复文本刷满。这里在窗口期内把相同文本折叠成 ×N。
    const auto now = std::chrono::steady_clock::now();// 当前单调时钟时刻（不受系统改时间影响）
    if (!state.system_logs.empty()) {// 缓冲区非空才可能与上一条比较
        LogEntry &last_entry = state.system_logs.back();// 只看最后一条：日志是追加写入的，重复必然相邻
        if (last_entry.text == text && now - state.last_system_log_time <= kSystemLogDedupeWindow) {// 文本相同且在去重窗口内
            ++last_entry.repeat_count;// 只累加次数，不新增行
            state.last_system_log_time = now;// 顺延窗口，连续重复时始终折叠在同一条上
            return;// 直接返回：既不写新行，也不打终端日志，避免终端同样刷屏
        }// if 结束
    }// if 结束
    AppendLogBuffer(state, state.system_logs, type, text);// 写入系统日志缓冲区
    state.last_system_log_time = now;// 记录本条写入时刻，作为下一次去重的基准
    EmitUiLog(type, "系统日志: ", text);// 同步输出到终端日志(spdlog)，便于离屏排查
}// AppendSystemLog 结束

// 功能：构造初始状态。
// 思路：清零红蓝双方各 9 个机器人的血量/弹量后返回。
State CreateInitialState() {// 返回一个字段均已初始化的 State 值
    State state;// 默认构造：optional 字段为空，vector 为空，计数器为初值
    for (size_t i = 0; i < std::size(state.red_robots); i++) {// 遍历 9 个机器人槽位(std::size 编译期取数组元素个数)
        RobotStatus robot;// 构造一台机器人的状态快照
        robot.health = 0;// 血量清零(尚未收到裁判系统数据)
        robot.bullets = 0;// 允许发弹量清零
        state.red_robots[i] = robot;// 写入红方第 i 个槽位
        state.blue_robots[i] = robot;// 蓝方同索引写入相同初值
    }// for 结束
    return state;// 按值返回(依赖 RVO/移动语义，开销可忽略)
}// CreateInitialState 结束

// 功能：主界面总绘制入口。组织视频背景、顶部栏、状态栏、各功能面板与开局面板的绘制顺序，处理视频主画面切换。
// 思路：先绘制全屏视频背景（按主/次视频源选择并居中缩放），再按布局矩形逐个 SetNextWindow 摆放各 ImGui 面板；在比赛中绘制准星，否则绘制开局面板。返回是否请求切换 UDP 视频翻转。
bool ShowMainWindow(const State &state,// 全局状态快照(各面板只读渲染)
                    RoboGo::io::MqttClient &mqtt_client,// MQTT 客户端(开局面板用于下发选型/开始指令)
                    ImTextureRef video_texture_ref,// UDP 视频纹理引用
                    const ImVec2 &video_size,// UDP 视频原始像素尺寸
                    ImTextureRef custom_video_texture_ref,// CustomByteBlock 自定义视频纹理引用
                    const ImVec2 &custom_video_size,// 自定义视频原始像素尺寸
                    const bool udp_video_display_ready,// UDP 视频是否有可显示帧
                    const bool custom_video_display_ready,// 自定义视频是否有可显示帧
                    const bool udp_video_inverted,// UDP 视频当前是否上下翻转
                    const ImTextureRef mini_map_texture_ref,// 小地图底图纹理引用
                    const ImVec2 &mini_map_texture_size) {// 小地图底图原始像素尺寸
    // CustomByteBlock 次视频面板已删除，这三个入参暂无界面消费；保留形参是为了不动上层调用与纹理管线。
    (void) custom_video_texture_ref;
    (void) custom_video_size;
    (void) custom_video_display_ready;

    const ImGuiViewport *viewport = ImGui::GetMainViewport(); // 获取 ImGui 主视口(覆盖整个窗口的工作区矩形，用于全屏布局)
    ImGuiStyle &style = ImGui::GetStyle(); // 获取全局 ImGui 样式(含窗口内边距/间距/分隔线尺寸等布局参数)
    float font_size = ImGui::GetFontSize(); // 获取当前字体一行高度(像素)，用于估算面板尺寸

    // 【2027 变更】英雄部署模式已删除，不再有第二路可选主画面：主画面固定为 UDP 图传。
    const ImTextureRef background_video_texture_ref = video_texture_ref;      // 主画面纹理：UDP
    const ImVec2 background_video_size = video_size;                          // 主画面尺寸：UDP
    const bool background_video_display_ready = udp_video_display_ready;      // 主画面就绪标志：UDP

    // 指挥手：登录成功就直接铺开指挥台，不再停留在赛前那一页（那一页对指挥手没有任何可用指令）。
    maybe_auto_enter_commander_console(mqtt_client);

    // 指挥手：整页就是综合指挥台，与普通 HUD 互斥。
    // 这里直接分流并返回，避免两套布局在同一帧里互相压盖（指挥台自己会画顶栏、地图与指令区）。
    if (is_commander_console_active()) {// 指挥台已打开
        ShowCommanderPanel(state, mqtt_client,// 状态 + MQTT（指挥台要下发哨兵/飞镖/地图标记）
                           background_video_texture_ref, background_video_size, background_video_display_ready,// 现场画面预览
                           mini_map_texture_ref, mini_map_texture_size);// 战术地图底图
        return false;// 指挥台不需要翻转 UDP 视频，直接返回
    }// 指挥台分支结束
    // 视频就绪则把整张 UDP 图传贴到背景绘制层；2027 只剩一路，直接拉伸铺满，不再做等比适配。
    if (background_video_display_ready && background_video_texture_ref.GetTexID() != ImTextureID_Invalid) {// 就绪且纹理句柄有效(无效句柄贴图会画不出东西)
        ImGui::GetBackgroundDrawList()->AddImage(background_video_texture_ref, ImVec2(0, 0), viewport->Size, ImVec2(0, 0), ImVec2(1, 1)); // 铺满背景层
    }// if 结束(未就绪则本帧不绘制视频底图)
    // 【2027 变更】英雄部署模式已在 2027 协议中删除，原叠加在视频上的辅助标尺一并移除。

    // 顶部状态栏（高度占窗口 15%）。
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always); // 固定顶部栏到窗口左上角(ImGuiCond_Always=每帧都应用该位置)
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y * 0.15f)); // 顶部栏宽=全宽，高=窗口高 15%
    ShowHeaderPanel(state); // 绘制顶部状态栏(比分/比赛阶段倒计时等)


    // 左下角本车状态面板（宽 20%，高度按字体估算）。
    float robot_status_panel_height = font_size * 11 + style.SeparatorSize + 4.0f * 4 + style.ItemSpacing.y * 11 + style.WindowPadding.y * 2;   // 2027 新增了"模块化42mm装配权限"一行，行数 10→11 // 估算本车状态面板高度(基于字体行高+分隔线+内边距)
    // 底部条也是贴底的，而且行数会变（多一行"控制度构成"就高 20px）。本车状态面板按底栏上一帧的高度往上让位，
    // 否则两块贴底面板会叠在一起，底栏那行的"控制度"会压在本车状态最后一行上。
    const float bottom_panel_height = GetBottomPanelHeight();   // 上一帧底部条高度（首帧为 0）
    ImGui::SetNextWindowPos(ImVec2(0, viewport->Size.y - robot_status_panel_height - bottom_panel_height), ImGuiCond_Always); // 本车状态面板定位到左下角(贴底，且让开底栏)
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.2f, robot_status_panel_height)); // 本车状态面板尺寸=全宽20% × 估算高度
    ShowRobotStatusPanel(state); // 绘制本车状态面板(红蓝机器人血条/弹量)

    // 左侧栏：2027 取消部署模式后不再有 CustomByteBlock 次视频画面，整栏留给日志面板。
    // UDP 画面反转开关已一并挪进登录控制面板（见 StartPanel）。
    const float left_panel_width = viewport->Size.x * 0.2f; // 左侧栏宽度=全宽 20%
    const float left_panel_top = viewport->Size.y * 0.15f; // 左侧栏顶部起点=窗口高 15%(顶部栏下方)
    const float left_panel_total_height = viewport->Size.y - viewport->Size.y * 0.2f - robot_status_panel_height - bottom_panel_height; // 左栏总高=窗口高 - 顶部15% - 底部本车面板 - 底栏（三者都不能压）

    ImGui::SetNextWindowPos(ImVec2(0, left_panel_top), ImGuiCond_Always); // 日志面板贴左栏顶部
    ImGui::SetNextWindowSize(ImVec2(left_panel_width, left_panel_total_height)); // 占满整个左栏
    ShowLogPanel(state); // 绘制日志面板(本车/系统日志列表)

    // 本帧是否请求翻转 UDP 画面：按钮在登录控制面板里，比赛中不再有（键盘 I 仍可用）。
    bool toggle_udp_video_inversion_requested = false;

    // 比赛中绘制准星覆盖全屏；否则绘制开局准备面板（居中 750x500 窗口）。
    if (is_in_game_mode()) {// 全局函数：当前是否已进入比赛(由上层状态机维护)
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always); // 准星覆盖层定位到全屏左上角
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y)); // 准星层尺寸=全屏
        ShowCrosshair(state); // 绘制全屏准星(比赛中显示)
    } else {// 未进入比赛：显示开局面板
        float width = 750 * ImGui::GetScale(); // 开局面板基准宽度=750 × 全局 UI 缩放
        // 高度从 500 加到 560：2027 里重装/哨兵的功能面板比 2026 多（装配+核心状态、哨兵 12 条指令），
        // 500 高装不下，只能靠滚动；加高 60px 后「登录区 + 全部功能面板 + 进入比赛」基本能一屏放完。
        float height = 560 * ImGui::GetScale(); // 开局面板基准高度=560 × 全局 UI 缩放
        ImGui::SetNextWindowPos(ImVec2((viewport->Size.x - width) / 2, (viewport->Size.y - height) / 2), ImGuiCond_Always); // 开局面板居中放置
        ImGui::SetNextWindowSize(ImVec2(width, height)); // 开局面板尺寸=750×560(按缩放)
        toggle_udp_video_inversion_requested = StartPanel(state, mqtt_client, udp_video_inverted); // 绘制开局准备面板，并取回是否点了 UDP 反转
    }// if-else 结束

    // 右下：小地图（按 28:15 标准场地比例计算高度）。
    // 右侧整栏宽度 20% → 24%：20% 时地图只有 ~380px 宽，地形缩得看不清，标点也显得很大。
    const float right_column_ratio = 0.24f; // 右侧栏（小地图 + Buff 面板）占全宽比例
    float mini_map_width = viewport->Size.x * right_column_ratio; // 小地图宽度=全宽 24%
    float mini_map_height = mini_map_width / 28 * 15; // 小地图高度按 28:15 标准场地比例换算
    ImGui::SetNextWindowPos(ImVec2(viewport->Size.x - mini_map_width, viewport->Size.y - mini_map_height), ImGuiCond_Always); // 小地图定位到右下角
    ImGui::SetNextWindowSize(ImVec2(mini_map_width, mini_map_height)); // 小地图尺寸
    ShowMiniMap(state, mqtt_client, mini_map_texture_ref, mini_map_texture_size); // 绘制小地图面板(自身/友军/雷达位置)

    // 右侧中部：增益/Buff 面板，占据顶部栏以下到小地图以上的区域。
    float buff_panel_width = viewport->Size.x * right_column_ratio; // Buff 面板宽度与小地图同宽，保证右边缘对齐
    float buff_panel_height = viewport->Size.y - viewport->Size.y * 0.15f - mini_map_height; // Buff 面板高=窗口高-顶部15%-小地图高
    ImGui::SetNextWindowPos(ImVec2(viewport->Size.x - buff_panel_width, viewport->Size.y * 0.15f), ImGuiCond_Always); // Buff 面板定位到右上(顶部栏下方)
    ImGui::SetNextWindowSize(ImVec2(buff_panel_width, buff_panel_height)); // Buff 面板尺寸
    ShowBuffPanel(state); // 绘制增益/Buff 面板(当前生效增益列表)

    // 底部信息条。
    ShowBottomPanel(state); // 底部条不预设位置/尺寸，由面板自身定位

    // 三个覆盖层放在最后创建：ImGui 按窗口创建顺序叠放，放早了会被开局面板/小地图盖住
    // （实测判罚横幅正好落在开局面板范围内，看不见）。
    ShowPenaltyBanner(state); // 生效判罚的顶部横幅(黄牌/红牌/超限)
    ShowEventToasts(state); // 重要事件的顶部浮层提示

    // 把"是否请求翻转 UDP 视频"沿调用链返回给上层。
    return toggle_udp_video_inversion_requested;// 返回值=true 表示用户本帧点击了翻转开关
}// ShowMainWindow 结束

// 功能：每帧推进本地状态。目前负责清理已超出剩余时间的 Buff。
// 思路：遍历 active_buffs，用当前时间减去更新时间得到已持续秒数，若 >= buff 剩余时间则移除；并统计清理条数用于调试日志。
void UpdateState(State &state) {// 由主循环每帧调用一次
    // 取单调时钟当前时刻(不受系统时间调整影响，适合做间隔/超时判断)
    auto now = std::chrono::steady_clock::now();

    const std::size_t previous_size = state.active_buffs.size();// 记录清理前的条数，用于后面算出"清掉了几条"
    // C++20 的"按谓词删除"算法：遍历容器，谓词为 true 的元素被移除
    std::erase_if(state.active_buffs,
            // 判据 lambda：捕获 now(值捕获)，入参是一条 Buff 记录
            [now](const BuffWithTimestamp &buff_entry) {
                // 把时间差强制转换成整秒，已持续时间 = 当前时刻 - 该 Buff 的接收时刻
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                        now - buff_entry.update_time)// 
                                        .count();// 取出秒数的数值部分(long long)
                return elapsed >= static_cast<int>(buff_entry.buff.buff_left_time());// 已持续时间 >= 协议给的剩余时间 → 判定过期，返回 true 触发删除
            });// erase_if 结束

    // 前后条数相减即本次清理掉的条数
    const std::size_t removed = previous_size - state.active_buffs.size();
    // 仅在有清理时打日志，避免每帧刷屏
    if (removed > 0) {
        SPDLOG_DEBUG("清理过期Buff removed={} remaining={}", removed, state.active_buffs.size());// DEBUG 级：清掉几条、还剩几条
    }// if 结束
}// UpdateState 结束

// 功能：按需（到达发布周期）构造并下发 CustomControl 控制包，把敌方血量透传给机器人电控。
// 思路：先判断是否到发布时刻及基本状态是否就绪；再从 GlobalUnitStatus 取出对方阵营血量组装成定长字节载荷，附加自增调试计数后序列化发布。
void PublishCustomControlIfDue(State &state, RoboGo::io::MqttClient &mqtt_client) {// 由主循环每帧调用，内部自行节流
    const auto now = std::chrono::steady_clock::now();// 取本次调用的单调时钟时刻，作为节流基准与时间戳
    // 未到发布周期则跳过，避免高频下发。
    if (!IsCustomControlPublishDue(state.last_custom_control_publish_time, now)) {// 距上次发布不足一个周期(或参数判定不该发)
        return;// 直接返回，本帧不下发
    }// if 结束
    // 缺乏己方静态信息或全局血量信息无法定位敌方，直接返回。
    if (!state.robot_static_status.has_value() || !state.global_unit_status.has_value()) {// 任一关键快照缺失都无法组装包
        return;// 尚未收到必要数据，跳过本次下发
    }// if 结束

    // 血量数组前半为己方、后半为对方。2027 协议每方 4 台(1,3,4,7)，旧协议为 5 台，故只做基本尺寸校验。
    const auto &robot_health = state.global_unit_status->robot_health();// 取血量数组(protobuf repeated 字段的只读引用，避免拷贝)
    const auto robot_health_size = robot_health.size();// 数组总长度(2027 通常是 8：己方 4 + 对方 4)
    const auto half_robot_health_size = robot_health_size / 2;// 折半点，即"己方"段长度，也是对方段起始下标
    if (half_robot_health_size < 4// 每方至少 4 台才有意义(2027 下限)
        || robot_health_size < half_robot_health_size * 2) {// 前后两段长度必须自洽
        return;// 数据不足，放弃本次下发(否则会越界读数)
    }// if 结束

    std::array<uint16_t, kCustomControlRobotCount> enemy_health{}; // 准备敌方血量缓冲(定长 5 台)
    if (half_robot_health_size == 4) {// 2027 标准：每方 4 台，顺序为 1,3,4,7（已去掉 2 号工程）
        // 4 台/方时协议段内下标 0..3 分别对应 1号/3号/4号/7号；
        // 载荷槽位固定为 重装(0)/保留(1)/步兵3(2)/步兵4(3)/哨兵(4)，因此映射为 0←1号, 2←3号, 3←4号, 4←7号。
        constexpr int kEnemySlotFromIndex[4] = {0, 2, 3, 4};// 段内下标 -> 载荷槽位
        for (int k = 0; k < 4; k++) {// 依次拷贝 4 台敌方机器人的血量
            // 取对方单位血量并钳制到 uint16 范围。
            const uint16_t hp = static_cast<uint16_t>(std::min(robot_health[half_robot_health_size + k], uint32_t{65535}));// 协议里血量是 uint32，下行包只有 2 字节，故先钳位再收窄
            enemy_health[kEnemySlotFromIndex[k]] = hp; // 写入对应的载荷槽位
        }// for 结束
        enemy_health[1] = 0; // 工程槽位(2 号)在 2027 血量数组中已不存在，先填 0，槽位定义待机器人端确认
    } 
    else if (robot_health_size >= half_robot_health_size + static_cast<int>(kCustomControlRobotCount)) {// 旧协议容错：每方 ≥5 台时按旧顺序直取 5 台
        for (size_t k = 0; k < kCustomControlRobotCount; k++) {// 依次拷贝 5 台敌方机器人的血量
            const int enemy_index = half_robot_health_size + static_cast<int>(k);// 敌方段起始下标 + 偏移
            // 取对方单位血量并钳制到 uint16 范围。
            const uint16_t hp = static_cast<uint16_t>(std::min(robot_health[enemy_index], uint32_t{65535}));// 协议里血量是 uint32，下行包只有 2 字节，故先钳位再收窄
            enemy_health[k] = hp; // 写入第 k 个敌方机器人血量
        }// for 结束
    } 
    else {// 段长度既不是 2027 的 4，也不够旧协议的 5
        return;// 数据不足，放弃本次下发
    }// if-else 结束

    // 自增调试计数器，用于下位机识别每帧控制包的新鲜度。
    static uint8_t debug_increment_counter = 0; // 模块级自增调试计数器(跨调用保持，供下位机判断控制包新鲜度)
    const uint8_t current_debug_counter = debug_increment_counter;// 先取出当前值，作为本次包的计数
    debug_increment_counter = static_cast<uint8_t>(debug_increment_counter + 1);// 再自增(uint8_t 会自然回绕 255→0)
    auto data = BuildCustomControlPayload(enemy_health,// 组装定长字节载荷：敌方 5 个血量槽
                                          current_debug_counter);// 本次包的调试计数
    CustomControl custom_control; // 构造 CustomControl 协议消息
    custom_control.set_data(std::string(data.begin(), data.end()));// 把字节数组塞进 protobuf 的 bytes 字段(用迭代器区间构造 std::string)
    std::string serialized; // 准备接收序列化后的字节串
    // 序列化成功则发布到 "CustomControl" 话题，并记录本次发布时间。
    if (custom_control.SerializeToString(&serialized)) {// protobuf 序列化：成功返回 true 并把二进制写入 serialized
        mqtt_client.publish("CustomControl", serialized);// 通过 MQTT 发布到自定义控制话题，机器人电控订阅该话题
        state.last_custom_control_publish_time = now; // 记录本次发布时刻，供下次节流判断
    }// if 结束(序列化失败则保持上次时间戳不变，下帧会再试)
    // 【2027 变更】原空中支援的"呼叫标志/反制冷却"两字节已改为保留位并固定填 0，不再打印其内容。
    SPDLOG_INFO("CustomControl 已发出: 重装={} 保留={} 步兵3={} 步兵4={} 哨兵={} 计数器={}",// 汇总打印本次下发的完整内容
                enemy_health[0],// 敌方重装血量
                enemy_health[1],// 保留槽(2027 无 2 号，恒为 0)
                enemy_health[2],// 敌方步兵3血量
                enemy_health[3],// 敌方步兵4血量
                enemy_health[4],// 敌方哨兵血量
                data[kCustomControlDebugCounterOffset]);// 调试计数器
}// PublishCustomControlIfDue 结束

// 功能：MQTT 入站消息的统一分发入口，按 topic 反序列化对应 protobuf 并刷新 State。
// 思路：用 if-else 链按 topic 匹配；解析成功则覆盖 State 快照（或追加日志/透传字节块），失败则记错误日志；未识别 topic 记警告。
void HandleMqttMessage(State &state,// 待刷新的全局状态
                       RoboGo::io::MqttClient &mqtt_client,// MQTT 客户端引用(本函数暂未使用，保留以便将来下行应答)
                       const std::string &topic,// 话题名 = 裁判系统消息类型
                       const std::string &payload,// 消息二进制载荷
                       const CustomByteBlockPacketHandler &custom_byte_block_packet_handler) // 自定义字节块(视频帧)透传回调
{   
    // 下行分发本文件暂未使用 mqtt_client 参数，显式 void 转换以抑制未使用警告
    (void) mqtt_client; 
    // MQTT 入站消息的统一分发入口：
    // 1. 根据 topic 选择对应的 protobuf 类型并反序列化 payload。
    // 2. 把最新数据写入 State，供各个 ImGui 面板读取显示。
    const auto log_parse_failure = [&](const std::string &current_topic) {
        // lambda：统一的解析失败日志(捕获 payload 以取长度)
        SPDLOG_ERROR("MQTT消息解析失败 topic={} bytes={}", current_topic, payload.size());// ERROR 级：打出话题名与载荷字节数，便于排查协议不匹配
    };// lambda 结束
    const auto log_parse_success = [&](const std::string &current_topic, const auto &) {
        // lambda：统一的解析成功日志(第二个参数为消息本体，仅用于类型推导，不直接使用)
        SPDLOG_DEBUG("MQTT消息解析成功 topic={}", current_topic);// DEBUG 级：高频消息(10Hz)只在调试时输出，避免刷屏
    };// lambda 结束

    // 普通同步类消息：解析成功后直接覆盖 State 中的最新快照。
    if (topic == "GameStatus") {// 2.2.3 比赛全局状态(1Hz)
        GameStatus game_status;// 在栈上构造消息对象(protobuf 生成类)，字段默认零值
        if (game_status.ParseFromString(payload)) {// 反序列化：从 payload 解析，成功返回 true，失败返回 false(数据损坏或类型不符)
            log_parse_success(topic, game_status);// 打一条调试日志
            // 【2027 变更】空中支援已移除，原"进入比赛中即重置空中支援反制次数"的上升沿处理一并删除。
            state.game_status = game_status; // 解析成功，覆盖本地比赛状态快照(供面板读取)
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "GlobalUnitStatus") {// 2.2.4 全局单位状态(1Hz)：双方基地/前哨站血量、10 台机器人血量、弹量、总伤害
        GlobalUnitStatus global_unit_status;// 构造消息对象
        if (global_unit_status.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, global_unit_status);// 调试日志
            state.global_unit_status = global_unit_status; // 解析成功，覆盖本地全局单位状态快照

            // GlobalUnitStatus 同时驱动左右双方血量/弹量面板，并把对方血量打包成
            // CustomControl 下发给机器人电控使用。
            const auto &robot_health = global_unit_status.robot_health();// 取血量数组只读引用(避免拷贝 repeated 字段)
            const auto update_robot_health = [](RobotStatus &robot, uint32_t health) {// 小工具 lambda：把一次血量读数写入 UI 结构
                robot.health = health;// 覆盖当前血量(血量会因受伤下降，也可能因补血上升)
                robot.max_health = std::max(robot.max_health, health);// 只向上更新"历史最大血量"，作为血条 100% 的基准
            };// lambda 结束
            bool is_red_team = state.robot_static_status.has_value() && state.robot_static_status->robot_id() < 100;// robot_id<100 判为红方(100+ 为蓝方)；未收到静态消息时按蓝方处理
            auto robot_health_size = robot_health.size();// 血量数组总长度
            auto half_robot_health_size = robot_health_size / 2;// 折半点：前半为己方、后半为对方

            // 【2027 规范】robot_health 数组顺序：己方 1,3,4,7 + 对方 1,3,4,7 (共8个，每方4台，已去掉 2 号工程)
            // 映射到客户端数组：red_robots/blue_robots[0-8]，其中：
            // 0=重装(1), 1=保留(2), 2=步兵(3), 3=步兵(4), 4=废弃步兵(5), 5=空中系统(6), 6=哨兵(7), 7=飞镖(8), 8=雷达(9)
            // 服务器发送的 2027 顺序 1,3,4,7 对应客户端索引 0,2,3,6；旧协议(每方5台)顺序 1,2,3,4,7 对应 0,1,2,3,6。
            const int client_indices_2027[] = {0, 2, 3, 6};// 2027：1号->0, 3号->2, 4号->3, 7号->6
            const int client_indices_legacy[] = {0, 1, 2, 3, 6};// 旧协议：1号->0, 2号->1, 3号->2, 4号->3, 7号->6
            // 自适应：每方 4 台按 2027 映射，否则沿用旧的 5 台映射。
            const int *client_indices = half_robot_health_size == 4 ? client_indices_2027 : client_indices_legacy;// 选出本帧使用的映射表
            const int health_count = std::min<int>(half_robot_health_size,// 本帧要遍历的台数，不能超过前半段长度
                                                  half_robot_health_size == 4 ? 4 : 5);// 2027 取 4 台，旧协议取 5 台
            const int enemy_offset = half_robot_health_size;// 对方段起始下标

            for (int i = 0; i < health_count; i++) {// 遍历前半段(己方)的下标
                // 根据 proto 注释，robot_health 数组顺序固定为「己方在前，对方在后」
                int self_index = i;// 己方元素下标：就在前半段内
                int enemy_index = i + enemy_offset;// 对方元素下标：前半段长度 + 偏移

                // 映射到客户端数组的正确索引
                int client_index = client_indices[i];// 把协议顺序换算成本地数组下标
                if (is_red_team) {// 本机是红方
                    update_robot_health(state.red_robots[client_index], robot_health[self_index]);// 前半段写入红方
                    update_robot_health(state.blue_robots[client_index], robot_health[enemy_index]);// 后半段写入蓝方
                } else {// 本机是蓝方，前半段是蓝方自己
                    update_robot_health(state.blue_robots[client_index], robot_health[self_index]);// 前半段写入蓝方
                    update_robot_health(state.red_robots[client_index], robot_health[enemy_index]);// 后半段写入红方
                }// if-else 结束
            }// for 结束
            const auto &robot_bullets = global_unit_status.robot_bullets();// 取允许发弹量数组(同样是己方在前、对方在后)
            auto robot_bullets_size = robot_bullets.size();// 弹量数组总长度
            auto half_robot_bullets_size = robot_bullets_size / 2;// 折半点
            for (int i = 0; i < half_robot_bullets_size; i++) {// 遍历前半段(己方)的下标
                int index = i;// 默认认为协议顺序与本地数组顺序一一对应
                // 由于官方没有明确说明顺序，以防万一，这里做一些容错处理
                if (half_robot_bullets_size == 4) {// 2027：每方 4 台，顺序 1 3 4 7
                    // 映射到本地数组索引 0,2,3,6（与血量数组一致）
                    index = client_indices_2027[i];// 直接查 2027 映射表
                } else if (half_robot_bullets_size == 6) {// 若数组是 6 台/方，说明包含了 6 号无人机
                    // 实际顺序
                    // 1 2 3 4 6 7
                    if (i >= 4) {// 从第 5 项起是 6 号和 7 号
                        index = i + 1;// 跳过本地数组里的 5 号(废弃步兵)槽位，整体后移一位
                    }// if 结束
                } else if (half_robot_bullets_size == 5) {// 若数组是 5 台/方
                    // 以防万一
                    // 1 3 4 6 7
                    if (i >= 3) {// 第 4、5 项对应 6 号和 7 号
                        index = i + 2;// 跳过 2 号(工程)与 5 号两个槽位，后移两位
                    } else if (i >= 1) {// 第 2、3 项对应 3 号和 4 号
                        index = i + 1;// 跳过 2 号(工程)一个槽位
                    }// 内层 if 结束
                }// if-else 结束
                int red_index;// 本帧要写入的红方数组下标(在弹量数组中的位置)
                int blue_index;// 本帧要写入的蓝方数组下标
                if (is_red_team) {// 本机红方：前半段属于红方
                    red_index = i;// 红方取前半段
                    blue_index = i + half_robot_bullets_size;// 蓝方取后半段
                } else {// 本机蓝方：前半段属于蓝方
                    red_index = i + half_robot_bullets_size;// 红方取后半段
                    blue_index = i;// 蓝方取前半段
                }// if-else 结束
                if (red_index < robot_bullets_size) {// 先做下标越界检查(协议长度可能异常)
                    state.red_robots[index].bullets = robot_bullets[red_index];// 写入红方第 index 号机器人的允许发弹量
                }// if 结束
                if (blue_index < robot_bullets_size) {// 同样检查蓝方下标
                    state.blue_robots[index].bullets = robot_bullets[blue_index];// 写入蓝方第 index 号机器人的允许发弹量
                }// if 结束
            }// for 结束
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "GlobalLogisticsStatus") {// 2.2.5 全局后勤信息(1Hz)：剩余经济、累计经济、科技等级、加密等级
        GlobalLogisticsStatus global_logistics_status;// 构造消息对象
        if (global_logistics_status.ParseFromString(payload)) {
            // 反序列化成功
            log_parse_success(topic, global_logistics_status);// 调试日志
            state.global_logistics_status = global_logistics_status; // 解析成功，覆盖本地全局后勤状态快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "GlobalSpecialMechanism") {// 2.2.6 全局特殊机制(1Hz)：堡垒占领计时等机制 ID 与时间参数
        GlobalSpecialMechanism global_special_mechanism;// 构造消息对象
        if (global_special_mechanism.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, global_special_mechanism);// 调试日志
            state.global_special_mechanism = global_special_mechanism; // 解析成功，覆盖本地全局特殊机制快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "Event") {// 2.2.7 比赛事件(触发式)：击杀、前哨站被毁、能量机关、阵地控制、装配结果等
        Event event;// 构造消息对象
        if (event.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, event);// 调试日志
            LogEntry log_entry; // 构造一条系统(事件)日志
            time_t now = time(nullptr); // 取当前日历时间(秒级)
            tm *timeinfo = localtime(&now); // 转为本地时间分解结构(tm)
            strftime(log_entry.time, sizeof(log_entry.time), "%H:%M:%S", timeinfo); // 格式化为 "HH:MM:SS" 存入时间字段
            log_entry.type = 0; // 事件日志默认 INFO 级别
            switch (event.event_id()) {// 按事件 ID 分派，拼出人类可读的事件描述
                case 1: {// 1 = 击杀事件
                    // 击杀事件（参数 1 为被击杀者，参数 2 为击杀者，如"1,101"，代表蓝方重装击毁红方重装）
                    auto parts = SplitByComma(event.param());// 把 "被击杀者,击杀者" 按首个逗号拆开
                    if (parts.has_value()) {// 拆分成功：两侧都能翻译成中文名
                        log_entry.text = GetRobotName(parts->first) + " 击杀了 " + GetRobotName(parts->second);// 拼出"红方X 击杀了 蓝方Y"这样的句子
                    } else {// 参数格式不符合预期(没有逗号)
                        log_entry.text = "击杀事件: " + event.param();// 退化：原样附上参数文本
                    }// if-else 结束
                    break;// 跳出 switch，走统一的日志落库流程
                }// case 1 结束
                case 2: {// 2 = 前哨站被摧毁事件
                    // 前哨站被摧毁事件（参数值为被击毁的目标 id，红方前哨站为 11，蓝方前哨站为 111）
                    const std::string &param = event.param();// 取参数文本的只读引用(避免拷贝)
                    std::string target_name;// 待拼装的目标名称
                    if (param == "11") {// 11 = 红方前哨站
                        target_name = "红方前哨站";// 直接给出中文名
                    } else if (param == "111") {// 111 = 蓝方前哨站
                        target_name = "蓝方前哨站";// 直接给出中文名
                    } else {// 未知 ID
                        target_name = "前哨站" + param;// 兜底：把原始 ID 拼在后面，便于排查
                    }// if-else 结束
                    log_entry.text = target_name + "被摧毁";// 组合成一句完整事件描述
                    break;// 跳出 switch
                }// case 2 结束
                case 3: {// 3 = 大能量机关激活成功
                    // 当前大能量机关被成功激活的灯臂数量、平均环数（仅大能量机关被激活时触发该事件，参数值为激活成功臂数，平均环数(浮点数)，比如”10,9.6”代表激活十个灯臂，平均环数为 9.6）
                    // 说明：此处原有一句 `return;`（TODO 暂时不处理），会把整条事件吞掉——连后面的
                    //       系统日志都写不进去。现已改为正常分支，只写日志、不弹提示（PushEventToast 未给 3 定级）。
                    auto parts = SplitByComma(event.param());// 把 "臂数,平均环数" 拆开
                    if (parts.has_value()) {// 拆分成功
                        log_entry.text = "大能量机关激活成功: " + parts->first + "个灯臂，平均环数" + parts->second;// 拼出激活详情
                    } else {// 参数格式异常
                        log_entry.text = "大能量机关激活: " + event.param();// 退化为原始参数
                    }// if-else 结束
                    break;// 跳出 switch
                }// case 3 结束
                case 4: {// 4 = 能量机关进入已激活状态
                    // 能量机关进入已激活状态（激活类型 1：小能量机关，2：大能量机关）
                    const std::string &param = event.param();// 参数即激活类型
                    if (param == "1") {// 1 = 小能量机关
                        log_entry.text = "小能量机关已激活";// 小符
                    } else if (param == "2") {// 2 = 大能量机关
                        log_entry.text = "大能量机关已激活";// 大符
                    } else {// 未知类型
                        log_entry.text = "能量机关已激活: " + param;// 附上原始参数
                    }// if-else 结束
                    break;// 跳出 switch
                }// case 4 结束
                case 5: {// 5 = R 前哨阵地改变控制状态
                    // R 前哨阵地改变控制状态（参数 1 红方临时控制，2 红方完全控制，3 蓝方临时控制，4 蓝方完全控制）
                    const std::string &param = event.param();// 参数即控制状态码
                    std::string state_name;// 待翻译的控制状态文案
                    if (param == "1") state_name = "红方临时控制";// 1 = 红方临时控制
                    else if (param == "2")// 2 = 红方完全控制
                        state_name = "红方完全控制";// 红方完全掌控
                    else if (param == "3")// 3 = 蓝方临时控制
                        state_name = "蓝方临时控制";// 蓝方临时掌控
                    else if (param == "4")// 4 = 蓝方完全控制
                        state_name = "蓝方完全控制";// 蓝方完全掌控
                    else// 未知状态码
                        state_name = "未知状态(" + param + ")";// 兜底：把原始码嵌进文案
                    log_entry.text = "R 前哨阵地控制状态变更: " + state_name;// 组合成完整描述
                    break;// 跳出 switch
                }// case 5 结束
                case 6: {// 6 = B 前哨阵地改变控制状态
                    // B 前哨阵地改变控制状态（参数含义与 case 5 相同：1 红临时 / 2 红完全 / 3 蓝临时 / 4 蓝完全）
                    const std::string &param = event.param();// 参数即控制状态码
                    std::string state_name;// 待翻译的控制状态文案
                    if (param == "1") state_name = "红方临时控制";// 1 = 红方临时控制
                    else if (param == "2")// 2 = 红方完全控制
                        state_name = "红方完全控制";// 红方完全掌控
                    else if (param == "3")// 3 = 蓝方临时控制
                        state_name = "蓝方临时控制";// 蓝方临时掌控
                    else if (param == "4")// 4 = 蓝方完全控制
                        state_name = "蓝方完全控制";// 蓝方完全掌控
                    else// 未知状态码
                        state_name = "未知状态(" + param + ")";// 兜底：把原始码嵌进文案
                    log_entry.text = "B 前哨阵地控制状态变更: " + state_name;// 组合成完整描述
                    break;// 跳出 switch
                }// case 6 结束
                case 7: {// 7 = R 基地阵地改变控制方
                    // R 基地阵地改变控制方（参数 1 红方完全控制，2 蓝方临时控制）
                    const std::string &param = event.param();// 参数即控制方状态码
                    if (param == "1") {// 1 = 红方完全控制
                        log_entry.text = "R 基地阵地控制方变更: 红方完全控制";// 红方完全掌控 R 基地
                    } else if (param == "2") {// 2 = 蓝方临时控制
                        log_entry.text = "R 基地阵地控制方变更: 蓝方临时控制";// 蓝方临时掌控 R 基地
                    } else {// 未知状态码
                        log_entry.text = "R 基地阵地控制方变更: 未知状态(" + param + ")";// 兜底：附上原始码
                    }// if-else 结束
                    break;// 跳出 switch
                }// case 7 结束
                case 8: {// 8 = B 基地阵地改变控制方
                    // B 基地阵地改变控制方（参数 1 蓝方完全控制，2 红方临时控制）
                    const std::string &param = event.param();// 参数即控制方状态码
                    if (param == "1") {// 1 = 蓝方完全控制
                        log_entry.text = "B 基地阵地控制方变更: 蓝方完全控制";// 蓝方完全掌控 B 基地
                    } else if (param == "2") {// 2 = 红方临时控制
                        log_entry.text = "B 基地阵地控制方变更: 红方临时控制";// 红方临时掌控 B 基地
                    } else {// 未知状态码
                        log_entry.text = "B 基地阵地控制方变更: 未知状态(" + param + ")";// 兜底：附上原始码
                    }// if-else 结束
                    break;// 跳出 switch
                }// case 8 结束
                case 9: {// 9 = 飞镖命中
                    // 飞镖命中（第一个参数为命中方，1 为红方命中，2 为蓝方命中；第二个参数值为命中目标）
                    auto parts = SplitByComma(event.param());// 拆成 "命中方,命中目标"
                    if (parts.has_value()) {// 拆分成功
                        std::string side_name = (parts->first == "1") ? "红方" : "蓝方";// 1 记为红方命中，其余按蓝方处理
                        const std::string &target = parts->second;// 命中目标编号

                        std::string target_name;// 待翻译的目标名称
                        if (target == "1") target_name = "前哨站";// 1 = 前哨站
                        else if (target == "2")// 2 = 基地固定目标
                            target_name = "基地固定目标";// 固定靶
                        else if (target == "3")// 3 = 基地随机固定目标
                            target_name = "基地随机固定目标";// 随机固定靶
                        else if (target == "4")// 4 = 基地随机移动目标
                            target_name = "基地随机移动目标";// 随机移动靶
                        else if (target == "5")// 5 = 基地末端移动目标
                            target_name = "基地末端移动目标";// 末端移动靶
                        else// 其他编号
                            target_name = "目标" + target;// 兜底：附上原始编号

                        log_entry.text = side_name + "飞镖命中" + target_name;// 拼出"红方飞镖命中基地固定目标"之类
                    } else {// 参数格式异常
                        log_entry.text = "飞镖命中: " + event.param();// 退化为原始参数
                    }// if-else 结束
                    break;// 跳出 switch
                }// case 9 结束
                case 10: {// 10 = 对方飞镖闸门开启
                    // 对方飞镖闸门开启（无参数）
                    log_entry.text = "对方飞镖闸门开启";// 固定文案
                    break;// 跳出 switch
                }// case 10 结束
                case 11: {// 11 = 基地遭到攻击
                    // 基地遭到攻击（无参数值，每次触发存在 5s 内置冷却）
                    // 说明：此处原有一句 `return;`（TODO 暂时不处理），导致这条"危急"事件既不写日志、
                    //       也不弹顶部提示（PushEventToast 给 11 定级为 level 2）。现已改为正常分支。
                    log_entry.text = "基地遭到攻击";// 固定文案
                    break;// 跳出 switch
                }// case 11 结束
                case 12: {// 12 = 对方前哨站停转
                    // 对方前哨站停转（无参数）
                    log_entry.text = "对方前哨站停转";// 固定文案
                    break;// 跳出 switch
                }// case 12 结束
                case 13: {// 13 = 对方基地护甲展开
                    // 对方基地护甲展开（无参数）
                    log_entry.text = "对方基地护甲展开";// 固定文案
                    break;// 跳出 switch
                }// case 13 结束
                case 14: {// 14 = 重装战亡临时激活，进入强制退出缓冲期
                    // 重装战亡临时激活，进入强制退出缓冲期（无参数）
                    log_entry.text = "重装战亡临时激活，进入强制退出缓冲期";// 固定文案
                    break;// 跳出 switch
                }// case 14 结束
                case 15: {// 15 = 装配结果事件
                    // 装配结果事件(0 成功，1 拔出，2 超时，3 离开装配区过久，4 重装战亡，5 四级难度未满足完成协作时限，6 主动退出，7 完成装配但结算时未检测到有能量单元，8 缓冲期到期，装配流程强制结束)
                    const std::string &param = event.param();// 参数即结果码
                    std::string result_name;// 待翻译的结果文案
                    if (param == "0") result_name = "成功";// 0 = 装配成功
                    else if (param == "1")// 1 = 被拔出
                        result_name = "拔出";// 装配臂被拔出
                    else if (param == "2")// 2 = 超时
                        result_name = "超时";// 协作时限内未完成
                    else if (param == "3")// 3 = 离开装配区过久
                        result_name = "离开装配区过久";// 工程离开区域
                    else if (param == "4")// 4 = 重装战亡
                        result_name = "重装战亡";// 重装机器人被击毁
                    else if (param == "5")// 5 = 四级难度未满足协作时限
                        result_name = "四级难度未满足完成协作时限";// 四级特殊规则
                    else if (param == "6")// 6 = 主动退出
                        result_name = "主动退出";// 操作手取消装配
                    else if (param == "7")// 7 = 完成装配但结算时未检测到能量单元
                        result_name = "完成装配但结算时未检测到有能量单元";// 结算校验失败
                    else if (param == "8")// 8 = 缓冲期到期强制结束
                        result_name = "缓冲期到期，装配流程强制结束";// 缓冲期结束
                    else// 未知结果码
                        result_name = "未知结果(" + param + ")";// 兜底：把原始码嵌进文案
                    log_entry.text = "装配结果: " + result_name;// 组合成完整描述
                    break;// 跳出 switch
                }// case 15 结束
                case 16: {// 16 = 重装成功安装模块化 42mm 发射机构
                    // 重装机器人成功安装模块化 42mm 发射机构（无参数）
                    log_entry.text = "重装机器人成功安装模块化42mm发射机构";// 固定文案
                    break;// 跳出 switch
                }// case 16 结束
                default: {// 未识别的事件 ID
                    log_entry.text = "未知事件: ID=" + std::to_string(event.event_id()) + ", 参数=" + event.param();// 兜底：打印 ID 与原始参数，便于后续补全处理
                    break;// 跳出 switch
                }// default 结束
            }// switch 结束
            AppendSystemLog(state, log_entry.type, log_entry.text);// 把拼好的事件描述写入系统日志(同时输出终端)
            PushEventToast(state, event.event_id(), log_entry.text);// 重要事件额外推一条顶部提示(2027 致盲等窗口只有几秒，日志来不及看)
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "RobotInjuryStat") {// 2.2.8 机器人受伤统计(1Hz)：各类扣血明细与击杀者
        RobotInjuryStat robot_injury_stat;// 构造消息对象
        if (robot_injury_stat.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, robot_injury_stat);// 调试日志
            state.robot_injury_stat = robot_injury_stat; // 解析成功，覆盖本地机器人受伤统计快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "RobotRespawnStatus") {// 2.2.9 复活状态(1Hz)：是否待复活、读条进度、金币复活条件
        RobotRespawnStatus robot_respawn_status;// 构造消息对象
        if (robot_respawn_status.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, robot_respawn_status);// 调试日志
            state.robot_respawn_status = robot_respawn_status; // 解析成功，覆盖本地复活状态快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "RobotStaticStatus") {// 2.2.10 机器人静态属性(1Hz)：robot_id/兵种/等级/最大血量热量等
        RobotStaticStatus robot_static_status;// 构造消息对象
        if (robot_static_status.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, robot_static_status);// 调试日志
            state.robot_static_status = robot_static_status; // 解析成功，覆盖本地机器人静态属性快照(含 robot_id 判定敌我)
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "RobotDynamicStatus") {// 2.2.11 机器人实时数据(10Hz)：血量/热量/底盘能量/弹量等
        RobotDynamicStatus robot_dynamic_status;// 构造消息对象
        if (robot_dynamic_status.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, robot_dynamic_status);// 调试日志
            state.robot_dynamic_status = robot_dynamic_status; // 解析成功，覆盖本地机器人实时数据快照(10Hz)
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "RobotModuleStatus") {// 2.2.12 模块运行状态(1Hz)：11 个模块的在线状态
        RobotModuleStatus robot_module_status;// 构造消息对象
        if (robot_module_status.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, robot_module_status);// 调试日志
            state.robot_module_status = robot_module_status; // 解析成功，覆盖本地模块运行状态快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "RobotPosition") {// 2.2.13 机器人坐标与朝向(1Hz)：世界坐标与 yaw 角
        // TODO 这里文档没有把坐标系没说清楚，需要实际测试。猜测也许与雷达站是一样的？
        RobotPosition robot_position;// 构造消息对象
        if (robot_position.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, robot_position);// 调试日志
            state.robot_position = robot_position; // 解析成功，覆盖本地机器人坐标/朝向快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "Buff") {// 2.2.14 Buff 增益(获得时触发，之后 1Hz)：同一时刻可存在多个
        Buff buff;// 构造消息对象
        if (buff.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, buff);// 调试日志
            BuffWithTimestamp buff_entry; // 构造一条带时间戳的 Buff 记录
            buff_entry.buff = buff; // 填入协议下发的 Buff 本体(类型/增益值/剩余时间)
            buff_entry.update_time = std::chrono::steady_clock::now(); // 记录本地接收时刻，供 UpdateState 做过期判定
            auto it = std::ranges::find_if(state.active_buffs, [&buff](const BuffWithTimestamp &existing) {// C++20 ranges 版 find_if：按谓词在列表里找已存在的同款 Buff
                return existing.buff.robot_id() == buff.robot_id() && existing.buff.buff_type() == buff.buff_type();// 同一机器人 + 同一 Buff 类型即视为"同一条"，应原地刷新而不是新增
            });// find_if 结束
            if (it != state.active_buffs.end()) {// 找到了：这是一条已存在 Buff 的续期消息
                it->buff = buff; // 已存在同 robot_id+buff_type 的 Buff，原地更新其内容与时间
                it->update_time = buff_entry.update_time; // 刷新接收时刻(重置过期倒计时)
                SPDLOG_INFO("更新Buff robot_id={} buff_type={} left_time={}",// 三个占位符：机器人 ID / Buff 类型 / 剩余时间
                            buff.robot_id(),// 归属机器人 ID
                            buff.buff_type(),// Buff 类型(1攻击/2防御/3冷却/4功率/5回血/6弹量/7跨越)
                            buff.buff_left_time());// 协议给出的剩余有效时间(秒)
            } else {// 没找到：新 Buff
                state.active_buffs.push_back(buff_entry); // 新 Buff，追加到活动列表供面板显示
                SPDLOG_INFO("新增Buff robot_id={} buff_type={} left_time={}",// 三个占位符：机器人 ID / Buff 类型 / 剩余时间
                            buff.robot_id(),// 归属机器人 ID
                            buff.buff_type(),// Buff 类型
                            buff.buff_left_time());// 剩余有效时间(秒)
            }// if-else 结束
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "PenaltyInfo") {// 2.2.15 判罚信息(触发式)：黄牌/红牌/超功率/超热量/超射速
        PenaltyInfo penalty_info;// 构造消息对象
        if (penalty_info.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, penalty_info);// 调试日志
            LogEntry log_entry; // 构造一条系统(判罚)日志
            time_t now = time(nullptr); // 取当前日历时间(秒级)
            tm *timeinfo = localtime(&now); // 转为本地时间分解结构(tm)
            strftime(log_entry.time, sizeof(log_entry.time), "%H:%M:%S", timeinfo); // 格式化为 "HH:MM:SS" 存入时间字段
            log_entry.type = 1; // 判罚日志标记为 WARNING 级别
            uint32_t penalty_type = penalty_info.penalty_type();// 判罚类型码
            uint32_t penalty_effect_sec = penalty_info.penalty_effect_sec();// 受罚效果持续秒数
            std::string penalty_name;// 判罚类型的中文名
            switch (penalty_type) {// 按类型码翻译成中文
                case 1:// 1 = 黄牌
                    penalty_name = "黄牌";// 单方黄牌
                    break;// 跳出 switch
                case 2:// 2 = 双方黄牌
                    penalty_name = "双方黄牌";// 双方同时被判
                    break;// 跳出 switch
                case 3:// 3 = 红牌
                    penalty_name = "红牌";// 红牌(罚下)
                    break;// 跳出 switch
                case 4:// 4 = 超功率
                    penalty_name = "超功率";// 底盘功率超限
                    break;// 跳出 switch
                case 5:// 5 = 超热量
                    penalty_name = "超热量";// 枪管热量超限
                    break;// 跳出 switch
                case 6:// 6 = 超射速
                    penalty_name = "超射速";// 射频超限
                    break;// 跳出 switch
                default:// 未知类型
                    penalty_name = "未知判罚类型(" + std::to_string(penalty_type) + ")";// 兜底：带上原始码
                    break;// 跳出 switch
            }// switch 结束
            log_entry.text = penalty_name + ", 受罚效果时长" + std::to_string(penalty_effect_sec) + "秒";// 拼出"黄牌, 受罚效果时长10秒"
            if (penalty_effect_sec > 0) {// 有时长的才是真实判罚
                AppendSystemLog(state, log_entry.type, log_entry.text);// 写入系统日志(WARNING 级)
            } else {// 时长为 0：协议里表示"没有实际受罚效果"
                // 这类消息常见于发送端整轮广播默认值，既不影响比赛也没必要占用操作手视线，只留终端调试记录。
                SPDLOG_DEBUG("收到无实际效果的判罚消息，已忽略: {}", log_entry.text);// 终端留痕便于排查
            }// if-else 结束
            state.penalty_info = penalty_info;// 快照最近一次判罚，供顶部判罚横幅显示
            state.penalty_received_time = std::chrono::steady_clock::now();// 记录本地接收时刻，横幅据此自行倒数
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "RobotPathPlanInfo") {// 2.2.16 哨兵轨迹规划(触发式)：意图 + 49 点相对位移
        RobotPathPlanInfo robot_path_plan_info;// 构造消息对象
        if (robot_path_plan_info.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, robot_path_plan_info);// 调试日志
            state.robot_path_plan_info = robot_path_plan_info; // 解析成功，覆盖本地哨兵轨迹规划快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "MapClickInfo") {// 2.2.17 地图点击标记(触发式)：操作手打点后回传的同步信息
        MapClickInfo map_click_info;// 构造消息对象
        if (map_click_info.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, map_click_info);// 调试日志
            state.map_click_info = map_click_info; // 解析成功，覆盖本地地图点击标记快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "RadarInfoToClient") {// 2.2.18 雷达目标信息(触发式)：12 台机器人位置
        RadarInfoToClient radar_info_to_client;// 构造消息对象
        if (radar_info_to_client.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, radar_info_to_client);// 调试日志
            state.radar_info_to_client = radar_info_to_client; // 解析成功，覆盖本地雷达目标信息快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "CustomByteBlock") {// 自定义字节块(触发式)：通常承载本车图传的压缩视频帧
        CustomByteBlock custom_byte_block;// 构造消息对象
        if (custom_byte_block.ParseFromString(payload)) {// 反序列化成功(此时 data 字段内才是真正的帧数据)
            log_parse_success(topic, custom_byte_block);// 调试日志
            const std::string &data = custom_byte_block.data(); // 取自定义字节块载荷(通常为图传编码帧数据)
            if (!data.empty() && custom_byte_block_packet_handler) { // 有数据且已注册解码回调才向下透传
                custom_byte_block_packet_handler(std::span<const uint8_t>(// 用 span 包装一段连续内存(长度+指针)，避免额外的拷贝
                        reinterpret_cast<const uint8_t *>(data.data()),// std::string 底层是 char，需要按字节 reinterpret_cast
                        data.size()));// span 的长度即字符串长度
            }// if 结束
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "TechCoreMotionStateSync") {// 2.2.21 科技核心(能量机关)运动状态(1Hz)
        TechCoreMotionStateSync tech_core_motion_state_sync;// 构造消息对象
        if (tech_core_motion_state_sync.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, tech_core_motion_state_sync);// 调试日志
            state.tech_core_motion_state_sync = tech_core_motion_state_sync; // 解析成功，覆盖本地科技核心运动状态快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "RobotPerformanceSelectionSync") {// 2.2.23 性能体系同步(1Hz)：服务器权威的发射机构/底盘选型
        RobotPerformanceSelectionSync robot_performance_selection_sync;// 构造消息对象
        if (robot_performance_selection_sync.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, robot_performance_selection_sync);// 调试日志
            state.robot_performance_selection_sync = robot_performance_selection_sync;// 覆盖本地性能体系选型快照(开局面板据此回显)
            state.last_received_robot_performance_selection_sync = std::chrono::steady_clock::now(); // 记录收到同步的本地时刻，供开局面板判断"服务器刚推新值"
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "RuneStatusSync") {// 2.2.28 能量机关状态(1Hz)：未激活/激活中/已激活 + 灯臂数 + 平均环数
        RuneStatusSync rune_status_sync;// 构造消息对象
        if (rune_status_sync.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, rune_status_sync);// 调试日志
            state.rune_status_sync = rune_status_sync; // 解析成功，覆盖本地能量机关状态快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "SentryStatusSync") {// 2.2.30 哨兵姿态(1Hz)：进攻/防御/移动 + 弱化/强化标记
        SentryStatusSync sentry_status_sync;// 构造消息对象
        if (sentry_status_sync.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, sentry_status_sync);// 调试日志
            state.sentry_status_sync = sentry_status_sync; // 解析成功，覆盖本地哨兵姿态快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "DartSelectTargetStatusSync") {// 2.2.31 飞镖目标选择(1Hz)：选定目标 + 闸门状态
        DartSelectTargetStatusSync dart_select_target_status_sync;// 构造消息对象
        if (dart_select_target_status_sync.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, dart_select_target_status_sync);// 调试日志
            state.dart_select_target_status_sync = dart_select_target_status_sync; // 解析成功，覆盖本地飞镖目标选择快照
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else if (topic == "SentryCtrlResult") {// 2.2.33 哨兵控制指令结果(触发式)：指令编号 + 执行结果码
        SentryCtrlResult sentry_ctrl_result;// 构造消息对象
        if (sentry_ctrl_result.ParseFromString(payload)) {// 反序列化成功
            log_parse_success(topic, sentry_ctrl_result);// 调试日志
            // 写入全局状态，供开局面板/指挥台的"上一条指令 → 结果"回显使用。
            // 2027 协议只推"指令编号 + 结果码"，不推失败原因，所以这里同时补一条日志：
            // 成功走普通级、失败走警告级，操作手不用盯面板也能看到指令到底有没有生效。
            state.sentry_ctrl_result = sentry_ctrl_result;// 保存最近一次回执
            const std::string result_text = "哨兵指令 " + std::to_string(sentry_ctrl_result.command_id()) +
                                            (sentry_ctrl_result.result_code() == 0
                                                     ? " 执行成功"
                                                     : " 执行失败，结果码 " + std::to_string(sentry_ctrl_result.result_code()));// 组装回执文案
            AppendSystemLog(state, sentry_ctrl_result.result_code() == 0 ? 0 : 1, result_text);// 成功=INFO，失败=WARNING
        } else {// 解析失败
            log_parse_failure(topic);// 记录错误日志
        }// if-else 结束
    } else {// 所有已知话题都不匹配
        SPDLOG_WARN("收到未处理MQTT话题 topic={} bytes={}", topic, payload.size());// WARNING 级：可能是协议新增话题，打出话题名与长度
    }// if-else 链结束
}// HandleMqttMessage 结束

// 功能：把机器人 ID 字符串翻译为「红方/蓝方 + 兵种」中文名。
// 思路：ID<100 为红方（蓝方为 ID-100），按个位/十位映射到兵种常量表；非法输入返回"未知机器人"。
std::string GetRobotName(const std::string &robot_id) {// 形参为十进制 ID 文本，如 "1"/"101"/"11"
    if (robot_id.empty()) {// 空串无法解析
        SPDLOG_WARN("收到空机器人ID");// 打一条警告，便于追查协议异常
        return "未知机器人";// 返回兜底名称
    }// if 结束

    int id = 0;// 解析结果(红方 1~12，蓝方 101~112)
    try {// std::stoi 遇到非数字会抛异常，这里做保护
        id = std::stoi(robot_id);// 把字符串转成 int(十进制)
    } catch (const std::exception &error) {// 捕获 std::invalid_argument / std::out_of_range
        SPDLOG_WARN("机器人ID解析失败 robot_id={} message={}", robot_id, error.what());// 打印原始文本与异常原因
        return "未知机器人";// 返回兜底名称
    }// try-catch 结束
    bool is_red = id < 100;// 约定：ID<100 为红方，100+ 为蓝方
    int num = is_red ? id : id - 100;// 换算成 1~12 的兵种编号

    std::string prefix = is_red ? "红方" : "蓝方";// 阵营前缀
    std::string suffix;// 兵种后缀

    switch (num) {// 按兵种编号映射中文名
        case 1:// 1 = 重装
            suffix = "重装";// 重装机器人
            break;// 跳出 switch
        case 2:// 2 = 保留位
            suffix = "保留位";// 2027 起 2 号为保留位
            break;// 跳出 switch
        case 3:// 3 = 步兵(3 号)
            suffix = "步兵3";// 三号步兵
            break;// 跳出 switch
        case 4:// 4 = 步兵(4 号)
            suffix = "步兵4";// 四号步兵
            break;// 跳出 switch
        case 5:// 5 = 步兵(5 号)
            suffix = "步兵5";// 五号步兵
            break;// 跳出 switch
        case 6:// 6 = 空中系统
            suffix = "空中系统";// 2027 称空中系统
            break;// 跳出 switch
        case 7:// 7 = 哨兵
            suffix = "哨兵";// 哨兵机器人
            break;// 跳出 switch
        case 8:// 8 = 飞镖
            suffix = "飞镖";// 飞镖发射站
            break;// 跳出 switch
        case 9:// 9 = 雷达
            suffix = "雷达";// 雷达站
            break;// 跳出 switch
        case 11:// 11 = 前哨站
            suffix = "前哨站";// 前哨站(建筑)
            break;// 跳出 switch
        case 12:// 12 = 基地
            suffix = "基地";// 基地(建筑)
            break;// 跳出 switch
        default:// 未识别的编号
            suffix = std::to_string(num) + "号";// 兜底：显示"N号"
            break;// 跳出 switch
    }// switch 结束

    return prefix + suffix;// 拼接成"红方英雄"这样的完整名称
}// GetRobotName 结束

