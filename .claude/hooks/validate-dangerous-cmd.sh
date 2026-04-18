#!/usr/bin/env bash
# .claude/hooks/validate-dangerous-cmd.sh
# PreToolUse Hook: 拦截危险的 Shell 命令，防止 Claude 误操作

set -euo pipefail

# 从标准输入读取 Claude Code 传递的 JSON 数据
INPUT=$(cat)

# 提取即将执行的命令
COMMAND=$(echo "$INPUT" | jq -r '.tool_input.command // ""')

# 如果无法解析命令，默认放行
if [[ -z "$COMMAND" ]]; then
    exit 0
fi

# 定义危险命令模式（正则表达式）
DANGEROUS_PATTERNS=(
    # 强制删除系统目录
    "rm -rf /"
    "rm -rf /usr"
    "rm -rf /etc"
    "rm -rf /var"
    "rm -rf /boot"
    "rm -rf /home"
    "rm -rf /root"
    # 破坏性磁盘操作
    "dd if="
    "mkfs"
    # 修改系统关键权限
    "chmod -R 777 /"
    "chown -R"
    # Fork 炸弹
    ":\(\)\{ :\|:& \}\;:"
    # 危险的内核参数修改
    "sysctl -w"
    # 破坏 Git 历史
    "git push --force"
    "git reset --hard"
    # 清理所有 Docker 资源
    "docker system prune -a -f"
    "docker rm -f \$(docker ps -aq)"
    # 卸载关键文件系统
    "umount -f /"
)

# 检查命令是否匹配危险模式
for pattern in "${DANGEROUS_PATTERNS[@]}"; do
    if echo "$COMMAND" | grep -Eq "$pattern"; then
        # 输出 JSON 格式的拒绝信息
        cat <<EOF
{
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "deny",
    "permissionDecisionReason": "命令包含潜在危险操作：匹配模式 '$pattern'。如果你确实需要执行此命令，请在终端中手动运行。"
  }
}
EOF
        exit 2
    fi
done

# 对可能造成数据丢失但仍可恢复的命令进行警告（但允许执行）
WARNING_PATTERNS=(
    "rm -rf ./"
    "rm -rf ~"
    "git clean -fdx"
)

for pattern in "${WARNING_PATTERNS[@]}"; do
    if echo "$COMMAND" | grep -Eq "$pattern"; then
        # 输出警告信息但仍允许执行
        cat <<EOF
{
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "allow",
    "permissionDecisionReason": "命令已允许执行，但请注意：'$COMMAND' 可能删除重要文件。建议确认操作范围。"
  }
}
EOF
        exit 0
    fi
done

# 默认允许执行
cat <<EOF
{
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "allow",
    "permissionDecisionReason": "命令安全检查通过。"
  }
}
EOF

exit 0