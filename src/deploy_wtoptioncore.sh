#!/bin/bash
# deploy_wtoptioncore.sh
# Copies compiled WtOptionCore library and config to the execution environment

# Source dir
SRC_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
# Built library path
BUILD_DIR="$SRC_DIR/build_wtoptioncore/WtOptionCore"
# Target run dir
RUN_DIR="$SRC_DIR/../run"

mkdir -p "$RUN_DIR/strategies"
mkdir -p "$RUN_DIR/config"
mkdir -p "$RUN_DIR/log"
mkdir -p "$RUN_DIR/data"

echo "Deploying libWtOptionCore.so..."
if [ -f "$BUILD_DIR/libWtOptionCore.so" ]; then
    cp "$BUILD_DIR/libWtOptionCore.so" "$RUN_DIR/strategies/"
    echo "Successfully deployed to $RUN_DIR/strategies/libWtOptionCore.so"
else
    echo "Warning: libWtOptionCore.so not found! You might need to build it first."
fi

echo "Copying example config file..."
if [ -f "$SRC_DIR/WtOptionCore/config/option_config_demo.json" ]; then
    cp "$SRC_DIR/WtOptionCore/config/option_config_demo.json" "$RUN_DIR/config/option_strategy.json"
    echo "Successfully copied config to $RUN_DIR/config/option_strategy.json"
else
    echo "Warning: option_config_demo.json not found!"
fi

echo "Deployment complete."
