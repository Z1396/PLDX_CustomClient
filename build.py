#!/usr/bin/env python3
# =============================================================================
# RoboGo 跨平台一键构建 & 运行脚本 (Python 版)
#
#   同一份脚本在 Windows 与 Linux 上都能直接运行：
#     Windows : python build.py   (Git Bash / PowerShell / cmd 均可)
#     Linux   : python3 build.py
#
#   Windows : clang-cl (CMake release 预设) + Ninja + Windows SDK
#             - 资源编译用 llvm-rc，注入 CMAKE_RC_FLAGS="-DWIN32 -clang:-P"
#               (规避 .rc 中 // 注释导致的预处理失败)；resources.rc 已保持纯 ASCII
#             - 自动把 MSVC 工具链(ml64.exe)加入 PATH，避免 boost_context 汇编失败
#             - 生成 build.ninja 后自动打补丁：HLSL 着色器步骤独占单作业池，
#               规避 ninja 在并行下读 cmd.exe /C 管道 "ReadFile: The handle is invalid"
#   Linux   : clang(>=16) / gcc(>=11) + Ninja + 系统 Vulkan SDK / GLFW
#             - 项目 CMake 已为 Linux 预留分支（glfw + vulkan + imgui vulkan 后端）
#             - 但 Linux 分支尚未完整验证：CMakeLists.txt 直接 link 了未定义的
#               'vulkan' 目标（缺 find_package(Vulkan)），configure 可能失败并给出提示
#
# 用法： python build.py [选项] [-- 程序参数]
#   python build.py                  # Release 编译并自动运行
#   python build.py --debug          # Debug 编译
#   python build.py --release --no-run   # 只编译不运行
#   python build.py -- --some-arg    # 编译后运行并传参
# =============================================================================

import os
import sys
import re
import glob
import shutil
import subprocess
import multiprocessing

# -------------------------- 颜色日志 --------------------------
USE_COLOR = sys.stdout.isatty()

def _c(code, s):
    return f"\033[{code}m{s}\033[0m" if USE_COLOR else s

def status(symbol, code, msg):
    print(f"{_c(code, symbol)} {msg}")

def info(msg):  status("[i]", 34, msg)
def ok(msg):    status("[o]", 32, msg)
def warn(msg):  status("[!]", 33, msg)
def die(msg):
    status("[x]", 31, msg)
    sys.exit(1)

# -------------------------- 默认配置 --------------------------
BUILD_TYPE = "Release"
ENABLE_ASAN = False
ENABLE_TSAN = False
RUN_AFTER_BUILD = True
VERBOSE = False
BUILD_TARGET = "RoboGo"
RUN_ARGS = []

# 运行时探测变量
OS_NAME = ""
ARCH = ""
BUILD_JOBS = 0
BUILD_DIR = ""
CMAKE_EXTRA_ARGS = []
EXE_PATH = ""

# -------------------------- 工具函数 --------------------------
def which(cmd):
    return shutil.which(cmd)

def detect_os():
    p = sys.platform
    if p.startswith("win"):
        return "windows"
    if p.startswith("linux"):
        return "linux"
    if p.startswith("msys") or p.startswith("cygwin"):
        # Git Bash / MSYS2 / Cygwin 下的 python 仍视为 Windows 工具链
        return "windows"
    return "unknown"

def detect_arch():
    m = platform_machine().lower()
    if m in ("x86_64", "amd64", "x86-64"):
        return "x86_64"
    if m in ("aarch64", "arm64", "arm"):
        return "arm64"
    return "unknown"

def platform_machine():
    import platform as _p
    return _p.machine()

def num_cpus():
    try:
        return max(1, multiprocessing.cpu_count())
    except Exception:
        return 4

def run_get_first(cmd):
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        if r.stdout:
            return r.stdout.splitlines()[0]
    except Exception:
        pass
    return ""

def clang_major(path):
    v = run_get_first([path, "--version"])
    m = re.search(r"clang version (\d+)", v)
    return int(m.group(1)) if m else 0

def gcc_major(path):
    v = run_get_first([path, "--version"])
    m = re.search(r"(\d+)\.\d+\.\d+", v)
    return int(m.group(1)) if m else 0

def run_cmd(cmd):
    info("运行: " + " ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        die(f"命令失败 (exit={r.returncode}): {' '.join(cmd)}")

def ensure_tools():
    if not which("cmake"):
        die("未找到 cmake，请安装并加入 PATH。")
    if not which("ninja"):
        die("未找到 ninja，请安装并加入 PATH。")

def add_msvc_tools_to_path():
    # Windows: boost_context 的 MASM 汇编(.asm)需要 ml64.exe。
    # 在 clang-cl 工具链下，MSVC 的 bin 目录默认不会加入 PATH，
    # 缺失时会出现 "ml64 ... CreateProcess failed: The system cannot find the file specified."
    # 这里动态查找并预置到 PATH，不修改任何源文件/CMake 文件。
    if OS_NAME != "windows":
        return
    candidates = []
    base = r"C:\Program Files (x86)\Microsoft Visual Studio"
    for ed in ("2022", "2019"):
        for sku in ("BuildTools", "Community", "Professional", "Enterprise"):
            root = os.path.join(base, ed, sku)
            if os.path.isdir(root):
                candidates += glob.glob(
                    os.path.join(root, "VC", "Tools", "MSVC", "*", "bin", "Hostx64", "x64", "ml64.exe")
                )
    found = [c for c in candidates if os.path.isfile(c)]
    if not found:
        warn("未找到 ml64.exe（MSVC MASM 汇编器）。若编译 boost_context 失败，请安装 VS Build Tools / MSVC。")
        return
    bindir = os.path.dirname(found[0])
    existing = [p.strip().lower() for p in os.environ.get("PATH", "").split(os.pathsep)]
    if bindir.lower() in existing:
        return
    os.environ["PATH"] = bindir + os.pathsep + os.environ.get("PATH", "")
    info(f"已将 MSVC 工具链加入 PATH: {bindir}")

def patch_build_ninja():
    # Windows + Ninja 已知缺陷：并行作业较多时，ninja 读取 `cmd.exe /C`
    # 自定义命令(DirectXTK12 的 HLSL 着色器生成)的 stdout 管道会偶发
    # "ReadFile: The handle is invalid"。该问题与 PowerShell / 控制台无关，
    # 在 Git Bash / cmd 同样会出现，且仅在“需要真正生成着色器”的干净构建时触发。
    # 修复：把该着色器生成步骤放入 depth=1 的独占池，使其不与其它并行作业
    # 同时运行（等价该单步用 -j 1），其余 472 个目标仍全并行。
    # 这是对“生成的 build.ninja”做的一次性补丁，不修改任何源码 / CMake 文件。
    if OS_NAME != "windows":
        return
    nj = os.path.join(BUILD_DIR, "build.ninja")
    if not os.path.isfile(nj):
        return
    with open(nj, "r", encoding="utf-8", errors="replace") as f:
        txt = f.read()
    if "pool = single_shaders" in txt:
        return  # 已打过补丁
    # 1) 注入 depth=1 池定义（置于文件顶部，确保先于使用）
    pool_def = "pool single_shaders\n  depth = 1\n\n"
    txt = pool_def + txt
    # 2) 把着色器 build 语句绑定到该池（紧跟 DESC 行之后）
    marker = "DESC = Generating HLSL shaders..."
    if marker in txt:
        txt = txt.replace(marker, marker + "\n  pool = single_shaders", 1)
    else:
        warn("未在 build.ninja 中找到 HLSL 着色器生成步骤，跳过 ninja 补丁。")
        return
    with open(nj, "w", encoding="utf-8", errors="replace") as f:
        f.write(txt)
    info("已对 build.ninja 打补丁：HLSL 着色器生成步骤使用独占单作业池（规避 ninja 管道 ReadFile 缺陷）。")

# -------------------------- 平台配置 --------------------------
def configure_windows():
    global BUILD_JOBS
    info("Configuring for Windows (clang-cl + Ninja via CMake preset)...")
    # RC 修复通过 -D 注入，不修改 CMakePresets.json：
    # 项目使用 llvm-rc(clang-cl 默认资源编译器)，需用 -clang:-P 规避 .rc 中 // 注释
    # 导致的预处理失败。resources.rc 中的 VERSIONINFO 必须为纯 ASCII
    # （llvm-rc 不支持非 ASCII 字符，如 ©），已将该字符改为 (C)。
    CMAKE_EXTRA_ARGS.append("-DCMAKE_RC_FLAGS=-DWIN32 -clang:-P")
    BUILD_JOBS = num_cpus()
    ok("Windows ready (preset-driven clang-cl)")

def configure_linux():
    global BUILD_JOBS
    info("Configuring for Linux...")
    cxx = ""
    cc = ""
    for c in ("clang++-22", "clang++-21", "clang++-20", "clang++-19", "clang++-18", "clang++"):
        if which(c):
            if clang_major(c) >= 16:
                cxx = which(c)
                break
    if not cxx:
        for c in ("g++-13", "g++-12", "g++-11", "g++"):
            if which(c):
                if gcc_major(c) >= 11:
                    cxx = which(c)
                    break
    if not cxx:
        die("Linux 上未找到 clang(>=16) 或 gcc(>=11)。")
    cc = cxx.replace("clang++", "clang")
    if not (cc and which(cc)):
        cc = which("gcc") or cxx
    CMAKE_EXTRA_ARGS.extend(["-DCMAKE_C_COMPILER=" + cc, "-DCMAKE_CXX_COMPILER=" + cxx])
    BUILD_JOBS = num_cpus()
    ok("Linux ready (" + os.path.basename(cxx) + ")")
    warn("Linux 构建依赖系统 Vulkan SDK + GLFW 开发包；且项目 CMake 当前引用了未定义的 'vulkan' 目标，configure 可能会失败。")

# -------------------------- CMake 流程 --------------------------
def configure_cmake():
    global BUILD_DIR
    if OS_NAME == "windows":
        preset = "debug" if BUILD_TYPE == "Debug" else "release"
        info(f"Configuring (preset={preset}) ...")
        BUILD_DIR = "build/" + preset
        run_cmd(["cmake", "--preset", preset] + CMAKE_EXTRA_ARGS)
    else:
        bd = "build/" + BUILD_TYPE.lower()
        info(f"Configuring (dir={bd}) ...")
        BUILD_DIR = bd
        run_cmd(["cmake", "-G", "Ninja", "-B", bd, "-S", ".",
                 "-DCMAKE_BUILD_TYPE=" + BUILD_TYPE] + CMAKE_EXTRA_ARGS)

def build_target():
    args = []
    if VERBOSE:
        args.append("--verbose")
    if OS_NAME == "windows":
        preset = "debug" if BUILD_TYPE == "Debug" else "release"
        info(f"Building {BUILD_TARGET} ({BUILD_TYPE}) via preset {preset} ...")
        run_cmd(["cmake", "--build", "--preset", preset, "--target", BUILD_TARGET] + args)
    else:
        info(f"Building {BUILD_TARGET} ({BUILD_TYPE}) ...")
        run_cmd(["cmake", "--build", BUILD_DIR, "--target", BUILD_TARGET] + args)
    ok("Build complete!")

def run_target():
    if not RUN_AFTER_BUILD:
        return
    exe = "RoboGo.exe" if OS_NAME == "windows" else "RoboGo"
    exe_path = os.path.join(BUILD_DIR, exe)
    if not os.path.isfile(exe_path):
        die(f"编译产物未找到: {exe_path}")
    info("启动 RoboGo ...")
    if RUN_ARGS:
        info("程序参数: " + " ".join(RUN_ARGS))
    r = subprocess.run([exe_path] + RUN_ARGS)
    sys.exit(r.returncode)

# -------------------------- 参数解析 --------------------------
def parse_args(argv):
    global BUILD_TYPE, ENABLE_ASAN, ENABLE_TSAN, RUN_AFTER_BUILD, VERBOSE, RUN_ARGS
    i = 0
    n = len(argv)
    while i < n:
        a = argv[i]
        if a == "--release":
            BUILD_TYPE = "Release"
        elif a == "--debug":
            BUILD_TYPE = "Debug"
        elif a == "--asan":
            ENABLE_ASAN = True
        elif a == "--tsan":
            ENABLE_TSAN = True
        elif a == "--no-run":
            RUN_AFTER_BUILD = False
        elif a == "--verbose":
            VERBOSE = True
        elif a == "--":
            RUN_ARGS = argv[i + 1:]
            break
        else:
            RUN_ARGS.append(a)
        i += 1

# -------------------------- 主入口 --------------------------
def main():
    global OS_NAME, ARCH, CMAKE_EXTRA_ARGS
    parse_args(sys.argv[1:])

    print()
    status("🤖", 34, "RoboGo Build & Run")
    print()

    if not (os.path.isfile("CMakeLists.txt") and os.path.isfile("CMakePresets.json")):
        die("请在项目根目录（含 CMakeLists.txt / CMakePresets.json）运行本脚本。")

    OS_NAME = detect_os()
    ARCH = detect_arch()
    if OS_NAME == "unknown":
        die(f"不支持的系统: {sys.platform}")
    ensure_tools()

    # Windows 下确保 MASM 汇编器(ml64.exe)在 PATH 中，否则 boost_context 编译会失败
    add_msvc_tools_to_path()

    if ENABLE_ASAN and ENABLE_TSAN:
        die("ASan 与 TSan 不能同时开启。")

    info(f"Platform: {OS_NAME} ({ARCH})")
    info(f"Build type: {BUILD_TYPE}")
    info(f"Run after build: {RUN_AFTER_BUILD}")

    if OS_NAME == "windows":
        configure_windows()
    else:
        configure_linux()

    if ENABLE_ASAN:
        warn("ASan 为实验性：依赖库需以 ASan 重新编译，否则可能链接/运行时报错。")
        CMAKE_EXTRA_ARGS.extend([
            "-DCMAKE_CXX_FLAGS=-fsanitize=address -fno-omit-frame-pointer",
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address",
        ])
        os.environ["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=0"
    if ENABLE_TSAN:
        warn("TSan 为实验性。")
        CMAKE_EXTRA_ARGS.extend([
            "-DCMAKE_CXX_FLAGS=-fsanitize=thread",
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread",
        ])

    os.environ["CMAKE_BUILD_PARALLEL_LEVEL"] = str(BUILD_JOBS)

    configure_cmake()
    # 生成 build.ninja 后、开始编译前，打上 ninja 管道缺陷补丁（仅 Windows）
    patch_build_ninja()
    build_target()
    run_target()

if __name__ == "__main__":
    main()
