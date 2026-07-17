#!/usr/bin/env bash
# ============================================================
# deploy-futu.sh — 编译产物 + 配置 + 脚本 → 本地或远程部署
# ============================================================
# 从 src/build 编译产物 + 本地生产配置，完整部署到目标环境。
#
# 【基本用法】
#   ./deploy-futu.sh local                         本地 Debug 部署
#   ./deploy-futu.sh remote                        远程 Debug 部署
#   ./deploy-futu.sh remote --release              远程 Release 部署
#   ./deploy-futu.sh remote --with-ctp             远程 + CTP SDK
#   ./deploy-futu.sh remote --release --with-ctp   远程实盘全量
#
# 【自定义目标】
#   ./deploy-futu.sh local --dest /opt/wt/futu
#   ./deploy-futu.sh remote --dest /data/wt/futu
#   ./deploy-futu.sh remote --host ubuntu@10.0.0.1 --dest /home/ubuntu/wt
#   ./deploy-futu.sh remote --host root@192.168.1.100 --pass mypass --dest /opt/wt
#
# 【选项说明】
#   --release       使用 Release 编译产物 (默认 Debug)
#   --with-ctp      附带 CTP 第三方 SDK (行情+交易 .so)
#   --dest <path>   部署目标目录 (覆盖默认值)
#   --host <user@ip> 远程主机 (覆盖默认 ubuntu@129.211.5.54, 仅 remote)
#   --pass <pwd>    SSH 密码 (覆盖默认 Clx@1028, 仅 remote)
#   --build-dir <path> 编译产物根目录 (覆盖自动检测)
#
# 【默认值】
#   本地目标:  /home/wondertrader/futu
#   远程主机:  ubuntu@129.211.5.54
#   远程目标:  /home/ubuntu/projects/wondertrader/futu
#   SSH 密码:  Clx@1028
#   编译产物:  src/build/build_x64/{Debug|Release}/bin/
#
# 【部署内容】
#   1. 编译产物 (.so + WtUftRunner)  →  bin/ lib/
#   2. CTP SDK (可选)                →  lib/
#   3. 配置文件                       →  conf/
#   4. 运维脚本                       →  scripts/
#   5. 目录骨架                       →  runtime/ logs/ data/
#
# 【典型场景】
#   # WSL 本地开发测试 (Debug + CTP SDK)
#   ./deploy-futu.sh local --with-ctp
#
#   # 腾讯云实盘部署 (Release + CTP SDK)
#   ./deploy-futu.sh remote --release --with-ctp
#
#   # 其他服务器部署
#   ./deploy-futu.sh remote --host ubuntu@1.2.3.4 --pass xxx --dest /opt/wt --release --with-ctp
#
# 【注意事项】
#   - 配置文件从本地部署目录 (默认 /home/wondertrader/futu/conf/) 同步
#   - credentials.env 不会同步 (远程需手动填写)
#   - CTP SDK 有两种命名: thost*.so (dlopen 用) 和 libthost*.so
#   - parsers/ 和 traders/ 会自动创建 CTP SDK 软链
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

# 默认值
LOCAL_DEPLOY="/home/wondertrader/futu"
REMOTE_HOST="ubuntu@129.211.5.54"
REMOTE_DEPLOY="/home/ubuntu/projects/wondertrader/futu"
REMOTE_PASS="Clx@1028"
BUILD_DIR_OVERRIDE=""

# ============================================================
# 解析参数
# ============================================================
TARGET=""
BUILD_TYPE="debug"
WITH_CTP=false
DEST_OVERRIDE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        local|remote) TARGET="$1"; shift ;;
        --release|release) BUILD_TYPE="release"; shift ;;
        --with-ctp) WITH_CTP=true; shift ;;
        --dest) DEST_OVERRIDE="$2"; shift 2 ;;
        --host) REMOTE_HOST="$2"; shift 2 ;;
        --pass) REMOTE_PASS="$2"; shift 2 ;;
        --build-dir) BUILD_DIR_OVERRIDE="$2"; shift 2 ;;
        -h|--help)
            cat << 'EOF'
Usage: deploy-futu.sh <local|remote> [options]

模式:
  local              部署到本地
  remote             部署到远程服务器

选项:
  --release          使用 Release 编译产物 (默认 Debug)
  --with-ctp         附带 CTP 第三方 SDK (行情+交易)
  --dest <path>      部署目标目录
  --host <user@ip>   远程主机 (仅 remote)
  --pass <pwd>       SSH 密码 (仅 remote)
  --build-dir <path> 编译产物根目录 (覆盖自动检测)

默认值:
  本地目标:  /home/wondertrader/futu
  远程主机:  ubuntu@129.211.5.54
  远程目标:  /home/ubuntu/projects/wondertrader/futu

示例:
  deploy-futu.sh local --with-ctp
  deploy-futu.sh remote --release --with-ctp
  deploy-futu.sh local --dest /opt/wt/futu --release
  deploy-futu.sh remote --host ubuntu@10.0.0.1 --dest /data/wt --with-ctp
EOF
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    echo "Usage: $0 <local|remote> [--release] [--with-ctp] [--dest PATH] [--host HOST] [--pass PASS]"
    echo ""
    echo "  local              部署到本地"
    echo "  remote             部署到远程服务器"
    echo ""
    echo "  --release          Release 编译产物 (默认 Debug)"
    echo "  --with-ctp         附带 CTP SDK"
    echo "  --dest <path>      目标目录 (默认: 本地 $LOCAL_DEPLOY / 远程 $REMOTE_DEPLOY)"
    echo "  --host <user@ip>   远程主机 (默认: $REMOTE_HOST)"
    echo "  --pass <pwd>       SSH 密码 (默认: 内置)"
    echo "  --build-dir <path> 编译产物根目录"
    echo ""
    echo "示例:"
    echo "  $0 local --with-ctp"
    echo "  $0 remote --release --with-ctp"
    echo "  $0 local --dest /opt/wt/futu"
    exit 1
fi

# 确定最终目标目录
if [[ "$TARGET" == "local" ]]; then
    DEPLOY_DEST="${DEST_OVERRIDE:-$LOCAL_DEPLOY}"
else
    DEPLOY_DEST="${DEST_OVERRIDE:-$REMOTE_DEPLOY}"
fi

# ============================================================
# 编译产物目录
# ============================================================
if [[ -n "$BUILD_DIR_OVERRIDE" ]]; then
    BUILD_BIN="$BUILD_DIR_OVERRIDE"
elif [[ "$BUILD_TYPE" == "release" ]]; then
    BUILD_BIN="$PROJECT_ROOT/src/build/build_x64/Release/bin"
else
    BUILD_BIN="$PROJECT_ROOT/src/build/build_x64/Debug/bin"
fi

echo "=== 模式: $BUILD_TYPE | CTP: $WITH_CTP | 目标: $TARGET ==="
echo "=== 目标目录: $DEPLOY_DEST ==="
if [[ "$TARGET" == "remote" ]]; then
    echo "=== 远程主机: $REMOTE_HOST ==="
fi

if [[ ! -d "$BUILD_BIN" ]]; then
    echo "ERROR: 编译产物目录不存在: $BUILD_BIN"
    echo "请先编译: cd $PROJECT_ROOT/src/build && cmake .. && make ..."
    exit 1
fi

# ============================================================
# 1. 收集编译产物
# ============================================================
echo "=== 1/4 验证编译产物 ==="

declare -A BINARIES
BINARIES["bin/WtUftRunner"]="$BUILD_BIN/WtUftRunner/WtUftRunner"
BINARIES["lib/futu/libWtFutuCore.so"]="$BUILD_BIN/WtUftRunner/futu/libWtFutuCore.so"
BINARIES["lib/libWtUftCore.so"]="$BUILD_BIN/libWtUftCore.so"
BINARIES["lib/libWtUftStraFact.so"]="$BUILD_BIN/WtUftRunner/libWtUftStraFact.so"
BINARIES["lib/libWtDataStorage.so"]="$BUILD_BIN/libWtDataStorage.so"
BINARIES["lib/libWtDataStorageAD.so"]="$BUILD_BIN/libWtDataStorageAD.so"
BINARIES["lib/parsers/libParserCTP.so"]="$BUILD_BIN/libParserCTP.so"
BINARIES["lib/traders/libTraderCTP.so"]="$BUILD_BIN/libTraderCTP.so"

MISSING=0
for dst in "${!BINARIES[@]}"; do
    src="${BINARIES[$dst]}"
    if [[ ! -f "$src" ]]; then
        echo "  MISSING: $src"
        MISSING=1
    else
        printf "  OK  %-50s (%s)\n" "$(basename "$src")" "$(du -h "$src" | cut -f1)"
    fi
done

# ============================================================
# 2. CTP SDK (可选)
# ============================================================
if $WITH_CTP; then
    echo "=== 2/4 CTP SDK ==="
    CTP_DIR="$PROJECT_ROOT/dist/bin"
    # CTP SDK 有两种命名: 带 lib 前缀和不带
    # ParserCTP.so dlopen 加载 thostmduserapi_se.so (不带 lib 前缀)
    # TraderCTP.so dlopen 加载 thosttraderapi_se.so (不带 lib 前缀)
    declare -A CTP_SDK=(
        ["lib/thosttraderapi_se.so"]="$CTP_DIR/thosttraderapi_se.so"
        ["lib/thostmduserapi_se.so"]="$CTP_DIR/thostmduserapi_se.so"
        ["lib/libthosttraderapi.so"]="$CTP_DIR/libthosttraderapi.so"
        ["lib/libthostmduserapi.so"]="$CTP_DIR/libthostmduserapi.so"
    )
    for dst in "${!CTP_SDK[@]}"; do
        src="${CTP_SDK[$dst]}"
        if [[ -f "$src" ]]; then
            BINARIES["$dst"]="$src"
            printf "  OK  %-50s (%s)\n" "$(basename "$src")" "$(du -h "$src" | cut -f1)"
        else
            echo "  WARN: 缺失 $src"
        fi
    done
fi

if [[ $MISSING -eq 1 ]]; then
    echo "ERROR: 编译产物缺失"
    exit 1
fi

# ============================================================
# 3. 配置文件
# ============================================================
echo "=== 3/4 配置文件 ==="
CONF_SRC="$LOCAL_DEPLOY/conf"

if [[ ! -d "$CONF_SRC" ]]; then
    echo "WARN: 本地配置目录不存在: $CONF_SRC"
    echo "请先执行一次本地部署: $0 local --with-ctp"
else
    echo "  源: $CONF_SRC"
    find "$CONF_SRC" -type f ! -name 'config.yaml' ! -path '*_bak*' -printf "  %P\n" | sort
fi

# ============================================================
# 4. 运维脚本
# ============================================================
echo "=== 4/4 运维脚本 ==="
SCRIPTS_SRC="$LOCAL_DEPLOY/scripts"

if [[ ! -d "$SCRIPTS_SRC" ]]; then
    echo "WARN: 本地脚本目录不存在: $SCRIPTS_SRC"
else
    echo "  源: $SCRIPTS_SRC"
    ls "$SCRIPTS_SRC"/*.sh 2>/dev/null | while read f; do
        echo "  $(basename "$f")"
    done
fi

# ============================================================
# 部署函数
# ============================================================
deploy_to_local() {
    local DEST="$1"
    echo
    echo "=== 本地部署: $DEST ==="

    mkdir -p "$DEST"/{bin,lib/{futu,parsers,traders},conf/common,scripts,runtime/{pid,outputs,outputs_archive,state},logs,data}

    # 编译产物 + CTP SDK
    for dst in "${!BINARIES[@]}"; do
        cp -v "${BINARIES[$dst]}" "$DEST/$dst"
    done

    # CTP SDK 软链: parsers/ 和 traders/ 需要访问 thost*.so
    if $WITH_CTP; then
        ln -sf ../thostmduserapi_se.so "$DEST/lib/parsers/thostmduserapi_se.so"
        ln -sf ../thostmduserapi.so "$DEST/lib/parsers/libthostmduserapi.so"
        ln -sf ../thosttraderapi_se.so "$DEST/lib/traders/thosttraderapi_se.so"
        ln -sf ../thosttraderapi.so "$DEST/lib/traders/libthosttraderapi.so"
    fi

    # 配置文件 (排除渲染后的 config.yaml 和备份)
    if [[ -d "$CONF_SRC" ]]; then
        echo
        echo "--- 配置文件 ---"
        rsync -a --exclude='config.yaml' --exclude='_bak*' "$CONF_SRC/" "$DEST/conf/"
    fi

    # 运维脚本 (排除凭据)
    if [[ -d "$SCRIPTS_SRC" ]]; then
        echo
        echo "--- 运维脚本 ---"
        rsync -a --exclude='credentials.env' "$SCRIPTS_SRC/" "$DEST/scripts/"
    fi

    echo
    echo "=== 完成 ==="
    echo "启动: cd $DEST && ./scripts/start.sh"
}

deploy_to_remote() {
    local DEST="$1"
    echo
    echo "=== 远程部署: $REMOTE_HOST:$DEST ==="

    $SSH_CMD "mkdir -p $DEST/{bin,lib/{futu,parsers,traders},conf/common,scripts,runtime/{pid,outputs,outputs_archive,state},logs,data}"

    # 编译产物 + CTP SDK
    for dst in "${!BINARIES[@]}"; do
        $SSHPASS scp -o StrictHostKeyChecking=no -q "${BINARIES[$dst]}" "$REMOTE_HOST:$DEST/$dst"
        echo "  -> $dst"
    done

    # CTP SDK 软链: parsers/ 和 traders/ 需要访问 thost*.so
    if $WITH_CTP; then
        $SSH_CMD "cd $DEST/lib/parsers && ln -sf ../thostmduserapi_se.so . && ln -sf ../thostmduserapi.so ."
        $SSH_CMD "cd $DEST/lib/traders && ln -sf ../thosttraderapi_se.so . && ln -sf ../thosttraderapi.so ."
    fi

    # 配置文件 (排除 config.yaml 和备份)
    if [[ -d "$CONF_SRC" ]]; then
        echo
        echo "--- 配置文件 ---"
        $SSHPASS rsync -az --progress \
            -e "ssh -o StrictHostKeyChecking=no" \
            --exclude='config.yaml' --exclude='_bak*' \
            "$CONF_SRC/" "$REMOTE_HOST:$DEST/conf/" 2>&1 | tail -3
    fi

    # 运维脚本 (排除凭据)
    if [[ -d "$SCRIPTS_SRC" ]]; then
        echo
        echo "--- 运维脚本 ---"
        $SSHPASS rsync -az --progress \
            -e "ssh -o StrictHostKeyChecking=no" \
            --exclude='credentials.env' \
            "$SCRIPTS_SRC/" "$REMOTE_HOST:$DEST/scripts/" 2>&1 | tail -3
    fi

    echo
    echo "=== 完成 ==="
    echo
    echo "远程下一步:"
    echo "  1. 填凭据: $SSH_CMD 'vim $DEST/scripts/credentials.env'"
    echo "  2. 测试启动: $SSH_CMD 'cd $DEST && ./scripts/start.sh'"
    echo "  3. 安装 crontab: $SSH_CMD 'crontab $DEST/scripts/wt-uft.crontab'"
}

# ============================================================
# 执行
# ============================================================
if [[ "$TARGET" == "local" ]]; then
    deploy_to_local "$DEPLOY_DEST"
elif [[ "$TARGET" == "remote" ]]; then
    SSHPASS="sshpass -p '$REMOTE_PASS'"
    SSH_CMD="$SSHPASS ssh -o StrictHostKeyChecking=no $REMOTE_HOST"
    deploy_to_remote "$DEPLOY_DEST"
fi
