#!/bin/bash
# =============================================================================
# manage_linux.sh - WonderTrader Build/Deploy/Run Management Script
# =============================================================================
# Usage:
#   ./manage_linux.sh build [Debug|Release]           # Build all
#   ./manage_linux.sh build <strategy> [Debug|Release] # Build specific strategy
#   ./manage_linux.sh deploy <strategy>               # Deploy strategy
#   ./manage_linux.sh start <strategy>                # Start strategy
#   ./manage_linux.sh all <strategy> [Debug|Release]  # Build + Deploy + Start
#
# Strategies: cta, hft, uft, futu, opt
# =============================================================================

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Global paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
SRC_DIR="$PROJECT_ROOT/src"
BUILD_DIR="$SRC_DIR/build_all"
DIST_DIR="$PROJECT_ROOT/dist"

# Strategy configuration
declare -A STRATEGY_CONFIG
STRATEGY_CONFIG[cta]="WtRunnerCta|WtRunner|libWtCtaStraFact.so|cta"
STRATEGY_CONFIG[hft]="WtRunnerHft|WtRunner|libWtHftStraFact.so|hft"
STRATEGY_CONFIG[uft]="WtRunnerUft|WtUftRunner|libWtUftStraFact.so|uft"
STRATEGY_CONFIG[futu]="WtRunnerFutu|WtUftRunner|libWtFutuCore.so;libWtUftStraFact.so|uft"
STRATEGY_CONFIG[opt]="WtRunnerOpt|WtRunner|libWtOptionCore.so|opt"

# Build targets
declare -A BUILD_TARGETS
BUILD_TARGETS[cta]="WtCore WtRunner WtCtaStraFact"
BUILD_TARGETS[hft]="WtCore WtRunner WtHftStraFact"
BUILD_TARGETS[uft]="WtUftCore WtUftRunner WtUftStraFact"
BUILD_TARGETS[futu]="WtUftCore WtFutuCore WtUftRunner WtUftStraFact"
BUILD_TARGETS[opt]="WtCore WtOptionCore WtRunner"

# Logging functions
log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

detect_os() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        OS=$NAME
    else
        log_error "Cannot detect OS distribution."
        exit 1
    fi
}

install_package() {
    local PKG=$1
    if [[ "$OS" == *"Ubuntu"* ]] || [[ "$OS" == *"Debian"* ]]; then
        sudo apt-get update && sudo apt-get install -y $PKG
    elif [[ "$OS" == *"CentOS"* ]] || [[ "$OS" == *"Red Hat"* ]]; then
        sudo yum install -y $PKG
    else
        log_warn "Auto-install not supported for $OS"
    fi
}

check_command() {
    local CMD=$1
    local PKG=$2
    if ! command -v $CMD &> /dev/null; then
        log_warn "$CMD not found. Installing..."
        install_package $PKG
    fi
    log_info "$CMD found."
}

check_dependencies() {
    log_info "Checking build tools..."
    check_command cmake cmake
    check_command make make
    check_command g++ g++
    check_command git git
}

install_spdlog() {
    log_info "Installing spdlog from source..."
    local DEPS_DIR="$PROJECT_ROOT/deps"
    mkdir -p "$DEPS_DIR"
    cd "$DEPS_DIR"

    if [ ! -d "spdlog-1.9.2" ]; then
        [ ! -f "v1.9.2.tar.gz" ] && wget https://github.com/gabime/spdlog/archive/refs/tags/v1.9.2.tar.gz
        tar -xzf v1.9.2.tar.gz
    fi

    cd spdlog-1.9.2 && mkdir -p build && cd build
    cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DSPDLOG_FMT_EXTERNAL=OFF -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/usr" ..
    make -j$(nproc) && make install

    export CMAKE_PREFIX_PATH="$DEPS_DIR/usr:$CMAKE_PREFIX_PATH"
    export CPATH="$DEPS_DIR/usr/include:$CPATH"
    export LD_LIBRARY_PATH="$DEPS_DIR/usr/lib:$LD_LIBRARY_PATH"

    log_info "spdlog installed to $DEPS_DIR/usr"
    cd "$SRC_DIR"
}

build_all() {
    local BUILD_TYPE="${1:-Release}"
    
    # 如果第一个参数是 "all"，则取第二个参数作为 BUILD_TYPE
    if [[ "$BUILD_TYPE" == "all" ]]; then
        BUILD_TYPE="${2:-Release}"
    fi

    log_info "========================================"
    log_info "  WonderTrader Build Script"
    log_info "========================================"
    log_info "Build Type: $BUILD_TYPE"

    detect_os
    check_dependencies

    # Check spdlog
    if [ ! -f "/usr/include/spdlog/fmt/bundled/format.h" ] && [ ! -f "/usr/local/include/spdlog/fmt/bundled/format.h" ]; then
        log_warn "spdlog missing. Building from source..."
        install_spdlog
    fi

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Check if we need to reconfigure with new build type
    local NEED_CMAKE=false
    if [ ! -f "CMakeCache.txt" ]; then
        NEED_CMAKE=true
    else
        local CACHED_TYPE=$(grep "^CMAKE_BUILD_TYPE:" CMakeCache.txt 2>/dev/null | cut -d= -f2)
        if [[ "$CACHED_TYPE" != "$BUILD_TYPE" ]]; then
            log_info "Build type changed from '$CACHED_TYPE' to '$BUILD_TYPE', removing cache and reconfiguring..."
            rm -f CMakeCache.txt
            NEED_CMAKE=true
        fi
    fi

    if [ "$NEED_CMAKE" = true ]; then
        log_info "Running CMake..."
        cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .. || { log_error "CMake failed!"; exit 1; }
    fi

    # Build base dependencies first (to avoid parallel build issues with post-build copy commands)
    log_info "Building base dependencies..."
    make -j$(nproc) \
        Share WTSUtils WTSTools \
        ParserCTP ParserCTPMini ParserCTPOpt ParserFemas ParserShm ParserUDP ParserXTP ParserXeleSkt \
        TraderCTP TraderCTPMini TraderCTPOpt TraderFemas TraderMocker TraderXTP TraderYD TraderDumper \
        WtExeFact WtRiskMonFact WtDataStorage WtDataStorageAD WtMsgQue \
        2>/dev/null || true

    log_info "Building all targets..."
    make -j$(nproc) || { log_error "Make failed!"; exit 1; }

    log_info "Build completed!"
}

build_strategy() {
    local STRATEGY=$1
    local BUILD_TYPE="${2:-Release}"

    if [[ -z "${STRATEGY_CONFIG[$STRATEGY]}" ]]; then
        log_error "Unknown strategy: $STRATEGY"
        log_info "Valid: cta, hft, uft, futu, opt"
        exit 1
    fi

    log_info "Building strategy: $STRATEGY ($BUILD_TYPE)"

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Check if we need to reconfigure with new build type
    local NEED_CMAKE=false
    if [ ! -f "CMakeCache.txt" ]; then
        NEED_CMAKE=true
    else
        local CACHED_TYPE=$(grep "^CMAKE_BUILD_TYPE:" CMakeCache.txt 2>/dev/null | cut -d= -f2)
        if [[ "$CACHED_TYPE" != "$BUILD_TYPE" ]]; then
            log_info "Build type changed from '$CACHED_TYPE' to '$BUILD_TYPE', reconfiguring..."
            NEED_CMAKE=true
        fi
    fi

    if [ "$NEED_CMAKE" = true ]; then
        cmake -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .. || { log_error "CMake failed!"; exit 1; }
    fi

    # Build all base dependencies first (parsers, traders, core libs)
    # This ensures WtRunner can find all required files
    log_info "Building base dependencies..."
    make -j$(nproc) \
        Share WTSUtils WTSTools \
        ParserCTP ParserCTPMini ParserCTPOpt ParserFemas ParserShm ParserUDP ParserXTP ParserXeleSkt \
        TraderCTP TraderCTPMini TraderCTPOpt TraderFemas TraderMocker TraderXTP TraderYD TraderDumper \
        WtExeFact WtRiskMonFact WtDataStorage WtDataStorageAD WtMsgQue \
        2>/dev/null || true

    # Build strategy-specific deps
    case $STRATEGY in
        cta|hft|opt) 
            log_info "Building WtCore..."
            make WtCore -j$(nproc) 2>/dev/null || true
            ;;
        uft|futu) 
            log_info "Building WtUftCore..."
            make WtUftCore -j$(nproc) 2>/dev/null || true
            ;;
    esac

    # Build strategy targets
    for target in ${BUILD_TARGETS[$STRATEGY]}; do
        log_info "Building $target..."
        make $target -j$(nproc) || { log_error "Failed to build $target"; exit 1; }
    done

    log_info "Strategy $STRATEGY build completed!"
}

# =============================================================================
# 部署共享文件到 dist/bin 目录
# 包含: 执行器、解析器模块、交易模块、核心库、第三方API库
# =============================================================================
deploy_common() {
    local BIN_DIR="$DIST_DIR/bin"
    log_info "Deploying common files to $BIN_DIR"

    mkdir -p "$BIN_DIR"/{parsers,traders}

    # Determine build directory (prefer Debug if exists)
    local BUILD_SUBDIR="build_x64/all"
    if [ -d "$BUILD_DIR/build_x64/Debug" ]; then
        BUILD_SUBDIR="build_x64/Debug"
        log_info "Using Debug build directory"
    fi

    # Copy executables
    for exec in WtRunner WtUftRunner WtBtRunner; do
        local EXEC_PATH="$BUILD_DIR/$BUILD_SUBDIR/bin/$exec/$exec"
        if [ -f "$EXEC_PATH" ]; then
            cp -fv "$EXEC_PATH" "$BIN_DIR/"
        else
            # Fallback to find
            EXEC_PATH=$(find "$BUILD_DIR" -name "$exec" -type f 2>/dev/null | head -1)
            [ -n "$EXEC_PATH" ] && cp -fv "$EXEC_PATH" "$BIN_DIR/"
        fi
    done

    # Copy parser modules
    find "$BUILD_DIR/$BUILD_SUBDIR/bin" -name "libParser*.so" -exec cp -fv {} "$BIN_DIR/parsers/" \; 2>/dev/null || true

    # Copy trader modules
    find "$BUILD_DIR/$BUILD_SUBDIR/bin" -name "libTrader*.so" -exec cp -fv {} "$BIN_DIR/traders/" \; 2>/dev/null || true

    # Copy core libraries
    for lib in libWtCore.so libWtUftCore.so libWtDataStorage.so libWtDataStorageAD.so \
               libWtDtHelper.so libWtDtPorter.so libWtDtServo.so libWtExecMon.so \
               libWtExeFact.so libWtMsgQue.so libWtPorter.so libWtRiskMonFact.so \
               libWtShareHelper.so libWtBtPorter.so libObjects.so libEncryptedPassword_demo.so; do
        local LIB_PATH=$(find "$BUILD_DIR/$BUILD_SUBDIR/bin" -name "$lib" -type f 2>/dev/null | head -1)
        [ -n "$LIB_PATH" ] && cp -fv "$LIB_PATH" "$BIN_DIR/"
    done

    # Copy strategy factory libraries (shared)
    for lib in libWtCtaStraFact.so libWtHftStraFact.so libWtUftStraFact.so libWtOptionCore.so; do
        local LIB_PATH=$(find "$BUILD_DIR/$BUILD_SUBDIR/bin" -name "$lib" -type f 2>/dev/null | head -1)
        [ -n "$LIB_PATH" ] && cp -fv "$LIB_PATH" "$BIN_DIR/"
    done

    # Copy API libraries from src/API
    if [ -d "$SRC_DIR/API" ]; then
        find "$SRC_DIR/API" -name "*.so" -exec cp -fv {} "$BIN_DIR/" \; 2>/dev/null || true
    fi

    log_info "Common deployment completed!"
}

# =============================================================================
# 部署策略 - 仅部署策略特定文件到策略目录
# 策略目录包含: 配置文件、策略库、日志目录
# 共享文件位于 dist/bin 和 dist/common
# =============================================================================
deploy_strategy() {
    local STRATEGY=$1

    if [[ -z "${STRATEGY_CONFIG[$STRATEGY]}" ]]; then
        log_error "Unknown strategy: $STRATEGY"
        exit 1
    fi

    IFS='|' read -r DEPLOY_DIR EXECUTABLE STRATEGY_LIBS SUBDIR <<< "${STRATEGY_CONFIG[$STRATEGY]}"

    local TARGET_DIR="$DIST_DIR/$DEPLOY_DIR"
    log_info "Deploying $STRATEGY to $TARGET_DIR"

    # Stop running process if exists
    local PID=$(pgrep -f "$TARGET_DIR.*$EXECUTABLE" 2>/dev/null || true)
    if [ -n "$PID" ]; then
        log_warn "Stopping running $EXECUTABLE (PID: $PID)..."
        kill $PID 2>/dev/null || true
        sleep 1
        # Force kill if still running
        if pgrep -f "$TARGET_DIR.*$EXECUTABLE" >/dev/null 2>&1; then
            log_warn "Force killing $EXECUTABLE..."
            pkill -9 -f "$TARGET_DIR.*$EXECUTABLE" 2>/dev/null || true
            sleep 1
        fi
    fi

    # First, deploy common files
    deploy_common

    # Create strategy directory structure
    mkdir -p "$TARGET_DIR"/{$SUBDIR,Logs,data}

    # Create symbolic links to shared parsers and traders directories
    # This allows module names in config to work without full paths
    rm -rf "$TARGET_DIR/parsers" "$TARGET_DIR/traders" 2>/dev/null || true
    ln -sf ../bin/parsers "$TARGET_DIR/parsers"
    ln -sf ../bin/traders "$TARGET_DIR/traders"
    log_info "Created symlinks: parsers -> ../bin/parsers, traders -> ../bin/traders"

    # Determine build subdirectory (prefer Debug if exists)
    local BUILD_SUBDIR="build_x64/all"
    if [ -d "$BUILD_DIR/build_x64/Debug" ]; then
        BUILD_SUBDIR="build_x64/Debug"
    fi

    # Copy strategy-specific libraries to strategy subdirectory
    IFS=';' read -ra LIBS <<< "$STRATEGY_LIBS"
    for lib in "${LIBS[@]}"; do
        local LIB_PATH=$(find "$BUILD_DIR/$BUILD_SUBDIR/bin" -name "$lib" -type f 2>/dev/null | head -1)
        if [ -n "$LIB_PATH" ]; then
            cp -fv "$LIB_PATH" "$TARGET_DIR/$SUBDIR/"
        fi
    done

    # Create config files if not exist
    [ ! -f "$TARGET_DIR/config.yaml" ] && create_config "$STRATEGY" "$TARGET_DIR"
    
    # Update config file paths to use new directory structure
    update_config_paths "$TARGET_DIR"

    # Create start script that uses shared binaries
    cat > "$TARGET_DIR/start.sh" << 'STARTSCRIPT'
#!/bin/bash
# WonderTrader Strategy Start Script
# Uses shared binaries from ../bin

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
BIN_DIR="$(dirname "$SCRIPT_DIR")/bin"
COMMON_DIR="$(dirname "$SCRIPT_DIR")/common"

# Set library path to include shared bin directory
export LD_LIBRARY_PATH="$BIN_DIR:$BIN_DIR/parsers:$BIN_DIR/traders:$SCRIPT_DIR/$STRAT_SUBDIR:${LD_LIBRARY_PATH}"

# Set common data path for config
export WT_COMMON="$COMMON_DIR"

cd "$SCRIPT_DIR"

# Check if executable exists
if [ ! -x "$BIN_DIR/STRAT_EXEC" ]; then
    echo "Error: Executable not found: $BIN_DIR/STRAT_EXEC"
    echo "Please run: ./manage_linux.sh deploy STRAT_NAME"
    exit 1
fi

echo "Starting strategy from $SCRIPT_DIR"
echo "Using binaries from $BIN_DIR"
exec "$BIN_DIR/STRAT_EXEC" ./config.yaml
STARTSCRIPT

    # Replace placeholders with actual values
    sed -i "s|STRAT_EXEC|$EXECUTABLE|g" "$TARGET_DIR/start.sh"
    sed -i "s|STRAT_SUBDIR|$SUBDIR|g" "$TARGET_DIR/start.sh"
    sed -i "s|STRAT_NAME|$STRATEGY|g" "$TARGET_DIR/start.sh"
    chmod +x "$TARGET_DIR/start.sh"

    log_info "Strategy $STRATEGY deployed to $TARGET_DIR"
    log_info "  - Config files: $TARGET_DIR/"
    log_info "  - Strategy libs: $TARGET_DIR/$SUBDIR/"
    log_info "  - Shared binaries: $DIST_DIR/bin/"
    log_info "  - Common data: $DIST_DIR/common/"
}

create_config() {
    local STRATEGY=$1
    local TARGET_DIR=$2

    # Use WtFutuCore config template if available
    if [[ "$STRATEGY" == "futu" ]] && [ -f "$SRC_DIR/WtFutuCore/config/uft_futu_config.yaml" ]; then
        cp "$SRC_DIR/WtFutuCore/config/uft_futu_config.yaml" "$TARGET_DIR/config.yaml"
    else
        local STRAT_TYPE="cta"
        [[ "$STRATEGY" == "uft" ]] && STRAT_TYPE="uft"
        [[ "$STRATEGY" == "hft" ]] && STRAT_TYPE="hft"

        # 使用 ../common 指向共享的 common 目录
        cat > "$TARGET_DIR/config.yaml" << EOF
basefiles:
  session: ../common/sessions.json
  commodity: ../common/commodities.json
  contract: ../common/contracts.json
  holiday: ../common/holidays.json

parsers: mdparsers.yaml
traders: tdtraders.yaml
bspolicy: actpolicy.yaml

env:
  name: $STRAT_TYPE
  product:
    session: TRADING
EOF
    fi

    # Create helper configs with correct module names
    # Note: symlinks parsers -> ../bin/parsers and traders -> ../bin/traders will resolve paths
    [ ! -f "$TARGET_DIR/mdparsers.yaml" ] && cat > "$TARGET_DIR/mdparsers.yaml" << 'EOF'
parsers:
  - active: true
    id: parser_ctp
    module: ParserCTP
    front: tcp://180.168.146.187:10111
    userid: "your_userid"
    password: "your_password"
    brokerid: "9999"
EOF

    [ ! -f "$TARGET_DIR/tdtraders.yaml" ] && cat > "$TARGET_DIR/tdtraders.yaml" << 'EOF'
traders:
  - active: true
    id: trader_ctp
    module: TraderCTP
    savedata: true
    riskmon:
      active: true
      policy:
        default:
          cancel_stat_timespan: 10
          cancel_times_boundary: 20
          cancel_total_limits: 470
          order_stat_timespan: 10
          order_times_boundary: 20
    front: tcp://180.168.146.187:10101
    broker: '9999'
    appid: simnow_client_test
    authcode: '0000000000000000'
    user: "your_userid"
    pass: "your_password"
    quick: true
EOF

    [ ! -f "$TARGET_DIR/logcfg.yaml" ] && cat > "$TARGET_DIR/logcfg.yaml" << 'EOF'
root:
  level: debug
  appenders:
    - type: console
      level: info
    - type: file
      level: debug
      pattern: "./Logs/%Y%M%D.log"
EOF

    [ ! -f "$TARGET_DIR/actpolicy.yaml" ] && echo 'default: {limit: {action: open}}' > "$TARGET_DIR/actpolicy.yaml"

    log_info "Config files created"
}

# =============================================================================
# 更新配置文件路径
# 将旧目录结构的路径更新为新目录结构
# =============================================================================
update_config_paths() {
    local TARGET_DIR=$1
    local CONFIG_FILE="$TARGET_DIR/config.yaml"
    
    if [ -f "$CONFIG_FILE" ]; then
        log_info "Updating config paths in $CONFIG_FILE"
        
        # Backup original config
        cp "$CONFIG_FILE" "$CONFIG_FILE.bak"
        
        # Update common paths: ./common/ -> ../common/
        sed -i 's|: ./common/|: ../common/|g' "$CONFIG_FILE"
        sed -i 's|: ./common/|: ../common/|g' "$CONFIG_FILE"
        
        # Update module paths - use just module name (symlinks will resolve the path)
        # module: ../bin/parsers/ParserCTP -> module: ParserCTP
        sed -i 's|module: \.\./bin/parsers/|module: |g' "$CONFIG_FILE"
        sed -i 's|module: \./parsers/|module: |g' "$CONFIG_FILE"
        sed -i 's|module: parsers/|module: |g' "$CONFIG_FILE"
        
        # Update trader module paths
        sed -i 's|module: \.\./bin/traders/|module: |g' "$CONFIG_FILE"
        sed -i 's|module: \./traders/|module: |g' "$CONFIG_FILE"
        sed -i 's|module: traders/|module: |g' "$CONFIG_FILE"
        
        # Remove .so suffix if present (wrap_module will add it)
        sed -i 's|module: \(Parser[A-Za-z]*\)\.so|module: \1|g' "$CONFIG_FILE"
        sed -i 's|module: \(Trader[A-Za-z]*\)\.so|module: \1|g' "$CONFIG_FILE"
        
        # Update bspolicy path
        sed -i 's|bspolicy: ./common/|bspolicy: ../common/|g' "$CONFIG_FILE"
        
        log_info "Config paths updated"
    fi
}

start_strategy() {
    local STRATEGY=$1

    if [[ -z "${STRATEGY_CONFIG[$STRATEGY]}" ]]; then
        log_error "Unknown strategy: $STRATEGY"
        exit 1
    fi

    IFS='|' read -r DEPLOY_DIR EXECUTABLE _ SUBDIR <<< "${STRATEGY_CONFIG[$STRATEGY]}"
    local TARGET_DIR="$DIST_DIR/$DEPLOY_DIR"
    local BIN_DIR="$DIST_DIR/bin"

    if [ ! -d "$TARGET_DIR" ]; then
        log_error "Strategy directory not found: $TARGET_DIR"
        log_info "Run './manage_linux.sh deploy $STRATEGY' first."
        exit 1
    fi

    if [ ! -x "$BIN_DIR/$EXECUTABLE" ]; then
        log_error "Executable not found: $BIN_DIR/$EXECUTABLE"
        log_info "Run './manage_linux.sh deploy $STRATEGY' to deploy common files."
        exit 1
    fi

    # Set library path: bin -> bin/parsers -> bin/traders -> strategy subdir
    export LD_LIBRARY_PATH="$BIN_DIR:$BIN_DIR/parsers:$BIN_DIR/traders:$TARGET_DIR/$SUBDIR:${LD_LIBRARY_PATH}"
    export WT_COMMON="$DIST_DIR/common"

    cd "$TARGET_DIR"

    log_info "Starting $STRATEGY from $TARGET_DIR"
    log_info "Using executable: $BIN_DIR/$EXECUTABLE"
    exec "$BIN_DIR/$EXECUTABLE" ./config.yaml
}

print_usage() {
    echo "Usage: $0 <command> [options]"
    echo ""
    echo "Commands:"
    echo "  build [Debug|Release]              - Build all targets"
    echo "  build all [Debug|Release]          - Build all targets (explicit)"
    echo "  build <strategy> [Debug|Release]   - Build specific strategy"
    echo "  deploy-common                      - Deploy shared files to dist/bin"
    echo "  deploy <strategy>                  - Deploy strategy (includes deploy-common)"
    echo "  start <strategy>                   - Start strategy"
    echo "  stop <strategy>                    - Stop strategy"
    echo "  all <strategy> [Debug|Release]     - Build + Deploy + Start"
    echo ""
    echo "Strategies: cta, hft, uft, futu, opt"
    echo ""
    echo "Examples:"
    echo "  $0 build all Debug                 - Build all with debug symbols"
    echo "  $0 build futu Release              - Build futu strategy in release mode"
    echo "  $0 deploy futu                     - Deploy futu strategy"
    echo "  $0 start futu                      - Start futu strategy"
    echo "  $0 all futu Debug                  - Build, deploy and start futu (debug)"
    echo ""
    echo "Directory Structure:"
    echo "  dist/bin/              - Shared executables and libraries"
    echo "  dist/bin/parsers/      - Parser modules (libParserCTP.so, etc.)"
    echo "  dist/bin/traders/      - Trader modules (libTraderCTP.so, etc.)"
    echo "  dist/common/           - Shared configuration data (sessions, contracts, etc.)"
    echo "  dist/WtRunnerXxx/      - Strategy directory"
    echo "    ├── config.yaml      - Strategy configuration"
    echo "    ├── parsers -> ../bin/parsers/   (symlink)"
    echo "    ├── traders -> ../bin/traders/   (symlink)"
    echo "    ├── uft/             - Strategy-specific libraries"
    echo "    └── Logs/            - Runtime logs"
    echo ""
    echo "VSCode Debug:"
    echo "  1. Build with Debug:  $0 build futu Debug"
    echo "  2. Deploy:            $0 deploy futu"
    echo "  3. Press F5 in VSCode to start debugging"
}

stop_strategy() {
    local STRATEGY=$1

    if [[ -z "${STRATEGY_CONFIG[$STRATEGY]}" ]]; then
        log_error "Unknown strategy: $STRATEGY"
        exit 1
    fi

    IFS='|' read -r DEPLOY_DIR EXECUTABLE _ _ <<< "${STRATEGY_CONFIG[$STRATEGY]}"
    local TARGET_DIR="$DIST_DIR/$DEPLOY_DIR"

    # Match process by strategy directory path
    local PID=$(pgrep -f "$TARGET_DIR.*config.yaml" 2>/dev/null || true)
    if [ -z "$PID" ]; then
        PID=$(pgrep -f "$EXECUTABLE.*$TARGET_DIR" 2>/dev/null || true)
    fi

    if [ -n "$PID" ]; then
        log_info "Stopping $STRATEGY ($EXECUTABLE, PID: $PID)..."
        kill $PID 2>/dev/null || true
        sleep 1
        if pgrep -f "$TARGET_DIR" >/dev/null 2>&1; then
            log_warn "Force killing..."
            pkill -9 -f "$TARGET_DIR" 2>/dev/null || true
        fi
        log_info "$STRATEGY stopped."
    else
        log_info "$STRATEGY is not running."
    fi
}

case "${1:-}" in
    build)
        if [[ -n "${2:-}" ]] && [[ -n "${STRATEGY_CONFIG[$2]:-}" ]]; then
            # build <strategy> [Debug|Release]
            build_strategy "$2" "${3:-Release}"
        elif [[ "${2:-}" == "all" ]]; then
            # build all [Debug|Release]
            build_all "${3:-Release}"
        else
            # build [Debug|Release]
            build_all "${2:-Release}"
        fi
        ;;
    deploy-common)
        mkdir -p "$BUILD_DIR"
        [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && cmake -S "$SRC_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
        deploy_common
        ;;
    deploy)
        [[ -z "${2:-}" ]] && { log_error "Missing strategy"; print_usage; exit 1; }
        deploy_strategy "$2"
        ;;
    start)
        [[ -z "${2:-}" ]] && { log_error "Missing strategy"; print_usage; exit 1; }
        start_strategy "$2"
        ;;
    stop)
        [[ -z "${2:-}" ]] && { log_error "Missing strategy"; print_usage; exit 1; }
        stop_strategy "$2"
        ;;
    all)
        [[ -z "${2:-}" ]] && { log_error "Missing strategy"; print_usage; exit 1; }
        build_strategy "$2" "${3:-Release}"
        deploy_strategy "$2"
        start_strategy "$2"
        ;;
    -h|--help)
        print_usage
        ;;
    *)
        print_usage
        exit 1
        ;;
esac
