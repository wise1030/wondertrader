/*!
 * \file ArbExecutionBridge.h
 * \brief 套利执行桥 (从 UftFutuMmStrategy 拆分的套利执行编排职责)
 *
 * 设计目的:
 *   集中管理主线程侧的套利执行编排:
 *     - tick 推送 (SPSC ~50ns)
 *     - Portfolio(SSOT) 仓位回填
 *     - MM 订单快照世代号增量同步 (自成交检测)
 *     - 订单请求回调执行 (同价去重 / OrderRouter 下单 / pair 标记 / 拒单反撤)
 *     - orphan leg 自动对冲
 *     - in_flight 超时清理
 *     - 残腿防护 (单腿被拒标记 → 对侧腿成交即反向平仓)
 */
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "../Includes/FasterDefs.h"

namespace wtp {
class IUftStraCtx;
class WTSTickData;
}

namespace futu {

class AsyncArbitrageExecutor;
class SpreadArbitrageManager;
class OrderRouter;
class UnifiedOrderTracker;
class SelfTradePrevention;
class FutuPortfolio;
class FutuRiskMonitor;
struct ContractInfo;

class ArbExecutionBridge
{
public:
    /// 依赖注入 (init 时设置一次)
    struct Deps {
        AsyncArbitrageExecutor* async_arb = nullptr;
        SpreadArbitrageManager* arb_manager = nullptr;
        OrderRouter* order_router = nullptr;
        UnifiedOrderTracker* order_tracker = nullptr;
        SelfTradePrevention* stp = nullptr;
        FutuPortfolio* portfolio = nullptr;
        FutuRiskMonitor* risk_monitor = nullptr;
        const std::vector<ContractInfo>* contract_infos = nullptr;
        bool use_spread_arbitrage = false;
        const char* strategy_id = "";
    };

    void setDeps(const Deps& deps) { _deps = deps; }

    /// 每 tick 主线程驱动 (原 processSpreadArbitrage 全部内容)
    void onTick(wtp::IUftStraCtx* ctx, const char* stdCode, wtp::WTSTickData* tick);

    /// 成交回报中的套利单处理 (consumePairTag → in_flight 递减 + 残腿对冲)
    void onTradeFill(wtp::IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
                     bool isLong, double vol, double price);

    /// A2/A3: 单腿被拒(broker拒单/流控/STP)时标记残腿防护.
    /// order_qty = 预期裸腿上限; 传 0 表示上限未知 (对冲该 pair 所有后续成交, session 末清理).
    void markLegRejected(const std::string& pair_id, double order_qty);

    /// A4: 套利腿撤单回报处理 — 撤对侧在途单 + 标记残腿防护 + 释放 in_flight.
    /// 覆盖 "leg1 成交 + leg2 被撤(超时/交易所撤单)" 场景, 此前该分支无任何残腿处理.
    void onLegCancelled(wtp::IUftStraCtx* ctx, const std::string& pair_id);

    /// session_begin 复位
    void resetSession();

private:
    Deps _deps;

    // 同价去重: 上次套利挂单价 (防 WT 底层自动撤单重下, 保护排队优先级)
    wtp::wt_hashmap<std::string, double> _arb_last_order_price;
    // A3: 残腿对冲状态 (pair_id → 预期上限/已对冲量).
    // 旧实现为 unordered_set<pair_id>, 首笔成交即 erase → 分笔成交时后续裸腿残留.
    struct CloseHedgeState {
        double original_qty = 0;
        double hedged_qty = 0;
        uint64_t created_time_ms = 0;  ///< B5 fix: for stale entry cleanup
    };
    std::unordered_map<std::string, CloseHedgeState> _arb_hedge_on_fill;
    // MM 订单快照世代号 (增量同步到 arb 执行器)
    uint64_t _last_mm_generation = 0;

    // v7.1: replay 时钟 (onTick 注入; 超时/时间戳统一时间基准, 0=回退墙钟)
    uint64_t _now_ms = 0;
};

} // namespace futu
