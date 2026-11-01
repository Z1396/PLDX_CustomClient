// 程序入口模块：依据平台(Windows/Linux)收集命令行参数，创建平台后端，
// 初始化日志系统，并启动 RoboGo 顶层应用主循环；主循环结束后关闭日志。
#include "app/application.h"  // 应用层接口：ParseLaunchOptions(解析启动参数) 与 RunApplication(运行主循环)
#include "logging/logging.h"  // 日志模块接口：initialize / shutdown
#include "platform/platform_backend.h"  // 平台后端抽象：CreatePlatformBackend 工厂与 application_name 访问器

// 文件内部匿名命名空间：集中存放与平台相关的命令行参数收集(Windows 标准 WinMain / 非 Windows 标准 main)
// 以及统一的 RunMain 主流程封装；这些内容仅本编译单元使用，不对外暴露接口。
namespace {  // 匿名命名空间：内部链接，避免符号与其他翻译单元冲突

#ifdef _WIN32  // 条件编译：以下代码只在 Windows 平台编译
// 功能：在 Windows 平台下解析进程 Unicode 命令行，转换为 UTF-8 字符串列表。
// 思路：GetCommandLineW 获取宽字符命令行，CommandLineToArgvW 切分参数，
//       再逐条用 WideCharToMultiByte(CP_UTF8) 转码为 std::string。
std::vector<std::string> CollectCommandLineArgs() {  // Windows 版：不接收 argc/argv，直接从系统读完整命令行
    std::vector<std::string> args;  // 存放转码后的 UTF-8 参数列表(不含程序自身路径)
    int argc = 0;  // 出参：CommandLineToArgvW 写回切分出的参数个数
    LPWSTR *argv_w = CommandLineToArgvW(GetCommandLineW(), &argc); // 调用 Win32 API 解析当前进程的宽字符命令行，返回参数指针数组
    // 解析失败（如系统内存不足）时直接返回空的参数列表。
    if (argv_w == nullptr) {  // 返回空指针说明系统未能分配参数数组
        return args;  // 返回空列表，后续应用以默认参数启动
    }  // 结束 argv_w 空指针防护分支

    // 跳过 argv[0]（程序路径），从第 1 个实际参数开始遍历。
    for (int i = 1; i < argc; ++i) {  // 逐个参数转码为 UTF-8
        int size = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, nullptr, 0, nullptr, nullptr); // 第一次调用传 nullptr 目标缓冲区，仅计算所需 UTF-8 字节数（含结尾 0）
        std::string arg(static_cast<size_t>(size), '\0'); // 按计算出的字节数预分配字符串缓冲区（含结尾 null）
        WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, arg.data(), size, nullptr, nullptr); // 第二次调用执行实际的宽字符到 UTF-8 转换，写入 arg 缓冲区
        arg.resize(static_cast<size_t>(size - 1)); // 去掉结尾的 null 字符，仅保留有效字符串内容
        args.push_back(std::move(arg)); // 将转码后的参数以 move 方式存入列表，避免拷贝
    }  // 结束逐参数转码循环
    LocalFree(argv_w); // 释放系统分配的命令行参数数组，避免内存泄漏
    return args;  // 返回收集完成的参数列表
}  // 结束 Windows 版 CollectCommandLineArgs
#else  // 非 Windows 平台走标准 C 入口
// 功能：在非 Windows 平台下收集标准 main 参数(argc/argv)为字符串列表。
// 思路：跳过 argv[0]（程序路径），将剩余 C 字符串直接构造为 std::string。
std::vector<std::string> CollectCommandLineArgs(const int argc, char *argv[]) {  // POSIX 版：直接使用 main 传入的 argv
    std::vector<std::string> args;  // 存放转换后的参数列表(不含程序自身路径)
    args.reserve(static_cast<size_t>(std::max(argc - 1, 0))); // 预分配容量，避免逐个 push 时的多次重新分配
    // 跳过 argv[0]（程序路径），从第 1 个实际参数开始收集。
    for (int i = 1; i < argc; ++i) {  // 逐个参数构造 std::string
        args.emplace_back(argv[i]);  // 原地构造字符串元素，省去临时对象拷贝
    }  // 结束逐参数收集循环
    return args;  // 返回收集完成的参数列表
}  // 结束 POSIX 版 CollectCommandLineArgs
#endif  // 结束平台条件编译

// 功能：统一的主流程入口，封装与平台无关的程序初始化与运行逻辑。
// 思路：创建平台后端 -> 解析启动参数 -> 初始化日志 -> 运行应用主循环 -> 关闭日志。
int RunMain(const std::vector<std::string> &args) {  // args 为已转码的命令行参数列表(不含程序路径)
    auto platform_backend = RoboGo::platform::CreatePlatformBackend(); // 工厂创建平台后端(Win=DX12+Win32 / Linux=Vulkan+GLFW)

    const RoboGo::app::LaunchOptions options = RoboGo::app::ParseLaunchOptions(args); // 解析命令行参数为结构化的启动选项

    RoboGo::logging::initialize(platform_backend->application_name(), options.invert_udp_video); // 初始化 spdlog 日志系统，传入应用名与视频翻转开关
    
    const int result = RoboGo::app::RunApplication(*platform_backend, options); // 进入应用主循环，返回进程退出码
    
    RoboGo::logging::shutdown(); // 应用退出后释放日志资源
    
    return result;  // 把应用主循环的退出码回传给操作系统
}  // 结束 RunMain

}// namespace  // 匿名命名空间结束

#ifdef _WIN32  // Windows 使用 GUI 子系统的 WinMain 作为入口(由链接器 /SUBSYSTEM:WINDOWS 指定)
// 功能：Windows 平台 GUI 程序入口(WinMain)。
// 思路：忽略未使用的 Win32 参数，收集命令行参数后调用 RunMain。
int WINAPI WinMain(HINSTANCE h_instance, HINSTANCE previous_instance, PSTR command_line, int show_command) {  // WINAPI 即 __stdcall 调用约定；HINSTANCE 为模块句柄，show_command 为窗口显示方式
    // 标记以下 Win32 参数为有意未使用，抑制编译器警告。
    (void) h_instance;  // 当前进程实例句柄，本项目不使用
    (void) previous_instance;  // 前一个实例句柄(Win16 遗留)，恒不使用
    (void) command_line;  // ANSI 命令行字符串；改用 GetCommandLineW 拿 Unicode 版本，故忽略
    (void) show_command;  // 主窗口初始显示方式，由框架自行控制
    return RunMain(CollectCommandLineArgs()); // 收集 Windows 命令行参数后进入主流程
}  // 结束 WinMain
#else  // 非 Windows 平台使用标准 main
// 功能：非 Windows 平台的标准程序入口(main)。
// 思路：直接接收 argc/argv，转发给 RunMain 启动主流程。
int main(const int argc, char *argv[]) {  // argc 参数个数(含程序路径)；argv 参数数组
    return RunMain(CollectCommandLineArgs(argc, argv)); // 收集标准参数后进入主流程
}  // 结束 main
#endif  // 结束入口函数平台条件编译
