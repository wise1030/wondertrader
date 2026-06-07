/*!
 * \file FutuConfig.h
 * \brief 配置工具方法
 *
 * 提供从 WTSVariant 读取配置值的静态工具方法。
 * 实际配置结构体定义在各模块头文件中：
 *   - FutuMmConfig (UftFutuMmStrategy.h)
 *   - CoordinatorConfig / ModuleParams (StrategyCoordinator.h)
 *   - GLFTParams (SpreadOptimizer.h)
 *   - ToxicityParams (ToxicFlowDetector.h)
 *   - SignalAggregatorConfig (SignalAggregator.h)
 *   - 等等
 */
#pragma once

#include <string>
#include <cstdint>
#include "../Includes/WTSVariant.hpp"

namespace futu {

class FutuConfig
{
public:
    static double readDouble(wtp::WTSVariant* cfg, const char* key, double defVal);
    static uint32_t readUInt32(wtp::WTSVariant* cfg, const char* key, uint32_t defVal);
    static bool readBool(wtp::WTSVariant* cfg, const char* key, bool defVal);
    static std::string readString(wtp::WTSVariant* cfg, const char* key, const char* defVal);
};

} // namespace futu