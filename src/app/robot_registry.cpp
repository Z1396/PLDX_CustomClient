// 机器人 ID 与显示名的映射实现。
#include "app/robot_registry.h"

#include "app/team_util.h"   // IsRedRobotId：阵营判定单一来源
#include "logging/logging.h"

namespace RoboGo::app::robot_registry {

std::string GetRobotName(const std::string &robot_id) {
    if (robot_id.empty()) {
        SPDLOG_WARN("收到空机器人ID");
        return "未知机器人";
    }

    int id = 0;
    try {
        id = std::stoi(robot_id);  // 十进制解析，红方 1~12 / 蓝方 101~112
    } catch (const std::exception &error) {
        SPDLOG_WARN("机器人ID解析失败 robot_id={} message={}", robot_id, error.what());
        return "未知机器人";
    }

    const bool is_red = team_util::IsRedRobotId(static_cast<uint32_t>(id));
    const int num = is_red ? id : id - 100;  // 换算成 1~12 的兵种编号
    const std::string prefix = is_red ? "红方" : "蓝方";

    // 1~9 号走统一名字表；11/12 是建筑，10 号未定义，走兜底。
    if (num >= 1 && num <= 9) {
        return prefix + kRobotTypes[num - 1].log_suffix;
    }
    if (num == 11) {
        return prefix + "前哨站";
    }
    if (num == 12) {
        return prefix + "基地";
    }
    return prefix + std::to_string(num) + "号";
}

}  // namespace RoboGo::app::robot_registry
