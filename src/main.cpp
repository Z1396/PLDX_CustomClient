// 程序入口模块：依据平台(Windows/Linux)收集命令行参数，创建平台后端，
// 初始化日志系统，并启动 RoboGo 顶层应用主循环；主循环结束后关闭日志。
#include "app/application.h"
#include "logging/logging.h"
#include "platform/platform_backend.h"

namespace {

#ifdef _WIN32
// 功能：在 Windows 平台下解析进程 Unicode 命令行，转换为 UTF-8 字符串列表。
// 思路：GetCommandLineW 获取宽字符命令行，CommandLineToArgvW 切分参数，
//       再逐条用 WideCharToMultiByte(CP_UTF8) 转码为 std::string。
std::vector<std::string> CollectCommandLineArgs() {
    std::vector<std::string> args;
    int argc = 0;
    LPWSTR *argv_w = CommandLineToArgvW(GetCommandLineW(), &argc); // 调用 Win32 API 解析当前进程的宽字符命令行，返回参数指针数组
    // 解析失败（如系统内存不足）时直接返回空的参数列表。
    if (argv_w == nullptr) {
        return args;
    }

    // 跳过 argv[0]（程序路径），从第 1 个实际参数开始遍历。
    for (int i = 1; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, nullptr, 0, nullptr, nullptr); // 第一次调用传 nullptr 目标缓冲区，仅计算所需 UTF-8 字节数（含结尾 0）
        std::string arg(static_cast<size_t>(size), '\0'); // 按计算出的字节数预分配字符串缓冲区（含结尾 null）
        WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, arg.data(), size, nullptr, nullptr); // 第二次调用执行实际的宽字符到 UTF-8 转换，写入 arg 缓冲区
        arg.resize(static_cast<size_t>(size - 1)); // 去掉结尾的 null 字符，仅保留有效字符串内容
        args.push_back(std::move(arg)); // 将转码后的参数以 move 方式存入列表，避免拷贝
    }
    LocalFree(argv_w); // 释放系统分配的命令行参数数组，避免内存泄漏
    return args;
}
#else
// 功能：在非 Windows 平台下收集标准 main 参数(argc/argv)为字符串列表。
// 思路：跳过 argv[0]（程序路径），将剩余 C 字符串直接构造为 std::string。
std::vector<std::string> CollectCommandLineArgs(const int argc, char *argv[]) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(std::max(argc - 1, 0))); // 预分配容量，避免逐个 push 时的多次重新分配
    // 跳过 argv[0]（程序路径），从第 1 个实际参数开始收集。
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}
#endif

// 功能：统一的主流程入口，封装与平台无关的程序初始化与运行逻辑。
// 思路：创建平台后端 -> 解析启动参数 -> 初始化日志 -> 运行应用主循环 -> 关闭日志。
int RunMain(const std::vector<std::string> &args) {
    auto platform_backend = RoboGo::platform::CreatePlatformBackend(); // 工厂创建平台后端(Win=DX12+Win32 / Linux=Vulkan+GLFW)
    const RoboGo::app::LaunchOptions options = RoboGo::app::ParseLaunchOptions(args); // 解析命令行参数为结构化的启动选项
    RoboGo::logging::initialize(platform_backend->application_name(), options.invert_udp_video); // 初始化 spdlog 日志系统，传入应用名与视频翻转开关
    const int result = RoboGo::app::RunApplication(*platform_backend, options); // 进入应用主循环，返回进程退出码
    RoboGo::logging::shutdown(); // 应用退出后释放日志资源
    return result;
}

}// namespace

#ifdef _WIN32
// 功能：Windows 平台 GUI 程序入口(WinMain)。
// 思路：忽略未使用的 Win32 参数，收集命令行参数后调用 RunMain。
int WINAPI WinMain(HINSTANCE h_instance, HINSTANCE previous_instance, PSTR command_line, int show_command) {
    // 标记以下 Win32 参数为有意未使用，抑制编译器警告。
    (void) h_instance;
    (void) previous_instance;
    (void) command_line;
    (void) show_command;
    return RunMain(CollectCommandLineArgs()); // 收集 Windows 命令行参数后进入主流程
}
#else
// 功能：非 Windows 平台的标准程序入口(main)。
// 思路：直接接收 argc/argv，转发给 RunMain 启动主流程。
int main(const int argc, char *argv[]) {
    return RunMain(CollectCommandLineArgs(argc, argv)); // 收集标准参数后进入主流程
}
#endif
