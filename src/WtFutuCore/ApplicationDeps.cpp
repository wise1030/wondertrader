// ApplicationDeps.cpp
// 19.33: ApplicationDeps::validate 实现 — 启动期 fail-fast。
#include "ApplicationDeps.h"

#include "../WTSTools/WTSLogger.h"

namespace futu
{

bool ApplicationDeps::validate(const char* owner) const
{
    // 核心依赖: 任何组件都不可缺 (可选模块/挂钩不在此列)
    int missing = 0;
    auto req = [&](const void* p, const char* name) {
        if (!p)
        {
            WTSLogger::error("[DEPS] {}: required dep '{}' is null (wireDeps 漏接线)", owner, name);
            ++missing;
        }
    };
    req(portfolio, "portfolio");
    req(risk_monitor, "risk_monitor");
    req(trading_state, "trading_state");
    req(order_router, "order_router");
    if (missing == 0)
        return true;
    WTSLogger::error("[DEPS] {}: validate FAILED with {} missing core deps — startup aborted", owner, missing);
    return false;
}

} // namespace futu
