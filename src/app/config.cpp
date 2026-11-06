// config.cpp：应用级配置读写实现——目前只持久化 MQTT 服务端地址。
//
// 为什么要有这个文件：
//   客户端默认连的是裁判系统（192.168.12.1:3333），但联调/复盘时会改成 127.0.0.1。
//   原先地址只存在内存里，每次启动都要重新输入，现场输错一位就白折腾半天。这里把
//   "上一次成功登录的地址"写进用户配置目录，下次启动直接预填。
#include "config.h"  // 本模块头文件

namespace RoboGo::app {  // 进入应用层命名空间

namespace {  // 匿名命名空间：内部实现细节

// 配置项键名（文件里是 "mqtt_server=host:port" 一行）
constexpr std::string_view kMqttServerKey = "mqtt_server";

// 功能：解析用户配置目录（不存在则按平台约定推导）。
std::filesystem::path ResolveConfigDirectory() {
#ifdef _WIN32
    if (const char *app_data = std::getenv("APPDATA"); app_data != nullptr && app_data[0] != '\0') {  // Windows 标准漫游目录
        return std::filesystem::path(app_data) / "RoboGo";  // 例如 C:/Users/xxx/AppData/Roaming/RoboGo
    }
#else
    if (const char *xdg_config = std::getenv("XDG_CONFIG_HOME"); xdg_config != nullptr && xdg_config[0] != '\0') {  // XDG 规范优先
        return std::filesystem::path(xdg_config) / "robogo";  // 例如 ~/.config/robogo
    }
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {  // 退回 ~/.config
        return std::filesystem::path(home) / ".config" / "robogo";  // 常见 Linux 布局
    }
#endif
    return std::filesystem::current_path();  // 兜底：当前工作目录（不会因为取不到环境变量而失败）
}

// 功能：去掉字符串首尾的 ASCII 空白。
std::string Trim(std::string_view text) {
    const auto is_space = [](char character) {  // 只把空格/制表/回车/换行当空白，避免 locale 依赖
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    };
    std::size_t begin = 0;  // 左边界
    std::size_t end = text.size();  // 右边界（开区间）
    while (begin < end && is_space(text[begin])) {  // 跳过左侧空白
        ++begin;  // 右移
    }
    while (end > begin && is_space(text[end - 1])) {  // 跳过右侧空白
        --end;  // 左移
    }
    return std::string(text.substr(begin, end - begin));  // 返回裁剪后的副本
}

}

// 功能：返回配置文件的绝对路径。
std::filesystem::path ConfigFilePath() {
    return ResolveConfigDirectory() / "config.ini";  // 固定文件名，便于用户手工编辑
}

// 功能：读取配置里的 MQTT 服务端地址。
// 思路：逐行扫描 key=value；忽略空行与 '#' 开头的注释；只认第一个 mqtt_server。
std::string LoadConfiguredMqttServer() {
    std::ifstream stream(ConfigFilePath());  // 打开配置文件（不存在时 is_open 为 false）
    if (!stream.is_open()) {  // 首次运行或文件被删
        return {};  // 返回空串，由调用方决定默认值
    }

    std::string line;  // 逐行缓冲
    while (std::getline(stream, line)) {  // 读到文件结束
        const std::string trimmed = Trim(line);  // 去掉首尾空白
        if (trimmed.empty() || trimmed.front() == '#') {  // 空行与注释行
            continue;  // 跳过
        }
        const std::size_t equals = trimmed.find('=');  // 找分隔符
        if (equals == std::string::npos) {  // 没有 '='，不是合法配置行
            continue;  // 跳过
        }
        if (Trim(std::string_view(trimmed).substr(0, equals)) != kMqttServerKey) {  // 键名不匹配
            continue;  // 跳过其它配置项
        }
        return Trim(std::string_view(trimmed).substr(equals + 1));  // 返回值部分（可能是空串）
    }
    return {};  // 没找到该项
}

// 功能：保存 MQTT 服务端地址。
bool SaveConfiguredMqttServer(std::string_view host_port) {
    const std::string value = Trim(host_port);  // 去掉用户输入可能带的空白
    if (value.empty()) {  // 空值不写，避免把配置清成空
        return false;  // 视为失败
    }

    const std::filesystem::path path = ConfigFilePath();  // 目标路径
    std::error_code error_code;  // 目录创建用 error_code 版本，避免抛异常
    std::filesystem::create_directories(path.parent_path(), error_code);  // 目录不存在则递归创建
    if (error_code) {  // 目录建不出来（权限/只读盘）
        SPDLOG_WARN("创建配置目录失败 path={} message={}", path.parent_path().string(), error_code.message());  // 记录但不影响主流程
        return false;  // 保存失败
    }

    const std::filesystem::path temporary_path = path.string() + ".tmp";  // 临时文件：先写它再原子替换
    {
        std::ofstream stream(temporary_path, std::ios::trunc);  // 覆盖写
        if (!stream.is_open()) {  // 打不开（只读/被占用）
            SPDLOG_WARN("写入配置文件失败 path={}", temporary_path.string());  // 记录原因
            return false;  // 保存失败
        }
        stream << "# RoboGo 配置（由客户端自动生成，可手工编辑）\n";  // 文件头说明
        stream << kMqttServerKey << "=" << value << "\n";  // 唯一一项配置
    }  // 作用域结束，文件自动关闭并刷盘

    std::filesystem::rename(temporary_path, path, error_code);  // 原子替换旧配置
    if (error_code) {  // 某些平台不允许覆盖已存在文件
        std::filesystem::remove(path, error_code);  // 先删旧的（失败也无所谓）
        error_code.clear();  // 清空错误码
        std::filesystem::rename(temporary_path, path, error_code);  // 再重试一次
    }
    if (error_code) {  // 两次都失败
        SPDLOG_WARN("保存配置文件失败 path={} message={}", path.string(), error_code.message());  // 记录原因
        return false;  // 保存失败
    }
    SPDLOG_INFO("MQTT 服务端地址已保存 path={} value={}", path.string(), value);  // 成功日志（现场排查很直观）
    return true;  // 保存成功
}

}  // namespace RoboGo::app
