#!/usr/bin/env bash
# RoboGo / PLDX 自定义终端 —— Ubuntu 24.04 开发环境准备脚本
#
# 只负责「系统依赖」，不负责构建；构建统一走 python3 build.py 或 CMake preset。
#
#   bash tools/setup_ubuntu.sh          # 安装依赖 + 自检
#   bash tools/setup_ubuntu.sh --check  # 只检查、不装包
#
# 门槛说明：
#   - 项目使用 C++23 std::expected，需要 GCC 12+ / Clang 16+（Ubuntu 24.04 默认 gcc-13 即可）
#   - Linux 后端为 GLFW + Vulkan，缺 libvulkan-dev 会在 CMake configure 失败
#   - GLFW 由 3rdparty_downloads 编译，但它需要 X11/Wayland 开发头文件
#
# 本脚本不会修改网卡/IP；赛场需另行把有线网卡设为静态 192.168.12.2/24。

set -euo pipefail

CHECK_ONLY=0
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=1

say()  { printf '\033[36m[i]\033[0m %s\n' "$*"; }
ok()   { printf '\033[32m[o]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[!]\033[0m %s\n' "$*"; }
err()  { printf '\033[31m[x]\033[0m %s\n' "$*"; }

[[ "$(uname -s)" == "Linux" ]] || { err "本脚本只用于 Linux（当前 $(uname -s)）。Windows 请直接 python build.py。"; exit 1; }

# ---------------------------------------------------------------------------
# 1. 系统信息
# ---------------------------------------------------------------------------
if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    say "系统：${PRETTY_NAME:-unknown}"
fi
say "内核：$(uname -r)  架构：$(uname -m)"

# ---------------------------------------------------------------------------
# 2. 编译器版本（硬门槛）
# ---------------------------------------------------------------------------
compiler_ok=0
if command -v g++ >/dev/null 2>&1; then
    gcc_ver=$(g++ -dumpversion | cut -d. -f1)
    say "检测到 g++ $(g++ -dumpversion)"
    if [[ "${gcc_ver:-0}" -ge 12 ]]; then
        compiler_ok=1
        ok "g++ 满足 C++23 std::expected 要求"
    else
        warn "g++ 主版本 ${gcc_ver} < 12，缺少 <expected>"
    fi
fi
if command -v clang++ >/dev/null 2>&1; then
    say "检测到 $(clang++ --version | head -n1)"
    compiler_ok=1
fi

# ---------------------------------------------------------------------------
# 3. 依赖包
# ---------------------------------------------------------------------------
PACKAGES=(
    build-essential g++ cmake ninja-build python3 pkg-config
    libvulkan-dev vulkan-tools mesa-vulkan-drivers
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
    libxkbcommon-dev libwayland-dev wayland-protocols
    zlib1g-dev
)

if [[ "${CHECK_ONLY}" -eq 1 ]]; then
    say "--check：以下包将被安装（本次不执行 apt）"
    printf '    %s\n' "${PACKAGES[@]}"
else
    if [[ "$(id -u)" -ne 0 ]] && ! command -v sudo >/dev/null 2>&1; then
        err "需要 sudo/root 安装依赖；也可手动 apt install 上面列出的包。"
        exit 1
    fi
    say "apt update（失败则沿用现有索引）"
    sudo apt update -y || warn "apt update 失败，继续用现有索引"
    say "安装依赖"
    sudo apt install -y "${PACKAGES[@]}"
fi

# ---------------------------------------------------------------------------
# 4. 安装后复检
# ---------------------------------------------------------------------------
if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
    err "仍未找到 C++ 编译器，请检查 apt 是否成功。"
    exit 1
fi
if command -v g++ >/dev/null 2>&1; then
    gcc_ver=$(g++ -dumpversion | cut -d. -f1)
    [[ "${gcc_ver:-0}" -ge 12 ]] || { err "g++ 仍低于 12（当前 ${gcc_ver}），请确认软件源版本。"; exit 1; }
fi
ok "编译器检查通过"

# ---------------------------------------------------------------------------
# 5. Vulkan 设备（只报告，不阻断）
# ---------------------------------------------------------------------------
say "检查 Vulkan 设备"
if command -v vulkaninfo >/dev/null 2>&1; then
    if vulkaninfo --summary 2>/dev/null | grep -q "deviceName"; then
        vulkaninfo --summary 2>/dev/null | grep -E "deviceName|driverName|apiVersion" | head -n 6 || true
        if vulkaninfo --summary 2>/dev/null | grep -qiE "llvmpipe|lavapipe|softpipe"; then
            warn "只有软件 Vulkan。界面能跑，但 60fps 图传的帧率不能作为现场依据，请装显卡驱动。"
        else
            ok "检测到硬件 Vulkan 设备"
        fi
    else
        warn "vulkaninfo 未返回设备：无显示环境下属正常；有桌面则检查显卡驱动。"
    fi
else
    warn "未安装 vulkan-tools，跳过 GPU 检查"
fi

# ---------------------------------------------------------------------------
# 6. 后续步骤
# ---------------------------------------------------------------------------
cat <<'EOF'

--------------------------------------------------------------------
环境就绪。在项目根目录依次执行：

  python3 build.py --no-run                     # 编译 Release
  ctest --test-dir build/linux-release --output-on-failure
  python3 tools/mock_rm_server.py --selftest    # 不含 UI 的协议自检
  python3 build.py -- --dev                     # 连本地 mock 启动界面

说明：
  - mock 要在本机（Linux）里跑，客户端 --dev 才能用 127.0.0.1:3333 连上；
    不要指望 WSL/Windows 侧的 127.0.0.1 能互通。
  - 图传硬解：Linux 无 D3D11VA，随附 FFmpeg 只含 cuvid + 软解；
    无 N 卡时先看 perf 面板的「解码」耗时，再决定是否重建 FFmpeg 开 VAAPI。
  - 赛场部署：网卡静态 192.168.12.2/24，直连裁判系统 192.168.12.1，
    不要用 WSL2/NAT/端口转发。
--------------------------------------------------------------------
EOF
