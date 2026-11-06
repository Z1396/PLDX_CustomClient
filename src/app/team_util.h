#pragma once  // 头文件包含守卫

#include <pch.h>              // 预编译头：集中包含标准库与第三方头
#include "main_window.h"      // State 完整定义（robot_static_status / radar_info_to_client）
#include "app/robot_registry.h"  // kRadarRobotIds* 雷达槽位常量
#include <vector>             // std::vector：雷达 robot_id 列表

namespace RoboGo::app::team_util {  // 团队/阵营相关的共享工具，消除各面板重复实现

// 判断一个 robot_id 属于哪一方：< 100 为红方，>= 100 为蓝方。
// 协议里蓝方编号统一是红方 +100，全仓所有阵营判断都应走这里，避免各处手写 "< 100" 造成口径漂移。
inline bool IsRedRobotId(uint32_t robot_id) {
    return robot_id < 100;
}

// 判断己方是否红方：robot_id < 100 为红方；缺静态状态时按红方处理（与顶部状态栏/其它面板一致）。
// 注意必须与 robot_static_status.has_value() 守卫一起使用，缺数据时不解引用，避免未定义行为。
inline bool WeAreRed(const State &state) {
    return !state.robot_static_status.has_value() || IsRedRobotId(state.robot_static_status->robot_id());
}

// 把雷达槽位映射成 robot_id 列表（兼容 8 槽位与旧 12 槽位，红/蓝在前两种排序）。
// 与 mini_map 原本的内联列表完全一致，集中在此后两边共用单一来源。
inline std::vector<uint32_t> RadarRobotIds(const State &state, bool we_red) {
    using RoboGo::app::robot_registry::kRadarRobotIdsRedFirst;
    using RoboGo::app::robot_registry::kRadarRobotIdsBlueFirst;
    using RoboGo::app::robot_registry::kRadarRobotIdsRedFirst8;
    using RoboGo::app::robot_registry::kRadarRobotIdsBlueFirst8;
    const auto &radar = *state.radar_info_to_client;
    if (radar.robots_info_size() >= 12) {
        // 我方为红方时蓝方排在前，否则红方排在前（与地图打点/显示顺序一致）
        return we_red ? std::vector<uint32_t>(kRadarRobotIdsBlueFirst.begin(), kRadarRobotIdsBlueFirst.end())
                      : std::vector<uint32_t>(kRadarRobotIdsRedFirst.begin(), kRadarRobotIdsRedFirst.end());
    }
    return we_red ? std::vector<uint32_t>(kRadarRobotIdsBlueFirst8.begin(), kRadarRobotIdsBlueFirst8.end())
                  : std::vector<uint32_t>(kRadarRobotIdsRedFirst8.begin(), kRadarRobotIdsRedFirst8.end());
}

}  // namespace RoboGo::app::team_util
