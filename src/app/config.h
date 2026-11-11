// config.h：应用级配置的读写接口（目前只有 MQTT 服务端地址）。
// 说明：现场每次启动都手输 IP 是一次固定的出错机会，所以把上一次成功登录的地址持久化到用户配置目录。
//       文件很小（key=value 纯文本），不引入 JSON/INI 库。
#pragma once  // 头文件包含守卫：防止重复包含

#include <pch.h>  // 预编译头：集中包含标准库与第三方头

namespace RoboGo::app {  // 进入应用层命名空间

/**
 * @brief 返回配置文件的绝对路径。位置：Windows 为 %APPDATA%/RoboGo/config.ini；Linux 为 $XDG_CONFIG_HOME/robogo/config.ini或
 *        ~/.config/robogo/config.ini；都取不到时退回当前工作目录。
 */
std::filesystem::path ConfigFilePath();

/**
 * @brief 读取上次保存的 MQTT 服务端地址。
 * @return 形如 "192.168.12.1:3333" 的字符串；文件不存在/没有该项/读取失败时返回空串。
 */
std::string LoadConfiguredMqttServer();

/**
 * @brief 保存 MQTT 服务端地址（形如 "192.168.12.1:3333"）。
 * @note 思路：先写临时文件再原子替换，避免写入过程中断电/崩溃把原配置写坏。
 * @return true 表示保存成功。
 */
bool SaveConfiguredMqttServer(std::string_view host_port);

}  // namespace RoboGo::app
