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

#include "../ApplicationDeps.h"  // 19.33: 统一聚合 (成员 _deps 为按值切片)
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <cstdint>
#include "../../Includes/FasterDefs.h"
#include "SpinLockGuard.h"

namespace wtp
{
class IUftStraCtx;
class WTSTickData;
} // namespace wtp

namespace futu
{


class ArbExecutionBridge
{
public:
    /// 依赖注入 (init 时设置一次)

    // 19.33: 依赖统一走 ApplicationDeps 按值切片 (原嵌套 Deps 删除)
    void setDeps(const ApplicationDeps& deps)
    {
        _deps = deps;
        _deps.validate("ArbExecutionBridge");
    }

    /// 每 tick 主线程驱动 (原 processSpreadArbitrage 全部内容)
    /// 19.35: now_ms 由 on_tick 主入口单一转换后经参数下发 (不再自转换)
    void onTick(wtp::IUftStraCtx* ctx, const char* stdCode, wtp::WTSTickData* tick, uint64_t now_ms);

    /// 成交回报中的套利单处理 (consumePairTag → in_flight 递减 + 残腿对冲)
    void
    onTradeFill(wtp::IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isLong, double vol, double price);

    /// A2/A3: 单腿被拒(broker拒单/流控/STP)时标记残腿防护.
    /// order_qty = 预期裸腿上限; 传 0 表示上限未知 (对冲该 pair 所有后续成交, session 末清理).
    /// V10-P2-6: leg_code 非空时经 Manager::convertLegQtyToOpposite 换算为对侧腿
    /// 计划对冲手数 (ratio 折算) —— 原实现直接用失败腿自身手数封顶, ratio≠1 时
    /// 对侧对冲量错配; 换算失败 (未知 pair/腿) 回退 order_qty 原值。
    void markLegRejected(
        const std::string& pair_id, double order_qty, const std::string& leg_code = "");

    /// A4: 套利腿撤单回报处理 — 撤对侧在途单 + 标记残腿防护 + 释放 in_flight.
    /// 覆盖 "leg1 成交 + leg2 被撤(超时/交易所撤单)" 场景, 此前该分支无任何残腿处理.
    void onLegCancelled(wtp::IUftStraCtx* ctx, const std::string& pair_id);

    /// session_begin 复位
    void resetSession();

private:
    ApplicationDeps _deps;  // 19.33: 按值切片 (消费点 _deps.xxx 零改动)

    // 同价去重: 上次套利挂单价 (防 WT 底层自动撤单重下, 保护排队优先级)
    // v7.6 阶段2: 递归自旋锁 — onTick(MdSpi) vs onTradeFill/onLegCancelled(TdSpi)
    mutable RecursiveSpinLock _lock;

    wtp::wt_hashmap<std::string, double> _arb_last_order_price;
    // A3: 残腿对冲状态 (pair_id → 预期上限/已对冲量).
    // 旧实现为 unordered_set<pair_id>, 首笔成交即 erase → 分笔成交时后续裸腿残留.
    struct CloseHedgeState
    {
        double original_qty = 0;
        double hedged_qty = 0;         ///< 已提交对冲量(受理口径, 仅日志)
        double filled_hedged = 0;      ///< 19.30-A1: 已成交核验对冲量(完成判据)
        uint64_t created_time_ms = 0; ///< B5 fix: for stale entry cleanup
    };
    std::unordered_map<std::string, CloseHedgeState> _arb_hedge_on_fill;
    /// 19.30-A1: 对冲单 localid → pair 反查表 (成交回报核验; 调用方须持 _lock)
    std::unordered_map<uint32_t, std::string> _hedge_fill_index;

    /// 19.30-A1: 登记对冲单 localids (调用方须持 _lock)
    void noteHedgeOrderLocked(const std::string& pair_id, const std::vector<uint32_t>& localids);
    /// 19.30-A1: 对冲单成交回报核验; true=该 fill 已被消费 (调用方须持 _lock)
    bool onHedgeFillLocked(uint32_t localid, double vol);
    /// 19.30-A1: 清理某 pair 的全部对冲单跟踪 (调用方须持 _lock)
    void cleanupHedgeTrackingLocked(const std::string& pair_id);
    // MM 订单快照世代号 (增量同步到 arb 执行器)
    uint64_t _last_mm_generation = 0;

    // v7.1: replay 时钟 (onTick 注入; 超时/时间戳统一时间基准, 0=回退墙钟)
    // V10-P2-11: 原子化 —— MdSpi onTick 写 / TdSpi onTradeFill·markLegRejected 读,
    // 此前为明域标量形式数据竞争 (x86-64 实际原子, 按项目定级标准仍待收编)。
    // 残余语义注记: 首帧前的墙钟 bootstrap 与 exchange 域差 ~1000×, 最坏后果 =
    // 首帧前创建的对冲条目在首帧后被判"立即过期"提前清理 (warn 告警, 无簿记损坏)。
    std::atomic<uint64_t> _now_ms{0};

    /// V10-P2-11: 统一时间读取 (relaxed load; 0=尚未收到首 tick)
    inline uint64_t nowMs() const { return _now_ms.load(std::memory_order_relaxed); }
};

} // namespace futu
