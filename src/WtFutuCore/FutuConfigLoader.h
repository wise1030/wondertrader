/*!
 * \file FutuConfigLoader.h
 * \brief 策略配置加载器 (从 UftFutuMmStrategy 拆分的配置解析职责)
 *
 * 设计目的:
 *   将 config.yaml 的解析、边界校验、合约列表加载从策略入口类剥离。
 *   纯静态方法, 无状态, 输出到 FutuMmConfig + ContractInfo 列表。
 */
#pragma once

#include "UftFutuMmStrategy.h"

namespace futu
{

class FutuConfigLoader
{
public:
    /// 从主配置解析 FutuMmConfig + 合约列表, 含边界校验
    /// @param cfg      config.yaml 根节点
    /// @param config   输出: 策略配置
    /// @param contracts 输出: 合约列表
    /// @param strategy_id 用于日志标识
    /// @return true=解析与校验通过
    static bool
    load(wtp::WTSVariant* cfg, FutuMmConfig& config, std::vector<ContractInfo>& contracts, const char* strategy_id);
};

} // namespace futu
