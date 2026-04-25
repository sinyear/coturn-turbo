#!/usr/bin/env bash
# .claude/hooks/check-deps.sh
# SessionStart Hook: 检查 coturn-turbo 项目的必要依赖是否已安装

set -euo pipefail

# 颜色定义（用于终端输出，不影响 JSON 响应）
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

# 记录缺失的依赖
MISSING_DEPS=()
WARNING_DEPS=()

# 检查基础构建工具
check_basic_tools() {
    echo -e "${YELLOW}>>> 检查基础构建工具...${NC}"
    local tools=("make" "gcc" "cmake" "pkg-config" "autoconf" "automake")
    for tool in "${tools[@]}"; do
        if ! command -v "$tool" &> /dev/null; then
            MISSING_DEPS+=("$tool")
            echo -e "  ${RED}✗${NC} $tool 未安装"
        else
            echo -e "  ${GREEN}✓${NC} $tool ($($tool --version | head -1))"
        fi
    done
}

# 检查开发库
check_libraries() {
    echo -e "${YELLOW}>>> 检查必需开发库...${NC}"
    
    # 检查 libevent (通过 pkg-config)
    if pkg-config --exists libevent; then
        echo -e "  ${GREEN}✓${NC} libevent $(pkg-config --modversion libevent)"
    else
        MISSING_DEPS+=("libevent-dev")
        echo -e "  ${RED}✗${NC} libevent 开发包未安装"
    fi

    # 检查 OpenSSL
    if pkg-config --exists openssl; then
        echo -e "  ${GREEN}✓${NC} OpenSSL $(pkg-config --modversion openssl)"
    else
        MISSING_DEPS+=("libssl-dev")
        echo -e "  ${RED}✗${NC} OpenSSL 开发包未安装"
    fi

    # 检查 SQLite3
    if pkg-config --exists sqlite3; then
        echo -e "  ${GREEN}✓${NC} SQLite3 $(pkg-config --modversion sqlite3)"
    else
        WARNING_DEPS+=("libsqlite3-dev")
        echo -e "  ${YELLOW}⚠${NC} SQLite3 开发包未安装（默认数据库后端，建议安装）"
    fi
}

# 检查 Turbo 可选依赖
check_turbo_deps() {
    echo -e "${YELLOW}>>> 检查 Turbo 模式可选依赖...${NC}"
    
    # 检查 DPDK
    if pkg-config --exists libdpdk 2>/dev/null; then
        echo -e "  ${GREEN}✓${NC} DPDK $(pkg-config --modversion libdpdk)"
    else
        WARNING_DEPS+=("dpdk-dev")
        echo -e "  ${YELLOW}⚠${NC} DPDK 开发包未安装（如需 DPDK 模式请安装）"
    fi

    # 检查 libbpf (AF_XDP)
    if pkg-config --exists libbpf 2>/dev/null; then
        echo -e "  ${GREEN}✓${NC} libbpf $(pkg-config --modversion libbpf)"
    else
        WARNING_DEPS+=("libbpf-dev")
        echo -e "  ${YELLOW}⚠${NC} libbpf 未安装（AF_XDP 模式需要）"
    fi

    # 检查 libxdp
    if pkg-config --exists libxdp 2>/dev/null; then
        echo -e "  ${GREEN}✓${NC} libxdp $(pkg-config --modversion libxdp)"
    else
        WARNING_DEPS+=("libxdp-dev")
        echo -e "  ${YELLOW}⚠${NC} libxdp 未安装（AF_XDP 模式需要）"
    fi

    # 检查大页内存（DPDK/AF_XDP 运行依赖）
    if [[ -f /proc/meminfo ]]; then
        HUGEPAGES_TOTAL=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
        if [[ "$HUGEPAGES_TOTAL" -gt 0 ]]; then
            echo -e "  ${GREEN}✓${NC} 大页内存已配置 (总计 ${HUGEPAGES_TOTAL} 页)"
        else
            echo -e "  ${YELLOW}⚠${NC} 大页内存未配置（运行 Turbo 模式建议配置）"
        fi
    fi
}

# 生成安装建议
generate_install_hint() {
    local os_hint=""
    if [[ -f /etc/os-release ]]; then
        . /etc/os-release
        case "$ID" in
            ubuntu|debian)
                os_hint="sudo apt update && sudo apt install -y"
                ;;
            centos|rhel|fedora)
                os_hint="sudo yum install -y"
                ;;
            arch)
                os_hint="sudo pacman -S"
                ;;
            *)
                os_hint="请使用你的包管理器安装"
                ;;
        esac
    fi

    if [[ ${#MISSING_DEPS[@]} -gt 0 ]]; then
        echo -e "\n${RED}错误：缺失必需依赖${NC}"
        echo -e "请运行以下命令安装："
        echo -e "  ${os_hint} ${MISSING_DEPS[*]}\n"
    fi

    if [[ ${#WARNING_DEPS[@]} -gt 0 ]]; then
        echo -e "${YELLOW}警告：缺失可选依赖${NC}"
        echo -e "如需完整功能，可运行："
        echo -e "  ${os_hint} ${WARNING_DEPS[*]}\n"
    fi
}

# 主流程
main() {
    echo -e "\n${GREEN}=== coturn-turbo 环境依赖检查 ===${NC}\n"
    
    check_basic_tools
    echo ""
    check_libraries
    echo ""
    check_turbo_deps
    echo ""

    # 生成安装提示
    generate_install_hint

    # 输出 JSON 格式的结果供 Claude Code 使用
    if [[ ${#MISSING_DEPS[@]} -gt 0 ]]; then
        STATUS="warning"
        MESSAGE="部分必需依赖缺失，可能影响构建。"
    else
        STATUS="success"
        MESSAGE="所有必需依赖已满足。"
    fi

    # 构建 JSON 响应
    cat <<EOF
{
  "hookSpecificOutput": {
    "hookEventName": "SessionStart",
    "status": "$STATUS",
    "message": "$MESSAGE",
    "missing_deps": $(printf '%s\n' "${MISSING_DEPS[@]}" | jq -R . | jq -s .),
    "warning_deps": $(printf '%s\n' "${WARNING_DEPS[@]}" | jq -R . | jq -s .)
  }
}
EOF
}

# 执行主函数
main