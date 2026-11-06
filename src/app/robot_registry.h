#pragma once  // 头文件包含守卫：防止重复包含
// 说明：集中 RoboMaster 2026/2027 客户端涉及的"机器人枚举 / 槽位 ID"常量，
//       消除散落在 start_panel / commander_panel 等处的魔法数字与字面量列表，
//       使届与届之间的规则变更（如机器人编号调整）只需改这一处。
//       全部为 inline constexpr，可安全被多个翻译单元包含（C++17 内联变量，不违反 ODR）。

#include <pch.h>  // 预编译头(PCH)：集中包含标准库与第三方头
#include <array>  // std::array：编译期定长数组
#include <cstdint>  // uint32_t：robot_id 类型

namespace RoboGo::app::robot_registry {  // 进入机器人注册表命名空间（隶属于应用层）

// 登录面板机器人 ID 表：[队伍][机器人序号] -> 实际 robot_id（蓝队为红队 +100）。
// 下标：0=重装 / 1=3号步兵 / 2=4号步兵 / 3=指挥手（沿用飞镖槽位 8 的连接）。
inline constexpr std::array<std::array<int, 4>, 2> kLoginRobotIds = {{
    {1, 3, 4, 8},          // 红队：1 重装 / 3、4 步兵 / 8 指挥手（沿用飞镖槽位连接）
    {101, 103, 104, 108},  // 蓝队：同上，robot_id 统一加 100
}};

// 雷达 12 槽位（含空中机器人 6/106）完整 robot_id 列表，两种排序：红方在前 / 蓝方在前。
// 与 mini_map 的槽位映射保持一致，兼容旧 8 槽位与新 12 槽位。
inline constexpr std::array<uint32_t, 12> kRadarRobotIdsRedFirst = {
    1, 2, 3, 4, 6, 7, 101, 102, 103, 104, 106, 107};  // 红方 1~7 + 蓝方 101~107
inline constexpr std::array<uint32_t, 12> kRadarRobotIdsBlueFirst = {
    101, 102, 103, 104, 106, 107, 1, 2, 3, 4, 6, 7};  // 蓝方 101~107 + 红方 1~7

// 雷达 8 槽位（无空中机器人 6/106）列表，两种排序：红方在前 / 蓝方在前。
inline constexpr std::array<uint32_t, 8> kRadarRobotIdsRedFirst8 = {
    1, 3, 4, 7, 101, 103, 104, 107};  // 红方 1/3/4/7 + 蓝方 101/103/104/107
inline constexpr std::array<uint32_t, 8> kRadarRobotIdsBlueFirst8 = {
    101, 103, 104, 107, 1, 3, 4, 7};  // 蓝方 101/103/104/107 + 红方 1/3/4/7

// 兵种编号(robot_id % 100) -> 本地血量数组槽位（red_robots/blue_robots 下标）。
// 与 main_window.cpp 的 client_indices_2027 同源（都是 1/3/4/7 -> 0/2/3/6），集中于此避免两处漂移。
// 仅 1/3/4/7 号进入血量数组；2(工程)/5(废弃步兵)/6(空中系统)/8(飞镖)不在其中，记为 -1（无效）。
inline constexpr int kRobotTypeToSlot[9] = {
    -1,  // 0：无效编号
    0,   // 1：重装
    -1,  // 2：工程（不在血量数组）
    2,   // 3：步兵
    3,   // 4：步兵
    -1,  // 5：废弃步兵
    -1,  // 6：空中系统
    6,   // 7：哨兵
    -1,  // 8：飞镖
};

// 兵种展示名：三个界面各用一种写法，但「叫什么」只在这里定义一次。
//   slot_name  —— 顶部状态栏在编号后显示（如 "3 步兵"）
//   standalone —— 指挥台卡片 / 登录身份按钮（如 "3号步兵"）
//   log_suffix —— 日志与事件文案（如 "红方步兵3"）
struct RobotTypeInfo {
    const char *slot_name;
    const char *standalone;
    const char *log_suffix;
};

inline constexpr std::array<RobotTypeInfo, 9> kRobotTypes = {{
        {"重装", "重装", "重装"},
        {"保留", "保留位", "保留位"},
        {"步兵", "3号步兵", "步兵3"},
        {"步兵", "4号步兵", "步兵4"},
        {"步兵", "5号步兵", "步兵5"},
        {"空中系统", "空中系统", "空中系统"},
        {"哨兵", "哨兵", "哨兵"},
        {"飞镖", "飞镖", "飞镖"},
        {"雷达", "雷达", "雷达"},
}};

// 把协议里的 robot_id 文本（如 "1" / "101" / "11"）翻译成「红方/蓝方 + 兵种」中文名。
// 非法输入返回 "未知机器人"；无法识别的兵种编号返回「红方12号」这类兜底文案。
std::string GetRobotName(const std::string &robot_id);

}  // namespace RoboGo::app::robot_registry
