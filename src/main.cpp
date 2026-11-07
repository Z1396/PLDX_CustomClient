// 程序入口模块：依据平台(Windows/Linux)收集命令行参数，创建平台后端，
// 初始化日志系统，并启动 RoboGo 顶层应用主循环；主循环结束后关闭日志。
#include <cstdio>   // 崩溃栈诊断用 std::fopen/std::fprintf
#include "app/application.h"  // 应用层接口：ParseLaunchOptions(解析启动参数) 与 RunApplication(运行主循环)
#include "logging/logging.h"  // 日志模块接口：initialize / shutdown
#include "platform/platform_backend.h"  // 平台后端抽象：CreatePlatformBackend 工厂与 application_name 访问器

#ifdef _WIN32   // 崩溃栈诊断需要 Win32 + dbghelp（仅在 Windows 下引入）
#include <windows.h>   // 异常处理与线程上下文：SetUnhandledExceptionFilter / CONTEXT
#include <shellapi.h>  // [pch-split] CommandLineToArgvW：把 WinMain 的命令行拆成 argv
#include <dbghelp.h>   // 符号解析：SymInitialize / SymFromAddr / StackWalk64
#endif   // 结束 Windows 头文件引入

// 文件内部匿名命名空间：集中存放与平台相关的命令行参数收集(Windows 标准 WinMain / 非 Windows 标准 main)
// 以及统一的 RunMain 主流程封装；这些内容仅本编译单元使用，不对外暴露接口。
namespace {  // 匿名命名空间：内部链接，避免符号与其他翻译单元冲突

#ifdef _WIN32  // 条件编译：以下代码只在 Windows 平台编译
// 崩溃栈记录：默认关闭，仅当设置环境变量 ROBOGO_CRASH_TRACE 时启用。
// 用途：现场出现崩溃时，把异常码与调用栈写进工作目录下的 crash_trace.txt，
//       便于在没有调试器的机器上（比赛现场通常不具备）定位问题。不设该变量时零开销。
// 说明：项目当前没有可用的调试器（lldb 缺 python311.dll、无 cdb），用 dbghelp 走 PDB 符号是最省事的定位手段。
//       定位完成后可整段删除；不设环境变量时完全不生效。
#pragma comment(lib, "dbghelp.lib")   // 链接 dbghelp（Windows SDK 自带）

// 功能：未处理异常过滤器——把异常地址与调用栈（含函数名/文件行号）写入 crash_trace.txt。
LONG WINAPI RoboGoCrashHandler(EXCEPTION_POINTERS *exception_info) {   // 由 SetUnhandledExceptionFilter 回调
    std::FILE *file = std::fopen("crash_trace.txt", "w");   // 输出文件放在进程工作目录
    if (file == nullptr) {   // 打不开就直接放行，避免二次崩溃
        return EXCEPTION_EXECUTE_HANDLER;   // 交回系统处理
    }   // 结束打不开分支
    std::fprintf(file, "ExceptionCode=0x%08lX Address=%p\n",   // 异常码（0xC0000005 即访问冲突）
                 exception_info->ExceptionRecord->ExceptionCode,   // 异常码
                 exception_info->ExceptionRecord->ExceptionAddress);   // 出错指令地址
    HANDLE process = GetCurrentProcess();   // 当前进程伪句柄
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);   // 带行号、还原 C++ 名、按需加载
    if (SymInitialize(process, nullptr, TRUE) == FALSE) {   // 初始化符号处理器（会自动找同名 PDB）
        std::fprintf(file, "SymInitialize failed=%lu\n", GetLastError());   // 初始化失败也要留下线索
        std::fclose(file);   // 关闭文件
        return EXCEPTION_EXECUTE_HANDLER;   
    }
    CONTEXT context = *exception_info->ContextRecord;   // 拷贝一份上下文（StackWalk64 会修改它）
    STACKFRAME64 frame{};   // 栈帧描述
    frame.AddrPC.Offset = context.Rip;   // 指令指针
    frame.AddrPC.Mode = AddrModeFlat;   // 平坦模式（x64）
    frame.AddrFrame.Offset = context.Rbp;   // 栈帧指针
    frame.AddrFrame.Mode = AddrModeFlat;   // 平坦模式
    frame.AddrStack.Offset = context.Rsp;   // 栈指针
    frame.AddrStack.Mode = AddrModeFlat;   // 平坦模式

    // 逐帧回溯，最多 40 层
    for (int index = 0; index < 40; ++index) {   // 遍历调用栈
        if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,   // 走一帧
                        nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == FALSE) {   // 回溯失败即停止
            break;
        }
        if (frame.AddrPC.Offset == 0) {   // 到达栈底
            break;
        }
        char symbol_buffer[sizeof(SYMBOL_INFO) + 256]{};   // 符号信息缓冲（变长结构）
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(symbol_buffer);   // 按 SYMBOL_INFO 解释
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);   // 结构大小
        symbol->MaxNameLen = 255;   // 名称容量
        DWORD64 displacement = 0;   // 符号内偏移
        const char *name = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != FALSE   // 查函数名
                                   ? symbol->Name   // 查到了
                                   : "<unknown>";   // 查不到
        IMAGEHLP_LINE64 line{};   // 行号信息
        line.SizeOfStruct = sizeof(line);   // 结构大小
        DWORD line_displacement = 0;   // 行内偏移
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line) != FALSE) {   // 有行号信息
            std::fprintf(file, "#%02d %s +0x%llx (%s:%lu)\n", index, name,   // 序号/函数名
                         static_cast<unsigned long long>(displacement),   // 符号内偏移
                         line.FileName, line.LineNumber);   // 源码文件与行号
        } else {   // 无行号信息
            std::fprintf(file, "#%02d %s +0x%llx\n", index, name,   // 只输出函数名
                         static_cast<unsigned long long>(displacement));   // 符号内偏移
        }
    }
    SymCleanup(process);   // 释放符号资源
    std::fclose(file);   // 关闭文件
    return EXCEPTION_EXECUTE_HANDLER;   // 让进程按系统默认方式退出
}

// 功能：按环境变量决定是否安装崩溃栈记录器。
void InstallCrashTraceIfRequested() {   // 在 WinMain 一开始调用
    const char *enabled = std::getenv("ROBOGO_CRASH_TRACE");   // 读取开关
    if (enabled != nullptr && *enabled != '\0') {   // 设置了才安装
        SetUnhandledExceptionFilter(RoboGoCrashHandler);   // 注册未处理异常过滤器
    }
}

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
    }

    // 跳过 argv[0]（程序路径），从第 1 个实际参数开始遍历。
    for (int i = 1; i < argc; ++i) {  // 逐个参数转码为 UTF-8
        int size = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, nullptr, 0, nullptr, nullptr); // 第一次调用传 nullptr 目标缓冲区，仅计算所需 UTF-8 字节数（含结尾 0）
        std::string arg(static_cast<size_t>(size), '\0'); // 按计算出的字节数预分配字符串缓冲区（含结尾 null）
        WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, arg.data(), size, nullptr, nullptr); // 第二次调用执行实际的宽字符到 UTF-8 转换，写入 arg 缓冲区
        arg.resize(static_cast<size_t>(size - 1)); // 去掉结尾的 null 字符，仅保留有效字符串内容
        args.push_back(std::move(arg)); // 将转码后的参数以 move 方式存入列表，避免拷贝
    }
    LocalFree(argv_w); // 释放系统分配的命令行参数数组，避免内存泄漏
    return args;  // 返回收集完成的参数列表
}
#else  // 非 Windows 平台走标准 C 入口
// 功能：在非 Windows 平台下收集标准 main 参数(argc/argv)为字符串列表。
// 思路：跳过 argv[0]（程序路径），将剩余 C 字符串直接构造为 std::string。
std::vector<std::string> CollectCommandLineArgs(const int argc, char *argv[]) {  // POSIX 版：直接使用 main 传入的 argv
    std::vector<std::string> args;  // 存放转换后的参数列表(不含程序自身路径)
    args.reserve(static_cast<size_t>(std::max(argc - 1, 0))); // 预分配容量，避免逐个 push 时的多次重新分配
    // 跳过 argv[0]（程序路径），从第 1 个实际参数开始收集。
    for (int i = 1; i < argc; ++i) {  // 逐个参数构造 std::string
        args.emplace_back(argv[i]);  // 原地构造字符串元素，省去临时对象拷贝
    }
    return args;  // 返回收集完成的参数列表
}
#endif

// 功能：统一的主流程入口，封装与平台无关的程序初始化与运行逻辑。
// 思路：创建平台后端 -> 解析启动参数 -> 初始化日志 -> 运行应用主循环 -> 关闭日志。
int RunMain(const std::vector<std::string> &args) {  // args 为已转码的命令行参数列表(不含程序路径)
    auto platform_backend = RoboGo::platform::CreatePlatformBackend(); // 工厂创建平台后端(Win=DX12+Win32 / Linux=Vulkan+GLFW)

    const RoboGo::app::LaunchOptions options = RoboGo::app::ParseLaunchOptions(args); // 解析命令行参数为结构化的启动选项

    RoboGo::logging::initialize(platform_backend->application_name(), 
                                options.invert_udp_video,
                                RoboGo::logging::ParseCaptureMode(options.capture_mode)); // 初始化 spdlog 日志系统，并按档位决定是否抓包

    const int result = RoboGo::app::RunApplication(*platform_backend, options); // 进入应用主循环，返回进程退出码

    RoboGo::logging::shutdown(); // 应用退出后释放日志资源

    return result;  // 把应用主循环的退出码回传给操作系统
}

}  // namespace

#ifdef _WIN32  // Windows 使用 GUI 子系统的 WinMain 作为入口(由链接器 /SUBSYSTEM:WINDOWS 指定)
// 功能：Windows 平台 GUI 程序入口(WinMain)。
// 思路：忽略未使用的 Win32 参数，收集命令行参数后调用 RunMain。
int WINAPI WinMain(HINSTANCE h_instance, HINSTANCE previous_instance, PSTR command_line, int show_command) {  // WINAPI 即 __stdcall 调用约定；HINSTANCE 为模块句柄，show_command 为窗口显示方式
    // 标记以下 Win32 参数为有意未使用，抑制编译器警告。
    (void) h_instance;  // 当前进程实例句柄，本项目不使用
    (void) previous_instance;  // 前一个实例句柄(Win16 遗留)，恒不使用
    (void) command_line;  // ANSI 命令行字符串；改用 GetCommandLineW 拿 Unicode 版本，故忽略
    (void) show_command;  // 主窗口初始显示方式，由框架自行控制
    InstallCrashTraceIfRequested(); // 可选诊断：设置 ROBOGO_CRASH_TRACE 时把崩溃调用栈写入 crash_trace.txt
    return RunMain(CollectCommandLineArgs()); // 收集 Windows 命令行参数后进入主流程
}
#else  // 非 Windows 平台使用标准 main
// 功能：非 Windows 平台的标准程序入口(main)。
// 思路：直接接收 argc/argv，转发给 RunMain 启动主流程。
int main(const int argc, char *argv[]) {  // argc 参数个数(含程序路径)；argv 参数数组
    return RunMain(CollectCommandLineArgs(argc, argv)); // 收集标准参数后进入主流程
}
#endif
