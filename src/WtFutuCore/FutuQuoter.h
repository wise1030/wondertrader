/*!
 * \file FutuQuoter.h
 * \brief Multi-level bilateral quoting engine for futures market making
 *
 * Manages multiple price levels of bids/asks per contract, with:
 *   - Configurable number of levels and per-level quantity
 *   - Auto-adjustment of widths based on portfolio risk (via FutuPortfolio)
 *   - Efficient O(1) cancel-and-replace for quote refreshes
 *   - Unified order tracking with AutoCancelPolicy via UnifiedOrderTracker
 *
 * Designed for inline synchronous use within on_tick callback.
 *
 * Performance optimizations:
 *   - Uses UnifiedOrderTracker for zero-copy order state access
 *   - Pre-allocated level vectors
 *   - Inline price computation
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include "../Includes/ExecuteDefs.h"
#include "../Includes/WTSMarcos.h"
#include "../Includes/FasterDefs.h"
#include "SpinLockGuard.h"
#include "BilateralQuoteStats.h"
#include "PreTradeDecision.h"

NS_WTP_BEGIN
class IUftStraCtx;
class WTSSessionInfo;
NS_WTP_END

namespace futu
{

class FutuPortfolio;
class UnifiedOrderTracker;
enum class CancelReason : uint8_t; // 前置声明 (定义在 UnifiedOrderTracker.h)

/// A single price level in the quote ladder (lightweight, no order state)
struct QuoteLevel
{
    double price;                    ///< Current quote price
    double qty;                      ///< Current quote quantity
    std::vector<uint32_t> order_ids; ///< Active order IDs (stra_buy/sell may return multiple)
    uint8_t level_index;             ///< Level index for O(1) lookup
    bool is_bid;                     ///< true=bid, false=ask

    QuoteLevel() : price(0), qty(0), level_index(0), is_bid(true) {}

    /// Check if any active orders
    bool hasOrders() const { return !order_ids.empty(); }
};

/// Configuration for multi-level quoting on a single contract
struct QuoterConfig
{
    std::string code;    ///< Contract code
    uint32_t num_levels; ///< Number of price levels per side (e.g., 3 = 3 bids + 3 asks)
    double base_spread;  ///< Base spread in ticks (per side, from mid)
    double level_step;   ///< Additional tick step between levels
    double base_qty;     ///< 义务层挂单手数；scout/flexible 层由 scoutQty/levelQtyMultiplier 派生
    double
        level_qty_multiplier; ///< M2: 档位间数量几何衰减系数 (e.g. 0.7 = 每档 ×0.7; 此前叫 qty_decay 易与 qty_decay_factor 混淆)
    double tick_size; ///< Minimum price increment

    // Sticky 策略参数
    double sticky_threshold; ///< Price stickiness threshold in ticks
    double improve_retreat_ratio; ///< Ratio for asymmetric sticky: improve price容忍更大, retreat更敏感 (default: 2.0)

    // 价格验证参数
    double max_price_deviation; ///< Max allowed price deviation from mid (in ticks), 0 = no limit

    // 做市报价价格保护参数
    // 防止报价穿过市场最优价格：bid不能超过最优买价+protect_ticks，ask不能低于最优卖价-protect_ticks
    bool price_protection; ///< 是否启用价格保护 (default: true)
    double protect_ticks;  ///< 价格保护tick数 (default: 1.0)

    // 双边报价配置
    bool use_bilateral_quote; ///< 是否使用双边报价接口 stra_quote() (default: false)
    double min_valid_qty;     ///< 全侧总深度有效阈值，用于统计/重挂 (default: 1.0)

    // v3 软风控参数（use_bilateral_quote=false 路径专用，bilateral 路径不受影响）
    double qty_decay_factor;   ///< qty 指数衰减因子 (default: 2.0)，bidQty *= exp(-factor * long_delta_util)
    double obligation_min_qty; ///< 全侧总深度义务阈值 (default: 10); 不直接决定单档报单量
    double
        obligation_max_spread_ticks; ///< 软 obligation 最大报价宽度 ticks (default: 10) — 同时用于报价生成和双边统计判断 (统一, 挂在哪=统计到哪)
    // v7.2 scout 多层结构: 自由探测层(level<obligation_level)居最优价小qty,
    //   义务层退居 obligation_level 档; scout 成交即撤同侧义务层防大单逆向成交
    uint32_t obligation_level; ///< 义务层所在档位 (default: 0=最优价层, 向后兼容)
    double scout_qty;          ///< scout/自由探测层手数 (default: 1.0, 应小于义务总深度)

    QuoterConfig()
        : num_levels(1), base_spread(2.0), level_step(1.0), base_qty(5.0), level_qty_multiplier(0.7), tick_size(1.0),
          sticky_threshold(1.0), improve_retreat_ratio(2.0), max_price_deviation(20.0), price_protection(true),
          protect_ticks(1.0), use_bilateral_quote(false), min_valid_qty(1.0), qty_decay_factor(2.0),
          obligation_min_qty(10.0), obligation_max_spread_ticks(10.0), obligation_level(0), scout_qty(1.0)
    {}
};

/// Multi-level quoter for a single contract
class FutuQuoter
{
public:
    FutuQuoter();
    ~FutuQuoter() {}

    /// Initialize with config
    void init(const QuoterConfig& cfg);

    /// Set shared order tracker (must be called before refreshQuotes)
    void setOrderTracker(UnifiedOrderTracker* tracker) { _tracker = tracker; }
    UnifiedOrderTracker* getOrderTracker() const { return _tracker; }

    /// Get configuration
    const QuoterConfig& config() const { return _cfg; }
    void updateQuotingParams(double base_spread, double base_qty, double level_qty_multiplier, double level_step)
    {
        RecursiveSpinGuard _g(_lock);
        _cfg.base_spread = base_spread;
        _cfg.base_qty = base_qty;
        _cfg.level_qty_multiplier = level_qty_multiplier;
        _cfg.level_step = level_step;
        // 重算预计算数量表 — init() 中 _level_qtys 由 base_qty*level_qty_multiplier^i 预计算,
        // 旧代码只改 _cfg 不刷表, 热更新对实际下单量无效.
        for (uint32_t i = 0; i < _level_qtys.size(); i++) {
            double qty = _cfg.base_qty * std::pow(_cfg.level_qty_multiplier, i);
            _level_qtys[i] = std::max(1.0, std::round(qty));
        }
    }
    void updateStickyParams(double sticky_threshold, double improve_retreat_ratio)
    {
        RecursiveSpinGuard _g(_lock);
        _cfg.sticky_threshold = sticky_threshold;
        _cfg.improve_retreat_ratio = improve_retreat_ratio;
    }
    void updateProtectionParams(bool price_protection, double protect_ticks)
    {
        RecursiveSpinGuard _g(_lock);
        _cfg.price_protection = price_protection;
        _cfg.protect_ticks = protect_ticks;
    }
    void updateMaxPriceDeviation(double max_price_deviation)
    {
        RecursiveSpinGuard _g(_lock);
        _cfg.max_price_deviation = max_price_deviation;
    }

    //==========================================================================
    // Quote management (called within on_tick, all synchronous)
    //==========================================================================

    /// Refresh all quote levels based on current market data
    /// @param ctx     Strategy context for placing/cancelling orders
    /// @param req     Quote request bundling mid/skew/prices, risk verdict and strategy inputs
    /// @return        Number of new orders placed (for rate limiting)
    /// B8: QuoteRequest - replaces 18 individual refreshQuotes parameters
    struct QuoteRequest
    {
        double mid;
        double l0_bid_price;
        double l0_ask_price;
        double spread_mult = 1.0;
        bool allow_bid = true;
        bool allow_ask = true;
        uint64_t now = 0;
        double upper_limit = 0;
        double lower_limit = 0;
        double best_bid = 0;
        double best_ask = 0;
        RiskVerdict verdict;      ///< 风控闸门 (halt/drain) - 决定能不能做
        StrategyInputs strategy;  ///< 策略输入 (util/obligation/block_add) - 决定怎么做
    };

    uint32_t refreshQuotes(wtp::IUftStraCtx* ctx, const QuoteRequest& req);

    /// Cancel all outstanding quotes (both sides)
    void cancelAll(wtp::IUftStraCtx* ctx);

    /// Cancel all outstanding quotes on one side only
    /// @param cancel_bid  true=cancel all bid orders, false=cancel all ask orders
    void cancelSide(wtp::IUftStraCtx* ctx, bool cancel_bid);

    /// B+: 发送撤单但**保留** slot id (发送即遗忘是 2026-08-19 僵尸单事故根源)。
    /// id 在 onOrder 终态 (Cncld/全成) 时才从 slot 移除; 未确认前挂单门禁
    /// (order_ids 非空) 阻止同层挂新单, 防双份敞口。已 pendingCancel 的 id
    /// 静默跳过 (B+ 正常态, 由 tracker 超时重试/升级接管)。
    /// @return 本次实际发出撤单请求的数量
    uint32_t cancelLevelOrders(wtp::IUftStraCtx* ctx, QuoteLevel& level, CancelReason reason, uint64_t now = 0);

    /// v7.2 scout: 自由内层(level<obligation_level)成交 → 撤同侧义务层挂单
    /// (scout 成交=逆向信号, 避免义务大单在旧价被逆向成交; 重挂由下一tick按新价完成)
    /// @return true 表示该单是 scout 单 (义务侧撤单已处理, 调用方应跳过通用重挂逻辑)
    bool onScoutFillCancelObligation(wtp::IUftStraCtx* ctx, uint32_t localid);

    /// Handle order update — clear level order ID, trigger stats update
    /// @param localid       本地订单号
    /// @param isCanceled    是否撤销
    /// @param leftQty       剩余数量
    /// @param uTime_HHMM    当前时间 HHMM 格式（用于 BilateralStats 时间累计；0 = 不更新统计）
    /// @param sec_in_min    分钟内秒数 [0, 59]
    void onOrder(uint32_t localid, bool isCanceled, double leftQty, uint32_t uTime_HHMM = 0, uint32_t sec_in_min = 0);

    /// v7.1: 报单引擎确认 (on_entrust 回调) — 双边统计的挂单确认入口。
    /// 设计: 报单统计以引擎回调为准 (建模网络延迟), 不以发出报单时刻为准。
    /// 回测 mocker 用 postTask 异步触发 on_entrust (回调时 ID 已注册);
    /// 实盘 on_entrust 在柜台确认后异步返回。两者语义一致。
    /// @param uTime_HHMM    回调时刻 HHMM (stra_get_time, 回测=replay 实盘=交易所时间)
    /// @param sec_in_min    分钟内秒数 [0, 59]
    void onEntrustAck(uint32_t localid, uint32_t uTime_HHMM, uint32_t sec_in_min);

    /// Handle fill — 注: v7.1 起统计更新由 onOrder 独立完成
    /// (mocker/实盘中每笔 on_trade 必伴随 on_order(leftQty) 回调,
    ///  onTrade 的统计更新冗余, 已移除避免双计)
    /// @param uTime_HHMM    当前时间 HHMM 格式（0 = 不更新统计）
    /// @param sec_in_min    分钟内秒数 [0, 59]
    void onTrade(uint32_t localid, double vol, double price, uint32_t uTime_HHMM = 0, uint32_t sec_in_min = 0);

    //==========================================================================
    // Accessors
    //==========================================================================

    /// Get all active bid levels
    const std::vector<QuoteLevel>& getBidLevels() const { return _bid_levels; }

    /// Get all active ask levels
    const std::vector<QuoteLevel>& getAskLevels() const { return _ask_levels; }

    // V8-R6/P3: getBidLevelsMut/getAskLevelsMut/getLevelByOrder 已删除 ——
    // 内部可变引用逃生口全项目零调用(逐项 grep 复核), 保留即绕过 _lock 的竞态地雷。

    /// Total bid quantity outstanding
    double totalBidQty() const;

    /// Total ask quantity outstanding
    double totalAskQty() const;

    /// Check if a given order ID belongs to this quoter - O(1) via tracker
    bool isMyOrder(uint32_t localid) const;

    //==========================================================================
    // 双边报价统计接口（R3 v2 - Per-Quoter 值成员）
    //==========================================================================

    /// 配置 BilateralStats（每 quoter 独立实例）
    /// @param sessInfo  必须传入有效 sessionInfo;nullptr 会让本 quoter 的统计 DISABLED
    /// @return true=注入成功, false=禁用统计
    bool initBilateralStats(WTSSessionInfo* sessInfo)
    {
        BilateralStatsConfig bcfg;
        bcfg.min_valid_qty = _cfg.min_valid_qty;
        bcfg.bilateral_stats_max_spread_ticks = _cfg.obligation_max_spread_ticks; // 统一: 统计阈值 = 报价宽度
        _bilateral_stats.setConfig(bcfg);
        return _bilateral_stats.setSessionInfo(sessInfo, _cfg.code.c_str());
    }

    /// 获取统计模块（const,只读)
    const BilateralQuoteStats& getBilateralStats() const { return _bilateral_stats; }
    BilateralQuoteStats& getBilateralStats() { return _bilateral_stats; }

    /// 获取是否使用双边报价接口
    bool useBilateralQuote() const { return _cfg.use_bilateral_quote; }

    /// 获取有效报价快照（用于统计）
    /// 累计加权语义（R3 v2）：
    ///   - 从最优档累加到 min_valid_qty，最后档按 qty 截取
    ///   - 加权价 = Σ(qty_i × price_i) / min_valid_qty
    ///   - 整侧累计 < min_valid_qty 时 has_valid_xxx=false（深度不足该侧不 valid）
    ValidQuoteSnapshot getValidQuoteSnapshot() const;

private:
    /// Compute quantity for a given level
    __attribute__((always_inline)) inline double computeQty(uint32_t level) const
    {
        if (level < _level_qtys.size())
            return _level_qtys[level];
        double qty = _cfg.base_qty * pow(_cfg.level_qty_multiplier, level);
        return std::max(1.0, std::round(qty));
    }

    /// Validate computed price before placing order
    /// @param price   Computed price to validate
    /// @param mid     Current mid-price
    /// @param upper_limit Upper price limit (0 = no check)
    /// @param lower_limit Lower price limit (0 = no check)
    /// @return true if price is valid, false if invalid
    __attribute__((always_inline)) inline bool
    validatePrice(double price, double mid, double upper_limit, double lower_limit) const
    {
        // Check for NaN or Inf
        if (std::isnan(price) || std::isinf(price))
            return false;

        // Check for non-positive price
        if (price <= 0)
            return false;

        // Check against limits
        if (upper_limit > 0 && price > upper_limit)
            return false;
        if (lower_limit > 0 && price < lower_limit)
            return false;

        // Check for extreme deviation from mid (potential calculation error)
        if (_cfg.max_price_deviation > 0 && mid > 0 && _cfg.tick_size > 0) {
            double deviation_ticks = std::abs(price - mid) / _cfg.tick_size;
            if (deviation_ticks > _cfg.max_price_deviation)
                return false;
        }

        return true;
    }

    /// 检查是否需要更新订单（不对称粘性阈值）
    /// @param newPrice     新计算的价格
    /// @param currentPrice 当前订单价格
    /// @param is_bid       是否为 bid 方向
    /// @return true 表示需要更新订单
    bool checkStickyUpdate(double newPrice, double currentPrice, bool is_bid) const
    {
        // BID方向：向上(价格提高=改善)容忍更大，向下(价格降低=撤退)更敏感
        // ASK方向：向下(价格降低=改善)容忍更大，向上(价格提高=撤退)更敏感
        double upper_ratio = is_bid ? _cfg.improve_retreat_ratio : 1.0;
        double lower_ratio = is_bid ? 1.0 : _cfg.improve_retreat_ratio;
        double threshold = _cfg.sticky_threshold * _cfg.tick_size;
        double upper_bound = currentPrice + upper_ratio * threshold;
        double lower_bound = currentPrice - lower_ratio * threshold;
        return (newPrice > upper_bound || newPrice < lower_bound);
    }

    /// 根据当前 tracker 中活跃子单重新汇总该档剩余量。
    /// 用于避免 stra_buy/sell 拆成多子单时，最后一笔 onOrder 覆盖整档 qty。
    void recomputeLevelQty(QuoteLevel& level) const;

private:
    // v7.6 阶段3: per-quoter 递归自旋锁 — refreshQuotes/cancelAll(MdSpi)
    //   vs onTrade/onOrder/onEntrustAck/onScoutFill(TdSpi)。
    //   整方法守卫 (非四段式): stra 调用在锁内, 与 _order_api_mtx 形成
    //   quoter_lock → order_api_mtx 单向锁序。
    //   已知残留: getBilateralStats() 返回引用的外部使用 (session begin/end
    //   统计, RtTicker 安静期, 影响=统计失真非崩溃), 未加锁。
    mutable RecursiveSpinLock _lock;

    QuoterConfig _cfg;
    std::vector<QuoteLevel> _bid_levels;
    std::vector<QuoteLevel> _ask_levels;
    std::vector<double> _level_qtys;

    UnifiedOrderTracker* _tracker;
    wtp::IUftStraCtx* _ctx = nullptr;
    bool _allow_bid = true;
    bool _allow_ask = true;

    BilateralQuoteStats _bilateral_stats; ///< Per-Quoter 值成员（R3 v2）

    // 存储level索引+方向，避免bid/ask共用索引时查找冲突
    struct OrderLevelInfo
    {
        uint8_t level; ///< Level index
        bool is_bid;   ///< true=bid, false=ask
    };
    wtp::wt_hashmap<uint32_t, OrderLevelInfo> _order_id_to_level;

    //==========================================================================
    // refreshQuotes 子函数 (重构: 报单控制分离)
    //==========================================================================

    /// Quote level pricing result (obligation or flexible path).
    /// Output: bidPrice, askPrice, bidQty, askQty, is_obligation_bid, is_obligation_ask.
    struct QuoteResult
    {
        double bidPrice;
        double askPrice;
        double bidQty;
        double askQty;
        bool is_obligation_bid;
        bool is_obligation_ask;
    };
    /// Obligation pricing: bilateral, never blocked by hard_block.
    /// Adding side at passive price (mid +/- obligation_max_spread_ticks).
    /// Reducing side at aggressive price (close to mid, within obligation band).
    QuoteResult computeObligationPrices(uint32_t level,
                                        double mid,
                                        double l0_bid_price,
                                        double l0_ask_price,
                                        bool allow_bid,
                                        bool allow_ask,
                                        double upper_limit,
                                        double lower_limit,
                                        double best_bid,
                                        double best_ask,
                                        const StrategyInputs& strategy,
                                        double long_decay,
                                        double short_decay);

    /// Flexible pricing: can be unilateral, can be blocked by block_add (inventory management).
    /// Qty decay based on delta utilization. Sticky pricing to reduce churn.
    QuoteResult computeFlexiblePrices(uint32_t level,
                                      double mid,
                                      double l0_bid_price,
                                      double l0_ask_price,
                                      bool allow_bid,
                                      bool allow_ask,
                                      double upper_limit,
                                      double lower_limit,
                                      double best_bid,
                                      double best_ask,
                                      const StrategyInputs& strategy,
                                      double long_decay,
                                      double short_decay);

    /// Common price protection and limit validation (shared by both paths).
    /// Applied AFTER obligation/flexible-specific pricing.
    void applyPriceProtection(QuoteResult& qr, double mid,
                              double upper_limit, double lower_limit,
                              double best_bid, double best_ask) const;

    /// 判断当前 level 是否需要履行做市义务(双边报单)
    bool needObligation(uint32_t level) const
    {
        // Phase 4: obligation level is always obligation (always_obligation removed).
        return level == _cfg.obligation_level;
    }

    /// 路径A: 做市双边接口 (stra_quote, 顶单自动撤旧)
    /// 必须双边报单，allow 不阻断，风控通过价格偏移体现
    uint32_t handleBilateralQuote(uint32_t level, const QuoteResult& qr, double mid, uint64_t now);

    /// 路径B1: 普通报单 + 做市义务 (stra_buy + stra_sell, 先撤残留再双边下单)
    /// 不走 sticky，每 tick 刷新。义务模式下 qty 不衰减。
    uint32_t handleObligationQuote(uint32_t level, const QuoteResult& qr, double mid, uint64_t now);

    /// 路径B2: 普通报单 + 自由报价 (stra_buy + stra_sell, sticky, 可单边)
    /// 允许 qty 衰减和单边阻断
    uint32_t handleFlexibleQuote(uint32_t level, const QuoteResult& qr, double mid, uint64_t now);
};

} // namespace futu
