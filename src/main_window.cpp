// main_window.cpp：主窗口实现——MQTT 下行消息分发与状态刷新、各面板布局调度、自定义控制包下发，以及本车/系统日志的维护。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_stdlib.h"
#include "imgui_internal.h"
#include <messages.pb.h>
// [/pch-split]
#include "main_window.h"  // 本模块头文件：引入 State 定义与 ShowMainWindow/HandleMqttMessage 等函数声明
#include "mqtt/mqtt_router.h"  // MQTT 下行分发：按 topic 查表调用 mqtt/handlers_*.cpp 中的处理函数
#include "keyboard_mouse_state_manager.h"  // 键鼠状态管理器：准星面板据此读取操作手的鼠标位移/按键状态
#include "logging/logging.h"  // 日志封装：提供 SPDLOG_INFO/WARN/ERROR/DEBUG 等宏(spdlog 的薄封装)
#include "panel/bottom_panel.h"  // 底部信息条面板：显示操作提示/状态摘要
#include "panel/buff_panel.h"  // 增益面板：显示当前生效的 Buff 列表
#include "panel/commander_panel.h"  // 指挥手综合指挥台：全屏地图 + 双方信息 + 哨兵/飞镖/空中指令
#include "panel/crosshair.h"  // 准星面板：比赛中在全屏覆盖层上绘制自瞄准星
#include "panel/header_panel.h"  // 顶部状态栏：比分/阶段/倒计时
#include "panel/log_panel.h"  // 日志面板：滚动显示本车日志与系统日志
#include "panel/mini_map.h"  // 小地图面板：绘制场地、自身与友军/敌方位置
#include "panel/perf_panel.h"  // 性能浮层：解码/转换/上传/渲染耗时（F12 呼出）
#include "panel/robot_status_panel.h"  // 本车状态面板：血条/热量/弹量等
#include "panel/start_panel.h"  // 开局面板：赛前选型、连接与开始比赛入口

// 无名命名空间：本文件内部使用的 UI 辅助函数集合（字符串解析、日志缓存、视频缩放、标尺绘制），不对外暴露。
namespace {  // 匿名命名空间开始：内部符号仅在当前翻译单元可见，避免与其他文件的同名函数冲突

    // 系统日志去重窗口：同一条文本在该时间窗内再次到达，视为发送端重复投递，折叠成 ×N 而不新增一行。
    // 取值依据：实测发送端会在同一毫秒内连发 4 份同一条判罚，且断流补发时整轮重复，2 秒足够覆盖这类突发。
    constexpr auto kSystemLogDedupeWindow = std::chrono::seconds(2);

    // 功能：把形如 "a,b" 的字符串按首个逗号拆成两段的键值对。
    // 思路：用 string::find 定位首个逗号，分别取左右子串；找不到逗号则返回空。
    // 说明：Event 事件参数的拆分逻辑随文案翻译一起搬到 mqtt/event_formatter.cpp。

    // 功能：构造一条带本地时间戳与自增序号的日志并追加到指定缓冲区。
    // 思路：取当前时间格式化为 "HH:MM:SS"，分配自增 sequence，超 50 条则丢弃最旧一条以限长。
    void AppendLogBuffer(State &state, std::vector<LogEntry> &buffer, int type, const std::string &text) {  // buffer 由调用方指定(本车日志或系统日志)
        LogEntry log_entry;  // 在栈上先构造一条待填充的日志记录
        time_t now = time(nullptr);  // 取当前 UTC 时间戳(自 1970 起的秒数)
        tm *timeinfo = localtime(&now);  // 转成本地时间的分解结构 tm(注意返回静态缓冲，仅本函数内借用)
        strftime(log_entry.time, sizeof(log_entry.time), "%H:%M:%S", timeinfo);  // 格式化为 8 字符 "HH:MM:SS"，长度刚好与 char time[9] 匹配(含结尾 '\0')
        log_entry.type = type;  // 记录日志级别(0 INFO / 1 WARNING / 2 ERROR)，供面板着色
        log_entry.text = text;  // 拷贝日志正文
        log_entry.sequence = state.next_log_sequence++;  // 分配全局自增序号(后置 ++ 先取值再加一)，两个日志列表共用同一计数器
        buffer.push_back(log_entry);  // 追加到目标缓冲区末尾
        if (buffer.size() > 50) {  // 缓冲区超过 50 条即限长，避免内存无限增长(UI 也只展示最近若干条)
            buffer.erase(buffer.begin());  // 丢弃最旧的一条(队首)，整体前移
        }
    }

    // 功能：按日志类型把文案输出到终端（spdlog），便于现场调试。
    // 思路：根据 type(0/1/2) 分别走 INFO/WARN/ERROR 级别，其余按 INFO 处理。
    void EmitUiLog(int type, const char *scope, const std::string &text) {  // scope 为前缀常量("本车日志: "/"系统日志: ")
        switch (type) {  // 按日志级别选择 spdlog 的输出宏
            case 0:  // 0 = INFO
                SPDLOG_INFO("{}{}", scope, text);  // 用两个 {} 占位符依次拼接前缀与正文，避免额外的字符串拼接开销
                break;  // 跳出 switch
            case 1:  // 1 = WARNING
                SPDLOG_WARN("{}{}", scope, text);  // 警告级日志(通常是黄色高亮)
                break;  // 跳出 switch
            case 2:  // 2 = ERROR
                SPDLOG_ERROR("{}{}", scope, text);  // 错误级日志(通常是红色高亮)
                break;  // 跳出 switch
            default:  // 未知级别，退化按 INFO 输出
                SPDLOG_INFO("{}{}", scope, text);  // 兜底：保证日志不丢
                break;  // 跳出 switch
        }
    }

    // 【2027 变更】原 FitImageSize（自定义视频等比居中显示）已随"双路主画面切换"一起删除：
    // 背景层现在固定铺 UDP 图传，直接拉伸铺满视口；次画面面板有自己的等比缩放实现。

}  // namespace

// 功能：追加本车日志（操作手主动动作/状态）。
// 思路：写入车辆日志缓冲区并打印终端日志。
void AppendVehicleLog(State &state, int type, const std::string &text) {  // 对外接口：type 为级别，text 为日志正文
    AppendLogBuffer(state, state.vehicle_logs, type, text);  // 写入本车日志缓冲区(带时间戳/自增序号，自动限长 50 条)
    EmitUiLog(type, "本车日志: ", text);  // 同步输出到终端日志(spdlog)，便于离屏排查
}

// 功能：追加系统日志（裁判系统事件/判罚）。
// 思路：写入系统日志缓冲区并打印终端日志。
void AppendSystemLog(State &state, int type, const std::string &text) {  // 对外接口：用于裁判系统事件、判罚、解析异常等
    // 去重：发送端在断流补发/整轮广播时会把同一条消息连发多份（实测同一毫秒 4 条判罚），
    // 若每条都记一行，日志面板会被无意义的重复文本刷满。这里在窗口期内把相同文本折叠成 ×N。
    const auto now = std::chrono::steady_clock::now();  // 当前单调时钟时刻（不受系统改时间影响）
    if (!state.system_logs.empty()) {  // 缓冲区非空才可能与上一条比较
        LogEntry &last_entry = state.system_logs.back();  // 只看最后一条：日志是追加写入的，重复必然相邻
        if (last_entry.text == text && now - state.last_system_log_time <= kSystemLogDedupeWindow) {  // 文本相同且在去重窗口内
            ++last_entry.repeat_count;  // 只累加次数，不新增行
            state.last_system_log_time = now;  // 顺延窗口，连续重复时始终折叠在同一条上
            return;  // 直接返回：既不写新行，也不打终端日志，避免终端同样刷屏
        }
    }
    AppendLogBuffer(state, state.system_logs, type, text);  // 写入系统日志缓冲区
    state.last_system_log_time = now;  // 记录本条写入时刻，作为下一次去重的基准
    EmitUiLog(type, "系统日志: ", text);  // 同步输出到终端日志(spdlog)，便于离屏排查
}

// 功能：构造初始状态。
// 思路：清零红蓝双方各 9 个机器人的血量/弹量后返回。
State CreateInitialState() {  // 返回一个字段均已初始化的 State 值
    State state;  // 默认构造：optional 字段为空，vector 为空，计数器为初值
    for (size_t i = 0; i < std::size(state.red_robots); i++) {  // 遍历 9 个机器人槽位(std::size 编译期取数组元素个数)
        RobotStatus robot;  // 构造一台机器人的状态快照
        robot.health = 0;  // 血量清零(尚未收到裁判系统数据)
        robot.bullets = 0;  // 允许发弹量清零
        state.red_robots[i] = robot;  // 写入红方第 i 个槽位
        state.blue_robots[i] = robot;  // 蓝方同索引写入相同初值
    }
    return state;  // 按值返回(依赖 RVO/移动语义，开销可忽略)
}

// 功能：主界面总绘制入口。组织视频背景、顶部栏、状态栏、各功能面板与开局面板的绘制顺序，处理视频主画面切换。
// 思路：先绘制全屏视频背景（按主/次视频源选择并居中缩放），再按布局矩形逐个 SetNextWindow 摆放各 ImGui 面板；在比赛中绘制准星，否则绘制开局面板。返回是否请求切换 UDP 视频翻转。
bool ShowMainWindow(const State &state,  // 全局状态快照(各面板只读渲染)
                    RoboGo::io::MqttClient &mqtt_client,  // MQTT 客户端(开局面板用于下发选型/开始指令)
                    ImTextureRef video_texture_ref,  // UDP 视频纹理引用
                    const ImVec2 &video_size,  // UDP 视频原始像素尺寸
                    const bool udp_video_display_ready,  // UDP 视频是否有可显示帧
                    const bool udp_video_inverted,  // UDP 视频当前是否上下翻转
                    const ImTextureRef mini_map_texture_ref,  // 小地图底图纹理引用
                    const ImVec2 &mini_map_texture_size,  // 小地图底图原始像素尺寸
                    ImTextureRef mini_map_terrain_texture_ref,  // 地形分层叠加层句柄
                    const ImVec2 &mini_map_terrain_texture_size) {  // 地形分层叠加层尺寸
    const ImGuiViewport *viewport = ImGui::GetMainViewport(); // 获取 ImGui 主视口(覆盖整个窗口的工作区矩形，用于全屏布局)
    const float ui_scale = ImGui::GetScale(); // DPI 缩放：定稿以 1280×800 为基准，所有像素尺寸都乘它

    // 【2027 变更】英雄部署模式已删除，不再有第二路可选主画面：主画面固定为 UDP 图传。
    const ImTextureRef background_video_texture_ref = video_texture_ref;      // 主画面纹理：UDP
    const ImVec2 background_video_size = video_size;                          // 主画面尺寸：UDP
    const bool background_video_display_ready = udp_video_display_ready;      // 主画面就绪标志：UDP

    // 指挥手：登录成功就直接铺开指挥台，不再停留在赛前那一页（那一页对指挥手没有任何可用指令）。
    maybe_auto_enter_commander_console(mqtt_client);

    // 兜底不变量：指挥手只要还连着，就不存在"指挥台已关闭"这个合法状态。
    // 之前「返回」只关界面、保持登录，于是又露出赛前指令页，且自动进入只认连接上升沿、不会把人拉回来。
    // 这里保证任何路径（按钮 / 状态异常 / 断线恢复）关掉指挥台但没登出时，立刻回到指挥台。
    // 指挥手的「返回」走的是登出（清角色 + 清 client_id），登出后本条件不成立，能正常回到身份选择页。
    if (is_commander_role() && !is_commander_console_active() &&
        mqtt_client.get_connection_state() == RoboGo::io::MqttClient::ConnectionState::Connected) {
        enter_commander_console();
    }

    // 指挥手：整页就是综合指挥台，与普通 HUD 互斥。
    // 这里直接分流并返回，避免两套布局在同一帧里互相压盖（指挥台自己会画顶栏、地图与指令区）。
    if (is_commander_console_active()) {  // 指挥台已打开
        ShowCommanderPanel(state, mqtt_client,  // 状态 + MQTT（指挥台要下发哨兵/飞镖/地图标记）
                           background_video_texture_ref, background_video_size, background_video_display_ready,  // 现场画面预览
                           mini_map_texture_ref, mini_map_texture_size,  // 战术地图底图
                           mini_map_terrain_texture_ref, mini_map_terrain_texture_size);  // 地形分层叠加层（M 开关）
        return false;  // 指挥台不需要翻转 UDP 视频，直接返回
    }
    // 视频就绪则把整张 UDP 图传贴到背景绘制层；2027 只剩一路，直接拉伸铺满，不再做等比适配。
    if (background_video_display_ready && background_video_texture_ref.GetTexID() != ImTextureID_Invalid) {  // 就绪且纹理句柄有效(无效句柄贴图会画不出东西)
        ImGui::GetBackgroundDrawList()->AddImage(background_video_texture_ref, ImVec2(0, 0), viewport->Size, ImVec2(0, 0), ImVec2(1, 1)); // 铺满背景层
    }  // if 结束(未就绪则本帧不绘制视频底图)
    // 【2027 变更】英雄部署模式已在 2027 协议中删除，原叠加在视频上的辅助标尺一并移除。

    // 顶部 HUD：定稿改为全视口自绘覆盖层（居中状态胶囊 + 左右各 300px 的队伍条）。
    // 队伍条从 y=18 起、延伸到约 219px，窗口仍覆盖全屏，避免队伍条或被后续浮层裁剪。
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ShowHeaderPanel(state);

    // 定稿的纵向分区：队伍条上移到 y=18，底部约 219px；日志 / Buff 面板紧贴其下，
    // 把左右上角与队伍条下方的空白全部收掉。本车卡与控制度带仍贴底。
    const float hud_top_area = 230.0f * ui_scale;
    const float side_margin = 24.0f * ui_scale;
    const float self_card_width = 322.0f * ui_scale;
    const float self_card_height = 168.0f * ui_scale;
    const float self_card_top = viewport->Size.y - self_card_height - side_margin;

    // 左下角本车卡（固定尺寸，与定稿一致；不再按字体行数估算）。
    ImGui::SetNextWindowPos(ImVec2(side_margin, self_card_top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(self_card_width, self_card_height), ImGuiCond_Always);
    ShowRobotStatusPanel(state);

    // 左侧事件流：从队伍条下方开始，到本车卡上方结束，宽度与队伍条一致。
    const float left_panel_top = hud_top_area;
    const float left_panel_height = std::max(0.0f, self_card_top - left_panel_top - 8.0f * ui_scale);
    ImGui::SetNextWindowPos(ImVec2(side_margin, left_panel_top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300.0f * ui_scale, left_panel_height), ImGuiCond_Always);
    ShowLogPanel(state);

    // 本帧是否请求翻转 UDP 画面：按钮在登录控制面板里，比赛中不再有（键盘 I 仍可用）。
    bool toggle_udp_video_inversion_requested = false;

    // 比赛中绘制准星覆盖全屏；否则绘制开局准备面板（居中 750x500 窗口）。
    if (is_in_game_mode()) {  // 全局函数：当前是否已进入比赛(由上层状态机维护)
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always); // 准星覆盖层定位到全屏左上角
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y)); // 准星层尺寸=全屏
        ShowCrosshair(state); // 绘制全屏准星(比赛中显示)
        // 重装专用装配浮层：F1 呼出，纯键盘操作。放在准星之后绘制，保证压在画面之上；
        // 又放在判罚横幅/Toast 之前，保证那两类提示仍然最顶层。
        ShowAssemblyOverlay(state, mqtt_client); // 非重装/非比赛模式时内部直接返回，无副作用
    } else {  // 未进入比赛：显示开局面板
        float width = 750 * ImGui::GetScale(); // 开局面板基准宽度=750 × 全局 UI 缩放
        // 高度从 500 加到 560：2027 里重装/哨兵的功能面板比 2026 多（装配+核心状态、哨兵 12 条指令），
        // 500 高装不下，只能靠滚动；加高 60px 后「登录区 + 全部功能面板 + 进入比赛」基本能一屏放完。
        float height = 560 * ImGui::GetScale(); // 开局面板基准高度=560 × 全局 UI 缩放
        ImGui::SetNextWindowPos(ImVec2((viewport->Size.x - width) / 2, (viewport->Size.y - height) / 2), ImGuiCond_Always); // 开局面板居中放置
        ImGui::SetNextWindowSize(ImVec2(width, height)); // 开局面板尺寸=750×560(按缩放)
        toggle_udp_video_inversion_requested = StartPanel(state, mqtt_client, udp_video_inverted); // 绘制开局准备面板，并取回是否点了 UDP 反转
    }

    // 右下：小地图（按 28:15 标准场地比例计算高度）。
    // 右侧整栏宽度 20% → 24%：20% 时地图只有 ~380px 宽，地形缩得看不清，标点也显得很大。
    const float right_column_ratio = 0.24f; // 右侧栏（小地图 + Buff 面板）占全宽比例
    float mini_map_width = viewport->Size.x * right_column_ratio; // 小地图宽度=全宽 24%
    float mini_map_height = mini_map_width / 28 * 15; // 小地图高度按 28:15 标准场地比例换算
    ImGui::SetNextWindowPos(ImVec2(viewport->Size.x - mini_map_width, viewport->Size.y - mini_map_height), ImGuiCond_Always); // 小地图定位到右下角
    ImGui::SetNextWindowSize(ImVec2(mini_map_width, mini_map_height)); // 小地图尺寸
    ShowMiniMap(state, mqtt_client, // 状态与 MQTT（标记模式要下发坐标）
                mini_map_texture_ref, mini_map_texture_size, // 底图
                mini_map_terrain_texture_ref, mini_map_terrain_texture_size); // 地形分层叠加层（M 键开关）

    // 右侧中部：增益/Buff 面板。从队伍条下沿（230px）开始，避免互相压盖。
    float buff_panel_width = viewport->Size.x * right_column_ratio; // Buff 面板宽度与小地图同宽，保证右边缘对齐
    float buff_panel_height = std::max(0.0f, viewport->Size.y - hud_top_area - mini_map_height);
    ImGui::SetNextWindowPos(ImVec2(viewport->Size.x - buff_panel_width, hud_top_area), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(buff_panel_width, buff_panel_height));
    ShowBuffPanel(state);

    // 底部信息条。
    ShowBottomPanel(state); // 底部条不预设位置/尺寸，由面板自身定位

    // 三个覆盖层放在最后创建：ImGui 按窗口创建顺序叠放，放早了会被开局面板/小地图盖住
    // （实测判罚横幅正好落在开局面板范围内，看不见）。
    ShowPenaltyBanner(state); // 生效判罚的顶部横幅(黄牌/红牌/超限)
    ShowEventToasts(state); // 重要事件的顶部浮层提示
    RoboGo::panel::ShowPerformancePanel(); // 性能浮层(默认隐藏，F12 呼出；内部自行判断可见性)

    // 把"是否请求翻转 UDP 视频"沿调用链返回给上层。
    return toggle_udp_video_inversion_requested;  // 返回值=true 表示用户本帧点击了翻转开关
}

// 功能：每帧推进本地状态。目前负责清理已超出剩余时间的 Buff。
// 思路：遍历 active_buffs，用当前时间减去更新时间得到已持续秒数，若 >= buff 剩余时间则移除；并统计清理条数用于调试日志。
void UpdateState(State &state) {  // 由主循环每帧调用一次
    // 取单调时钟当前时刻(不受系统时间调整影响，适合做间隔/超时判断)
    auto now = std::chrono::steady_clock::now();

    const std::size_t previous_size = state.active_buffs.size();  // 记录清理前的条数，用于后面算出"清掉了几条"
    // C++20 的"按谓词删除"算法：遍历容器，谓词为 true 的元素被移除
    std::erase_if(state.active_buffs,
            // 判据 lambda：捕获 now(值捕获)，入参是一条 Buff 记录
            [now](const BuffWithTimestamp &buff_entry) {
                // 把时间差强制转换成整秒，已持续时间 = 当前时刻 - 该 Buff 的接收时刻
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                        now - buff_entry.update_time)  //
                                        .count();  // 取出秒数的数值部分(long long)
                return elapsed >= static_cast<int>(buff_entry.buff.buff_left_time());  // 已持续时间 >= 协议给的剩余时间 → 判定过期，返回 true 触发删除
            });

    // 前后条数相减即本次清理掉的条数
    const std::size_t removed = previous_size - state.active_buffs.size();
    // 仅在有清理时打日志，避免每帧刷屏
    if (removed > 0) {
        SPDLOG_DEBUG("清理过期Buff removed={} remaining={}", removed, state.active_buffs.size());  // DEBUG 级：清掉几条、还剩几条
    }
}

// 功能：处理从 MQTT 收到的裁判系统/模拟器消息。
// 思路：转交 mqtt/mqtt_router 的静态分发表；每个 topic 的处理函数分布在 mqtt/handlers_*.cpp 中，
//       新增协议消息时只需在分发表加一行、在 handlers.h 加一个声明。
void HandleMqttMessage(State &state,  // 待刷新的全局状态
                       RoboGo::io::MqttClient &mqtt_client,  // 保留参数以统一签名（下行分发当前不使用）
                       const std::string &topic,  // 话题名 = 裁判系统消息类型
                       const std::string &payload)  // 消息二进制载荷
{
    (void) mqtt_client;  // 下行分发未使用该参数，显式转换以抑制未使用警告
    RoboGo::mqtt::DispatchMqttMessage(state, topic, payload);
}
