# =============================================================================
# WtFutuCore - 期货高频做市引擎
# =============================================================================
# 说明:
#   WtFutuCore 实现期货高频做市策略 (GLFT+Alpha 框架)
#   作为动态库被 WtUftRunner 动态加载运行
#
# 设计决策 - 动态库 vs 静态库:
#   - 选择动态库 (SHARED) 的原因:
#     1. 与其他策略工厂 (WtUftStraFact, WtHftStraFact) 保持架构一致
#     2. 支持运行时动态加载，无需重新编译主程序
#     3. 便于策略热更新和独立部署
#     4. 多进程可共享内存中的动态库代码段
#
#   - 针对低延迟的优化:
#     1. 使用 -flto (Link-Time Optimization) 进行全局优化
#     2. 关键路径函数使用 hot 属性标记
#     3. 避免动态库边界的热路径虚函数调用
#     4. 库加载是一次性开销 (~ms 级)，不影响运行时延迟
#
# 使用方式:
#   在 config.yaml 中配置:
#     strategies:
#       uft:
#         - name: FutuMM
#           params: { ... }
# =============================================================================
cmake_minimum_required(VERSION 3.0.0)

project(WtFutuCore LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Compiler flags - 与 WtUftCore 保持一致，避免 ABI 问题
# 注意: 移除 -flto -march=native 等可能导致跨库 ABI 不兼容的选项
# ============================================================================
IF(MSVC)
    # MSVC optimization flags
    SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Ob2")
ELSE()
    # GCC/Clang - 与 WtUftCore 保持一致的编译选项
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
    
    # Release mode optimizations (不使用 -flto 以避免跨库 ABI 问题)
    SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -ffast-math")
    SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -funroll-loops")
    SET(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-omit-frame-pointer")
ENDIF()

# ============================================================================
# Output directory - 输出到 WtUftRunner 目录结构中
# ============================================================================
SET(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/WtUftRunner/futu)

# Add all source files
file(GLOB SRCS *.cpp)
file(GLOB HDRS *.h)

# ============================================================================
# Include directories
# ============================================================================
INCLUDE_DIRECTORIES(
    ${INCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/..         # Parent src directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../Share   # Share utilities
    ${CMAKE_CURRENT_SOURCE_DIR}/../Includes
    ${CMAKE_CURRENT_SOURCE_DIR}/../WtUftCore
    ${CMAKE_CURRENT_SOURCE_DIR}/../WTSTools
)

LINK_DIRECTORIES(${LNKS})

# ============================================================================
# Create SHARED library (与其他策略工厂保持一致)
# ============================================================================
ADD_LIBRARY(WtFutuCore SHARED ${SRCS} ${HDRS})

# Link dependencies
TARGET_LINK_LIBRARIES(WtFutuCore
    WtUftCore        # UFT framework core
    WTSTools         # WTS utilities
    Share            # Shared utilities (TimeUtils, SpinMutex, etc.)
)

# ============================================================================
# Platform-specific settings (与 WtUftCore 保持一致)
# ============================================================================
IF (WIN32)
    # Windows specific settings
ELSE()
    # Linux/Unix settings
    TARGET_LINK_LIBRARIES(WtFutuCore
        boost_filesystem
        dl
        pthread
        atomic          # Required for std::atomic on lock-free data structures
    )
ENDIF()

# ============================================================================
# Installation
# ============================================================================
INSTALL(TARGETS WtFutuCore
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

INSTALL(FILES ${HDRS} DESTINATION include/WtFutuCore)