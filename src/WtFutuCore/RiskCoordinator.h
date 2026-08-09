/*!
 * \file RiskCoordinator.h
 * \brief 风控协调器 (从 StrategyCoordinator 拆分, P1.3 Step 2a)
 *
 * 职责: 承载从 coordinator 错位归属迁出的风控减仓逻辑 (复核§3.2)。
 * Step 2a: checkTakerReduce (taker 减仓: util 超阈值时 FAK 对手价减仓 + 限频冷却)。
 * Step 2b 将迁入 checkRisk。
 */
#pragma once
#include <string>
#include <unordered_map>
#include <cstdint>
namespace wtp { class IUftStraCtx; }
namespace futu {
class FutuPortfolio;
class OrderRouter;
class FutuRiskMonitor;
struct CoordinatorConfig;

class RiskCoordinator
{
public:
    struct Deps
    {
        FutuPortfolio* portfolio = nullptr;
        OrderRouter* order_router = nullptr;
        FutuRiskMonitor* risk_monitor = nullptr;
        const CoordinatorConfig* cfg = nullptr;
    };
    void setDeps(const Deps& deps) { _deps = deps; }
    /// taker 减仓: util 超阈值时 FAK 对手价减仓, 每合约限频冷却.
    /// exchange_time_ms = 当前 replay 时钟 (0 = 回退墙钟). 返回是否触发.
    bool checkTakerReduce(wtp::IUftStraCtx* ctx, uint64_t exchange_time_ms);
private:
    Deps _deps;
    /// 每合约上次 taker 减仓时间 (限频冷却, 从 StrategyCoordinator 迁入)
    std::unordered_map<std::string, uint64_t> _last_taker_reduce;
};
} // namespace futu
