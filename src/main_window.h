// 主窗口与全局状态定义：本文件声明主界面入口 ShowMainWindow 及共享的比赛状态 State。
#pragma once

#include <pch.h>  // 预编译头：集中引入 STL/protobuf/ImGui 等第三方头，加速整体编译
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
#include "imgui_stdlib.h"
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]
#include "custom_control_payload.h"  // 自定义控制包的字节布局与打包函数（手写实现，不由 protobuf 生成）
#include "io/mqtt_client.h"  // MQTT 客户端封装：提供 publish/subscribe 等接口，用于与裁判系统通信

// 一条日志记录的存储结构（用于本车日志与系统日志）。
struct LogEntry {
    char time[9];  // 时间戳字符串，格式 "HH:MM:SS"。
    std::string text;  // 日志正文。
    int type;  // 0: INFO, 1: WARNING, 2: ERROR
    uint64_t sequence = 0;  // 单调递增序号，用于排序与去重。
    uint32_t repeat_count = 1;  // 连续重复次数：同一条文本在去重窗口内再次到达时累加，面板显示为 "×N"。
};

// 单个机器人的血量/弹量状态（UI 展示用）。
struct RobotStatus {
    uint32_t health = 0;  // 当前血量。
    uint32_t max_health = 0;  // 历史最大血量（用于计算血条比例，仅在刷新时向上更新）。
    int32_t bullets = 0;  // 允许发弹量（剩余弹药兑换额度）。
};

// 地图点位坐标（ID + 归一化/像素坐标）。
struct MapPosition {
    int id;   // 点位编号，用于标识场地上预定义的位置（如出生点、资源点）
    float x;  // 归一化/像素横坐标
    float y;  // 归一化/像素纵坐标
};

// 带更新时间戳的 Buff，用于计算剩余有效时间并自动过期清理。
struct BuffWithTimestamp {
    Buff buff;  // 协议下发的增益本体（类型、增益值、剩余时间等）
    std::chrono::steady_clock::time_point update_time;  // 收到该 Buff 的本地时刻。
};

// 一条顶部事件提示（Event 消息的可视化摘要）。2027 的致盲/护甲展开等窗口只有几秒，光靠日志来不及看。
struct EventToast {
    std::string text;   // 提示正文（已翻译成中文，如"飞镖命中: 对方命中基地"）
    std::chrono::steady_clock::time_point time;   // 产生该提示的本地时刻，用于计算淡出
    int level = 0;   // 紧要程度：0 普通(白) / 1 重要(橙) / 2 危急(红)
};

// 全局比赛状态：聚合所有裁判系统/模拟器 MQTT 话题解析后的最新快照，供各面板读取显示。
struct State {
    // 【2.2.3 比赛全局状态 · 1Hz】当前局号/总局数、红蓝双方比分、比赛阶段(0未开始/1准备/2自检/3五秒倒计时/4比赛中/5结算中)、
    // 阶段剩余与已过秒数、是否暂停、当局胜负(0平/1红胜/2蓝胜，非结算为255)、结束原因。
    // 消费方：header_panel 显示比分与倒计时；开局面板据此判断当前处于哪个阶段。
    std::optional<GameStatus> game_status;

    // 【2.2.4 全局单位状态 · 1Hz】己方/对方基地与前哨站的血量、状态(无敌/护甲展开/被击毁/重建中)、阵地控制状态，
    // 全部8台机器人的血量数组(2027 起按己方1/3/4/7号、对方1/3/4/7号顺序，每方4台；2号为保留位)，己方剩余发弹量，双方累计总伤害与控制度。
    // 消费方：robot_status_panel 血条；PublishCustomControlIfDue 从这里取敌方血量下发机器人电控。
    std::optional<GlobalUnitStatus> global_unit_status;

    // 【2.2.5 全局后勤信息 · 1Hz】己方当前剩余经济、累计获得总经济、科技等级(重装曾装配的最高难度)、
    // 加密等级(雷达解析信息波机制)。
    std::optional<GlobalLogisticsStatus> global_logistics_status;

    // 【2.2.6 全局特殊机制 · 1Hz】当前正在生效的特殊机制 ID 列表及各自时间参数(秒)。
    // 1=己方堡垒被对方占领计时，2=对方堡垒被己方占领计时。
    std::optional<GlobalSpecialMechanism> global_special_mechanism;

    // 【2.2.8 机器人受伤统计 · 1Hz】本次存活期间累计受伤明细：总计、撞击、17mm弹丸、42mm弹丸、
    // 飞镖溅射、模块离线扣血、异常离线扣血、判罚扣血、服务器强制战亡扣血，以及击杀者ID(0表示未检测到)。
    // 注意：机器人一旦战亡复活即重新统计。
    std::optional<RobotInjuryStat> robot_injury_stat;

    // 【2.2.9 复活状态 · 1Hz】是否待复活、复活所需总读条与当前进度、可否免费复活、
    // 花费金币复活所需金币数、是否允许花金币复活。
    // 消费方：robot_status_panel 复活倒计时与一键复活按钮。
    std::optional<RobotRespawnStatus> robot_respawn_status;

    // 【2.2.10 机器人静态属性 · 1Hz】连接/上场/存活状态、robot_id、robot_type、等级、最大血量与热量、
    // 热量冷却速率、最大功率、最大缓冲能量与底盘能量，以及发射机构/底盘性能体系选型。
    // 关键：这里的 robot_id 决定本机是红方还是蓝方，是判断敌我的依据。
    std::optional<RobotStaticStatus> robot_static_status;

    // 【2.2.11 机器人实时数据 · 10Hz】当前血量、热量、上次弹丸射速、缓冲能量（2027 起底盘能量字段已改为保留位）、
    // 当前经验与升级所需经验、累计已发弹量、剩余允许发弹量、是否脱战及脱战倒计时、可否远程补血/补弹。
    // 频率最高(10Hz)，是 HUD 上血条、热量、弹量等实时数值的直接来源。
    std::optional<RobotDynamicStatus> robot_dynamic_status;

    // 【2.2.12 模块运行状态 · 1Hz】电源管理、RFID、灯条、17mm/42mm测速、UWB定位、装甲、
    // 图传、电容、主控、模块化42mm安装权限共11个模块的在线状态(0离线/1在线/2安装不规范视为离线)。
    std::optional<RobotModuleStatus> robot_module_status;

    // 【2.2.13 机器人坐标与朝向 · 1Hz】世界坐标 X/Y/Z 与测速模块朝向 yaw(度，正北为0)，
    // 以及坐标归属的 robot_id(6/106为空中机器人，7/107为哨兵，仅云台手客户端生效)。
    // 消费方：mini_map 绘制自身与友军位置。
    std::optional<RobotPosition> robot_position;

    // 【2.2.14 Buff 增益 · 获得时触发，此后1Hz直到失去】当前生效的全部增益列表，每项附带本地接收时刻。
    // Buff类型：1攻击/2防御或易伤/3射击热量冷却/4底盘功率/5回血/6可兑换发弹量/7地形跨越(预备)。
    // 用 vector 而非 optional：同一时刻可有多个增益并存；UpdateState 按 buff_left_time 过期剔除。
    std::vector<BuffWithTimestamp> active_buffs;

    // 【2.2.15 判罚信息 · 触发式】最近一次判罚的类型(1黄牌/2双方黄牌/3红牌/4超功率/5超热量/6超射速)
    // 与受罚效果持续秒数。协议只推"罚了什么、罚多久"，不推剩余时间，因此需要配合下面的本地接收时刻自己倒数。
    // 消费方：header_panel 的判罚横幅(顶部常驻，倒计时到 0 自动消失)。
    std::optional<PenaltyInfo> penalty_info;

    // 收到上面那条 PenaltyInfo 的本地时刻(steady_clock)。用于把 penalty_effect_sec 换算成本地倒计时，
    // 避免依赖服务器再推一条"判罚结束"消息(协议里没有这种消息)。
    std::chrono::steady_clock::time_point penalty_received_time;

    // 顶部事件提示队列（由 Event 消息的重要事件填充，渲染后自动按时间淡出）。
    // 消费方：header_panel 的 ShowEventToasts，最多同时显示若干条，超出丢弃最旧的。
    std::vector<EventToast> event_toasts;

    // 【2.2.16 哨兵轨迹规划 · 触发式】哨兵意图(1攻击/2防守/3移动)、起始点坐标(分米)，
    // 以及长度49的相对位移增量数组 offset_x/offset_y(-128~+127)，用于还原完整巡逻路径。
    // 消费方：mini_map 绘制哨兵规划轨迹。
    std::optional<RobotPathPlanInfo> robot_path_plan_info;

    // 【2.2.17 地图点击标记 · 触发式】发送范围(0指定/1除哨兵/2含哨兵)、目标机器人ID列表(固定7字节)、
    // 标记类型(1攻击/2防御/3警戒/4自定义)、标定的敌方ID、自定义图标ASCII码、标记模式(1地图/2敌方机器人)、
    // 地图坐标 X/Y。即操作手在小地图上打点后，裁判系统回传的同步信息。
    std::optional<MapClickInfo> map_click_info;

    // 【2.2.18 雷达目标信息 · 触发式】雷达站下发的12台机器人位置(对方1/2/3/4/6/7号 + 己方1/2/3/4/6/7号)，
    // 每项含坐标(cm)与是否特殊标识(0否/1是/2是但目标定位模块离线)。
    // 消费方：mini_map 显示雷达解析出的敌方位置。
    std::optional<RadarInfoToClient> radar_info_to_client;

    // 【2.2.21 科技核心(能量机关)运动状态 · 1Hz】当前可选最高装配难度、核心基础状态(1初始位/2运动中/3已到位)、
    // 能量单元的放入/平移/旋转状态、对方科技核心装配情况(仅己方四级装配可用时有效)、
    // 己方四级装配的总剩余时长与单步间隔剩余时长。
    std::optional<TechCoreMotionStateSync> tech_core_motion_state_sync;

    // 【2.2.23 性能体系同步 · 1Hz】发射机构性能体系、底盘性能体系、哨兵控制方式选择的当前生效值。
    // 这是服务器权威值；开局面板上的选型控件在收到它后需要回显同步。
    std::optional<RobotPerformanceSelectionSync> robot_performance_selection_sync;

    // 收到上一帧 robot_performance_selection_sync 的本地时刻(steady_clock，非 optional，默认即纪元起点)。
    // 用途：开局面板用它判断「服务器刚推来新同步」，避免在用户手动改选型的短暂窗口内被服务器旧值回弹覆盖
    // （配合 performance_panel_state.ignore_remote_sync_until 使用）。
    std::chrono::steady_clock::time_point last_received_robot_performance_selection_sync;

    // 【2.2.28 能量机关状态 · 1Hz】当前状态(1未激活/2正在激活/3已激活)、已激活灯臂数量、
    // 平均环数(仅大能量机关正在激活时有效，其余为0)。
    std::optional<RuneStatusSync> rune_status_sync;

    // 【2.2.30 哨兵姿态 · 1Hz】姿态ID(1进攻/2防御/3移动)、是否弱化、是否强化。
    std::optional<SentryStatusSync> sentry_status_sync;

    // 【2.2.31 飞镖目标选择 · 1Hz】当前选定的飞镖目标ID、闸门状态(0关闭/1开启中/2已开启)。
    std::optional<DartSelectTargetStatusSync> dart_select_target_status_sync;

    // 【2.2.33 哨兵控制指令结果 · 触发式】对应指令编号与执行结果码(0成功，其他失败)。
    // 即客户端发出哨兵控制指令后，服务器回执的成功/失败反馈。
    std::optional<SentryCtrlResult> sentry_ctrl_result;

    // 上一次下发 CustomControl 自定义控制包的本地时刻；为空表示从未发送过。
    // 用途：PublishCustomControlIfDue 据此做节流，避免每个渲染帧都高频下发。
    std::optional<std::chrono::steady_clock::time_point> last_custom_control_publish_time;

    // 自定义状态（本地维护，非裁判系统下发）
    bool auto_aim = false;       // 自瞄开关：由操作手在界面切换，随自定义控制包下发机器人电控
    bool gyro = false;           // 陀螺仪（小陀螺）开关：同上，指示底盘是否进入陀螺模式

    // 机器人状态：重装，保留，步兵x3，空中系统，哨兵，飞镖，雷达
    RobotStatus red_robots[9];   // 红方 9 台机器人的血量/弹量快照，按上述兵种顺序索引
    RobotStatus blue_robots[9];  // 蓝方 9 台机器人，索引顺序同红方

    // 日志
    uint64_t next_log_sequence = 1;        // 下一条日志将使用的全局自增序号（两个日志列表共用，保证可统一排序）
    std::vector<LogEntry> vehicle_logs;    // 本车日志：操作手视角的事件记录
    std::vector<LogEntry> system_logs;     // 系统日志：裁判系统事件、判罚、解析异常等
    // 上一条系统日志的写入时刻：用于把"发送端短时间重复投递的同一条文本"折叠成 ×N，避免刷屏。
    std::chrono::steady_clock::time_point last_system_log_time{};
};

// UDP 视频流判定为不可用的超时阈值（超过该时间未刷新则回退到备选视频源）。
inline constexpr auto kUdpVideoFallbackTimeout = std::chrono::milliseconds(250);

// 创建并返回初始化的全局状态（清零红蓝双方机器人数组等）。
State CreateInitialState();

// 主界面绘制入口：按布局顺序组织各面板，并返回用户是否请求切换 UDP 视频上下翻转。
bool ShowMainWindow(const State &state,
                    RoboGo::io::MqttClient &mqtt_client,  // MQTT 客户端引用(开局面板下发选型/开始比赛指令用)
                    ImTextureRef video_texture_ref,  // 主/次视频纹理句柄(UDP 视频源，交给 ImGui 背景绘制层贴图)
                    const ImVec2 &video_size,  // UDP 视频原始像素尺寸，用于保持宽高比缩放
                    bool udp_video_display_ready,  // UDP 视频当前是否有可显示帧(超时未刷新则为 false)
                    bool udp_video_inverted,  // UDP 视频当前是否处于上下翻转状态(展示勾选态)
                    ImTextureRef mini_map_texture_ref,  // 小地图底图纹理句柄(场地俯视图)
                    const ImVec2 &mini_map_texture_size,  // 小地图底图像素尺寸，用于坐标换算
                    ImTextureRef mini_map_terrain_texture_ref,  // 地形分层叠加层句柄
                    const ImVec2 &mini_map_terrain_texture_size); // 地形分层叠加层尺寸

// 每帧调用：清理已过期的 Buff 等需要基于时间推进的本地状态。
void UpdateState(State &state);

// 处理从 MQTT 收到的裁判系统/模拟器消息：
// 按 topic 解析对应 protobuf，刷新 UI 状态，必要时生成系统日志或透传自定义字节块。
void HandleMqttMessage(State &state,
                       RoboGo::io::MqttClient &mqtt_client,  // MQTT 客户端引用(下行分发目前未直接使用，保留以统一签名)
                       const std::string &topic,  // 消息话题名，即裁判系统协议里的消息类型(如 "GameStatus")
                       const std::string &payload);  // 消息原始字节串，交给对应 protobuf 的 ParseFromString 反序列化

// 追加一条本车（操作手视角）日志，同时输出到终端日志。
void AppendVehicleLog(State &state, int type, const std::string &text);

// 追加一条系统日志（裁判系统事件/判罚等），同时输出到终端日志。
void AppendSystemLog(State &state, int type, const std::string &text);
