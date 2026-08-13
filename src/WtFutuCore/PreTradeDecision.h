/*!
 * \file PreTradeDecision.h
 * \brief Pre-trade decision types: RiskVerdict (风控闸门) + StrategyInputs (策略输入)
 *
 * 分层语义（策略与风控严格分离）:
 *   - RiskVerdict:     风控措施 (硬规则). 触发依据 = 净头寸 vs maxPosition.
 *                      回答 "能不能做": 暂停报单 / 流控撤单.
 *   - StrategyInputs:  策略调整 (软参数). 触发依据 = projected utilization (含在途).
 *                      回答 "怎么做": skew / 义务报价 / flexible block side (库存管理).
 */
#pragma once

namespace futu
{

/// 风控裁决: 硬停止 / 流控措施
/// 准则: 净头寸 (策略派记 net position) 超过 maxPosition 即触发, 不带任何倍率阈值
struct RiskVerdict
{
    /// |净头寸| > maxPosition -> 暂停该合约全部报单 (cancelAll + 不再挂新单)
    /// 最后一道防线: 策略层 skew/衰减/block side 全部失效时的硬停止.
    /// 恢复: 每 tick 重估, 净头寸回落到 maxPosition 以内自动恢复.
    /// 减仓依赖 closeout 机制或人工介入.
    bool halt_quoting = false;

    /// pending 超限 -> 流控撤该侧旧单 + 跳过本轮该侧报单
    bool pending_drain_bid = false;
    bool pending_drain_ask = false;

    /// 同侧连续成交熔断 (风控层硬闸门, 按合约独立计数)
    /// 某合约同侧连续成交达阈值 -> 暂停该合约报价
    /// (cancelAll + no new quotes), auto resume after pause expires.
    bool side_pause_bid = false;
    bool side_pause_ask = false;
};

/// 策略输入: 软调整参数, 供定价函数使用
/// 准则: projected utilization (position + 同向 pending), 前瞻性库存管理
struct StrategyInputs
{
    double long_util = 0.0;            ///< projected_long / maxPosition
    double short_util = 0.0;           ///< projected_short / maxPosition

    /// utilization >= 1.0 -> 减仓侧强制义务报价 (obligation 被动价加仓侧)
    bool force_ask_obligation = false; ///< 多仓打满 -> ask 侧义务减仓报价
    bool force_bid_obligation = false; ///< 空仓打满 -> bid 侧义务减仓报价

    /// flexible 策略决策 (库存管理): 不再加仓, qty=0
    /// 注意: 这是策略行为不是风控措施, 仅作用于 flexible 加仓侧
    bool block_add_long = false;       ///< flexible: 停止加多仓 (bid qty=0)
    bool block_add_short = false;      ///< flexible: 停止加空仓 (ask qty=0)
};

/// checkPreTradePosition 返回类型: 风控裁决 + 策略输入
struct PreTradeDecision
{
    RiskVerdict risk;        ///< 风控闸门
    StrategyInputs strategy; ///< 策略输入
};

} // namespace futu
