// 界面侧机制与指令回执类 MQTT 消息处理（协议 2.2.14 起）：
// Buff、判罚、地图打点、雷达目标、科技核心、性能选型同步、能量机关、哨兵姿态、飞镖目标、哨兵指令回执。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]
#include "mqtt/handlers.h"

#include <ranges>  // std::ranges::find_if（Buff 原地续期查找）

#include "logging/logging.h"

namespace RoboGo::mqtt {

// 2.2.14 Buff 增益（获得时触发，此后 1Hz）：同一时刻可存在多个，按 robot_id + buff_type 原地续期。
void HandleBuff(State &state, std::string_view topic, const std::string &payload) {
    Buff buff;
    if (!ParseInto(topic, payload, buff)) {
        return;
    }
    BuffWithTimestamp buff_entry;
    buff_entry.buff = buff;
    buff_entry.update_time = std::chrono::steady_clock::now();  // 本地接收时刻，供 UpdateState 做过期判定

    auto it = std::ranges::find_if(state.active_buffs, [&buff](const BuffWithTimestamp &existing) {
        // 同一机器人 + 同一 Buff 类型视为「同一条」，应原地刷新而不是新增。
        return existing.buff.robot_id() == buff.robot_id() && existing.buff.buff_type() == buff.buff_type();
    });
    if (it != state.active_buffs.end()) {
        it->buff = buff;
        it->update_time = buff_entry.update_time;  // 重置过期倒计时
        SPDLOG_INFO("更新Buff robot_id={} buff_type={} left_time={}",
                    buff.robot_id(), buff.buff_type(), buff.buff_left_time());
        return;
    }

    state.active_buffs.push_back(buff_entry);
    // Buff 会被反复「过期-重新获得」，实测 35 秒 756 条。这里限流到每秒最多一条，
    // 并带上期间被省略的条数——既不丢「有新 Buff」这个事实，也不刷屏。
    static std::chrono::steady_clock::time_point last_buff_log_time{};
    static uint32_t buff_log_skips = 0;
    const auto now_time = std::chrono::steady_clock::now();
    const bool should_log = last_buff_log_time.time_since_epoch().count() == 0
                            || now_time - last_buff_log_time >= std::chrono::seconds(1);
    if (should_log) {
        SPDLOG_INFO("新增Buff robot_id={} buff_type={} left_time={} 省略={}",
                    buff.robot_id(), buff.buff_type(), buff.buff_left_time(), buff_log_skips);
        last_buff_log_time = now_time;
        buff_log_skips = 0;
    } else {
        ++buff_log_skips;
    }
}

// 2.2.15 判罚信息（触发式）：黄牌 / 红牌 / 超功率 / 超热量 / 超射速。
// 协议只推「罚了什么、罚多久」，不推剩余时间，因此记录本地接收时刻，由顶部横幅自行倒数。
void HandlePenaltyInfo(State &state, std::string_view topic, const std::string &payload) {
    PenaltyInfo penalty_info;
    if (!ParseInto(topic, payload, penalty_info)) {
        return;
    }
    const uint32_t penalty_type = penalty_info.penalty_type();
    const uint32_t penalty_effect_sec = penalty_info.penalty_effect_sec();

    std::string penalty_name;
    switch (penalty_type) {
        case 1: penalty_name = "黄牌"; break;
        case 2: penalty_name = "双方黄牌"; break;
        case 3: penalty_name = "红牌"; break;
        case 4: penalty_name = "超功率"; break;
        case 5: penalty_name = "超热量"; break;
        case 6: penalty_name = "超射速"; break;
        default: penalty_name = "未知判罚类型(" + std::to_string(penalty_type) + ")"; break;
    }
    const std::string text = penalty_name + ", 受罚效果时长" + std::to_string(penalty_effect_sec) + "秒";

    if (penalty_effect_sec > 0) {
        AppendSystemLog(state, 1, text);  // 判罚日志标记为 WARNING 级
    } else {
        // 时长为 0 表示「没有实际受罚效果」，常见于发送端整轮广播默认值：不占操作手视线，只留终端记录。
        SPDLOG_DEBUG("收到无实际效果的判罚消息，已忽略: {}", text);
    }
    state.penalty_info = std::move(penalty_info);
    state.penalty_received_time = std::chrono::steady_clock::now();
}

// 2.2.17 地图点击标记（触发式）：操作手打点后裁判系统回传的同步信息。
void HandleMapClickInfo(State &state, std::string_view topic, const std::string &payload) {
    MapClickInfo message;
    if (ParseInto(topic, payload, message)) {
        state.map_click_info = std::move(message);
    }
}

// 2.2.18 雷达目标信息（触发式）：12 台机器人位置。
void HandleRadarInfoToClient(State &state, std::string_view topic, const std::string &payload) {
    RadarInfoToClient message;
    if (ParseInto(topic, payload, message)) {
        state.radar_info_to_client = std::move(message);
    }
}

// 2.2.21 科技核心（能量机关）运动状态（1Hz）。
void HandleTechCoreMotionStateSync(State &state, std::string_view topic, const std::string &payload) {
    TechCoreMotionStateSync message;
    if (ParseInto(topic, payload, message)) {
        state.tech_core_motion_state_sync = std::move(message);
    }
}

// 2.2.23 性能体系同步（1Hz）：服务器权威的发射机构 / 底盘选型，开局面板据此回显。
void HandleRobotPerformanceSelectionSync(State &state, std::string_view topic, const std::string &payload) {
    RobotPerformanceSelectionSync message;
    if (!ParseInto(topic, payload, message)) {
        return;
    }
    state.robot_performance_selection_sync = std::move(message);
    // 记录收到同步的本地时刻，供开局面板判断「服务器刚推新值」，避免手动改选型时被旧值回弹覆盖。
    state.last_received_robot_performance_selection_sync = std::chrono::steady_clock::now();
}

// 2.2.28 能量机关状态（1Hz）：未激活 / 激活中 / 已激活 + 灯臂数 + 平均环数。
void HandleRuneStatusSync(State &state, std::string_view topic, const std::string &payload) {
    RuneStatusSync message;
    if (ParseInto(topic, payload, message)) {
        state.rune_status_sync = std::move(message);
    }
}

// 2.2.30 哨兵姿态（1Hz）：进攻 / 防御 / 移动 + 弱化 / 强化标记。
void HandleSentryStatusSync(State &state, std::string_view topic, const std::string &payload) {
    SentryStatusSync message;
    if (ParseInto(topic, payload, message)) {
        state.sentry_status_sync = std::move(message);
    }
}

// 2.2.31 飞镖目标选择（1Hz）：选定目标 + 闸门状态。
void HandleDartSelectTargetStatusSync(State &state, std::string_view topic, const std::string &payload) {
    DartSelectTargetStatusSync message;
    if (ParseInto(topic, payload, message)) {
        state.dart_select_target_status_sync = std::move(message);
    }
}

// 2.2.33 哨兵控制指令结果（触发式）：指令编号 + 执行结果码。
// 2027 协议只推「指令编号 + 结果码」不推失败原因，所以这里补一条日志：成功走 INFO、失败走 WARNING。
void HandleSentryCtrlResult(State &state, std::string_view topic, const std::string &payload) {
    SentryCtrlResult message;
    if (!ParseInto(topic, payload, message)) {
        return;
    }
    state.sentry_ctrl_result = message;
    const std::string result_text = "哨兵指令 " + std::to_string(message.command_id())
                                    + (message.result_code() == 0
                                               ? " 执行成功"
                                               : " 执行失败，结果码 " + std::to_string(message.result_code()));
    AppendSystemLog(state, message.result_code() == 0 ? 0 : 1, result_text);
}

}  // namespace RoboGo::mqtt
