/*!
 * \file RiskLiquidator.h
 * \brief P0-1 (v7.4): 统一强平/减仓执行原语
 *
 * 消灭"对手价平仓"在代码库中的多份重复实现 (coordinator HALT FORCE FLAT /
 * channel_ready AUTO REDUCE 等), 统一语义:
 *   - 对手价 (exitLong@bid1 / exitShort@ask1), 保证立即成交
 *   - flag 默认 1 (FAK), 风险处置必须当次成交, 不留挂单
 *   - Source::CLOSEOUT (走 OrderRouter 审计/统计)
 *   - 价格三级校验 (last_price/bid1/ask1 > 0), 无效价拒绝发单
 *   - qty clamp 到实际持仓, 不超卖
 *
 * 无状态服务: 每次调用前 setDeps 或持有成员长期复用均可。
 */
#pragma once

#include <string>
#include <cmath>
#include "../Includes/FasterDefs.h"
#include "../Includes/IUftStraCtx.h"
#include "../WTSTools/WTSLogger.h"
#include "FutuPortfolio.h"
#include "OrderRouter.h"

namespace futu {

class RiskLiquidator
{
public:
    struct Deps {
        OrderRouter*   router    = nullptr;
        FutuPortfolio* portfolio = nullptr;
    };

    void setDeps(const Deps& deps) { _deps = deps; }

    /// 对手价 FAK 全组合强平 (HALT IRREVERSIBLE 场景, v7.7 业务#2 修复)
    /// 旧实现 forceFlatAnchor 只平 anchor 且 qty=|组合delta|: 多合约组合下
    /// 非 anchor 净头寸敞口残留 (hedge_ratio≠1 时 anchor 手数也不准确)。
    /// 改为遍历全部合约, 逐合约按实际持仓对手价 FAK 平仓。
    /// @return 实际发单总手数
    double forceFlatAll(wtp::IUftStraCtx* ctx, const char* reason)
    {
        if (!_deps.portfolio)
            return 0;
        double total = 0;
        for (const auto& c : _deps.portfolio->getAllContractsSnapshot())
        {
            if (std::abs(c.position) > 0.01)
                total += reduceContract(ctx, c.code, std::abs(c.position), 1, reason);
        }
        return total;
    }

    /// 对手价减仓 code 合约 qty 手 (方向自动: 持仓>0→exitLong@bid1, <0→exitShort@ask1)
    /// @param flag 报单标志 (0=限价, 1=FAK); 风险处置建议 1
    /// @return 实际发单手数
    double reduceContract(wtp::IUftStraCtx* ctx, const std::string& code,
                          double qty, int flag, const char* reason)
    {
        if (!_deps.router || !_deps.portfolio || qty <= 0)
            return 0;
        ContractState cs_buf;
        const ContractState* cs = _deps.portfolio->getContractSnapshot(code, cs_buf) ? &cs_buf : nullptr;
        if (!cs || std::abs(cs->position) <= 0.01)
            return 0;
        if (cs->last_price <= 0 || cs->bid1 <= 0 || cs->ask1 <= 0)
        {
            WTSLogger::error("[LIQUIDATE] {} skip: invalid price last={} bid={} ask={} ({})",
                code, cs->last_price, cs->bid1, cs->ask1, reason);
            return 0;
        }

        double exec_qty = std::min(qty, std::abs(cs->position));
        if (cs->position > 0)
        {
            _deps.router->submitSell(ctx, code.c_str(), cs->bid1, exec_qty,
                Source::CLOSEOUT, flag);
            WTSLogger::error("[LIQUIDATE] SELL_CLOSE {} x{:.0f} @ {:.1f} ({})",
                code, exec_qty, cs->bid1, reason);
        }
        else
        {
            _deps.router->submitBuy(ctx, code.c_str(), cs->ask1, exec_qty,
                Source::CLOSEOUT, flag);
            WTSLogger::error("[LIQUIDATE] BUY_CLOSE {} x{:.0f} @ {:.1f} ({})",
                code, exec_qty, cs->ask1, reason);
        }
        return exec_qty;
    }

private:
    Deps _deps;
};

} // namespace futu
