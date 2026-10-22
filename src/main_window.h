// 主窗口与全局状态定义：本文件声明主界面入口 ShowMainWindow 及共享的比赛状态 State。
#pragma once

#include <pch.h>
#include "custom_control_payload.h"
#include "io/mqtt_client.h"

// 一条日志记录的存储结构（用于本车日志与系统日志）。
struct LogEntry {
    char time[9];// 时间戳字符串，格式 "HH:MM:SS"。
    std::string text;// 日志正文。
    int type;// 0: INFO, 1: WARNING, 2: ERROR
    uint64_t sequence = 0;// 单调递增序号，用于排序与去重。
};

// 单个机器人的血量/弹量状态（UI 展示用）。
struct RobotStatus {
    uint32_t health = 0;// 当前血量。
    uint32_t max_health = 0;// 历史最大血量（用于计算血条比例，仅在刷新时向上更新）。
    int32_t bullets = 0;// 允许发弹量（剩余弹药兑换额度）。
};

// 地图点位坐标（ID + 归一化/像素坐标）。
struct MapPosition {
    int id;
    float x;
    float y;
};

// 带更新时间戳的 Buff，用于计算剩余有效时间并自动过期清理。
struct BuffWithTimestamp {
    Buff buff;
    std::chrono::steady_clock::time_point update_time;// 收到该 Buff 的本地时刻。
};

// 全局比赛状态：聚合所有裁判系统/模拟器 MQTT 话题解析后的最新快照，供各面板读取显示。
struct State {
    std::optional<GameStatus> game_status;
    std::optional<GlobalUnitStatus> global_unit_status;
    std::optional<GlobalLogisticsStatus> global_logistics_status;
    std::optional<GlobalSpecialMechanism> global_special_mechanism;
    std::optional<RobotInjuryStat> robot_injury_stat;
    std::optional<RobotRespawnStatus> robot_respawn_status;
    std::optional<RobotStaticStatus> robot_static_status;
    std::optional<RobotDynamicStatus> robot_dynamic_status;
    std::optional<RobotModuleStatus> robot_module_status;
    std::optional<RobotPosition> robot_position;
    std::vector<BuffWithTimestamp> active_buffs;
    std::optional<RobotPathPlanInfo> robot_path_plan_info;
    std::optional<MapClickInfo> map_click_info;
    std::optional<RadarInfoToClient> radar_info_to_client;
    std::optional<TechCoreMotionStateSync> tech_core_motion_state_sync;
    std::optional<RobotPerformanceSelectionSync> robot_performance_selection_sync;
    std::chrono::steady_clock::time_point last_received_robot_performance_selection_sync;
    std::optional<DeployModeStatusSync> deploy_mode_status_sync;
    std::optional<RuneStatusSync> rune_status_sync;
    std::optional<SentryStatusSync> sentry_status_sync;
    std::optional<DartSelectTargetStatusSync> dart_select_target_status_sync;
    std::optional<SentryCtrlResult> sentry_ctrl_result;
    std::optional<AirSupportStatusSync> air_support_status_sync;
    AirSupportCustomControlState air_support_custom_control;
    std::optional<std::chrono::steady_clock::time_point> last_custom_control_publish_time;

    // 自定义状态
    bool auto_aim = false;
    bool gyro = false;

    // 机器人状态：英雄，工程，步兵x3，无人机，哨兵，飞镖，雷达
    RobotStatus red_robots[9];
    RobotStatus blue_robots[9];

    // 日志
    uint64_t next_log_sequence = 1;
    std::vector<LogEntry> vehicle_logs;
    std::vector<LogEntry> system_logs;
};

// 自定义字节块数据包回调：当收到 CustomByteBlock 话题且携带数据时被调用，转发给视频解码器。
using CustomByteBlockPacketHandler = std::function<void(std::span<const uint8_t> packet_bytes)>;

// UDP 视频流判定为不可用的超时阈值（超过该时间未刷新则回退到备选视频源）。
inline constexpr auto kUdpVideoFallbackTimeout = std::chrono::milliseconds(250);
// CustomByteBlock 视频流判定为不可用的超时阈值。
inline constexpr auto kCustomVideoFallbackTimeout = std::chrono::milliseconds(1000);

// 创建并返回初始化的全局状态（清零红蓝双方机器人数组等）。
State CreateInitialState();

// 主界面绘制入口：按布局顺序组织各面板，并返回用户是否请求切换 UDP 视频上下翻转。
bool ShowMainWindow(const State &state,
                    RoboGo::io::MqttClient &mqtt_client,
                    ImTextureRef video_texture_ref,
                    const ImVec2 &video_size,
                    ImTextureRef custom_video_texture_ref,
                    const ImVec2 &custom_video_size,
                    bool using_custom_video_as_main,
                    bool prefer_custom_video_as_main,
                    bool udp_video_display_ready,
                    bool custom_video_display_ready,
                    bool udp_video_inverted,
                    ImTextureRef mini_map_texture_ref,
                    const ImVec2 &mini_map_texture_size);

// 每帧调用：清理已过期的 Buff 等需要基于时间推进的本地状态。
void UpdateState(State &state);

// 在到达发布周期时，构造并下发空中支援/敌方血量等自定义控制包（CustomControl）给机器人电控。
void PublishCustomControlIfDue(State &state, RoboGo::io::MqttClient &mqtt_client);

// 处理从 MQTT 收到的裁判系统/模拟器消息：
// 按 topic 解析对应 protobuf，刷新 UI 状态，必要时生成系统日志或透传自定义字节块。
void HandleMqttMessage(State &state,
                       RoboGo::io::MqttClient &mqtt_client,
                       const std::string &topic,
                       const std::string &payload,
                       const CustomByteBlockPacketHandler &custom_byte_block_packet_handler = {});

// 追加一条本车（操作手视角）日志，同时输出到终端日志。
void AppendVehicleLog(State &state, int type, const std::string &text);

// 追加一条系统日志（裁判系统事件/判罚等），同时输出到终端日志。
void AppendSystemLog(State &state, int type, const std::string &text);

// 将机器人 ID（如 "1" / "101"）翻译成「红方/蓝方 + 兵种」中文名。
std::string GetRobotName(const std::string &robot_id);

// 把空中支援状态码翻译为中文文案。
const char *GetAirSupportStatusName(uint32_t airsupport_status);

// 把空中机器人发射机构状态码翻译为中文文案。
const char *GetAirSupportShooterStatusName(uint32_t shooter_status);
