/*!
 * \file FutuConfigMap.h
 * \brief 策略配置 → 协调器配置 的**唯一映射点** (F-core P1-4.1b, 2026-09-19)
 *
 * 背景（历史实证）:
 *   - 该映射原先以 11 行内联赋值散落在 FutuModuleAssembler::assemble 内, 新增跨结构字段时
 *     极易漏改（`alertThrottleMs` 需 5 处编辑; `perf` 阈值曾"注入后无逻辑读取"V8-R3;
 *     `setMinProfitThreshold` 曾无调用者 A10）。
 *   - 收敛为单一函数后: 新增字段只改此处 + 在 test_futu_config_map.cpp 补一条逐字段断言,
 *     由测试把"加了键忘了桥"变成编译期/CI 可见的失败, 而非运行期静默用默认值。
 *
 * 注意: 本函数只做**值搬运**, 不含任何业务判定; 覆盖语义 = config.yaml 的策略层设置
 *   对 coordinator.yaml 的最终覆盖 (final override), 与改动前逐字段等价。
 */
#pragma once

#include <cstddef>

namespace futu
{

struct FutuMmConfig;        // UftFutuMmStrategy.h
struct CoordinatorConfig;   // StrategyCoordinator.h

class FutuConfigMap
{
public:
    /// 把策略层 (config.yaml) 的覆盖项写入协调器配置。
    /// @return 实际写入的字段数 (测试据此断言映射完整性)
    static size_t applyStrategyOverrides(CoordinatorConfig& coord, const FutuMmConfig& cfg);
};

} // namespace futu
