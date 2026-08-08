/*!
 * \file FutuRuntimeOps.h
 * \brief 5A-3 (v7.5): 运行时事件处理外移 (on_trade / on_channel_ready)
 */
#pragma once

#include <cstdint>

namespace wtp
{
class IUftStraCtx;
}

namespace futu
{

class UftFutuMmStrategy;

class FutuRuntimeOps
{
public:
    /// 成交处理 (组合记账/恢复四道闸/arb复活/毒性/统计)
    static void processTradeFill(UftFutuMmStrategy& s,
                                 wtp::IUftStraCtx* ctx,
                                 uint32_t localid,
                                 const char* stdCode,
                                 bool isLong,
                                 uint32_t offset,
                                 double vol,
                                 double price);

    /// 通道恢复序列 (持仓同步/风控恢复/AUTO REDUCE)
    static void onChannelReady(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx);

    /// 会话开始 (日内状态复位/arb线程启动/双边统计 session start)
    static void onSessionBegin(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx, uint32_t uTDate);

    /// 会话结束 (CLOSEOUT 相位/撤单/绩效报告/closeout 收尾)
    static void onSessionEnd(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx, uint32_t uTDate);

    /// 报单回执 (错误计数/恢复状态机/双边统计挂单确认)
    static void
    onEntrust(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx, uint32_t localid, bool bSuccess, const char* message);

    /// 订单状态事件 (撤单统计/quoter状态/closeout跟踪/router终结/arb残腿)
    static void onOrderEvent(UftFutuMmStrategy& s,
                             wtp::IUftStraCtx* ctx,
                             uint32_t localid,
                             const char* stdCode,
                             bool isLong,
                             uint32_t offset,
                             double totalQty,
                             double leftQty,
                             double price,
                             bool isCanceled);

    /// 通道断开 (HALT/撤单/停arb/持仓快照)
    static void onChannelLost(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx);

    /// M1/M2: 拒单统一幂等清理 (tracker REJECTED + router 活跃表)
    static void finalizeOrder(UftFutuMmStrategy& s, uint32_t localid);
};

} // namespace futu
