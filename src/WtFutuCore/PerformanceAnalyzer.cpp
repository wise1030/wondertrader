/*!
 * \file PerformanceAnalyzer.cpp
 * \brief Market Making Performance Analysis Implementation
 */
#include "PerformanceAnalyzer.h"
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace futu
{

PerformanceAnalyzer::PerformanceAnalyzer()
    : _quote_count(0), _total_pnl(0), _total_spread_captured(0), _total_adverse_selection(0), _total_real_adverse(0),
      _real_adverse_count(0), _total_volume(0), _total_trades(0), _winning_trades(0), _peak_pnl(0), _max_drawdown(0),
      _toxicity_events(0), _alpha_signals(0), _alpha_correct(0), _alpha_pnl(0), _start_time(0), _last_trade_time(0),
      _trading_days(0)
{
}

void PerformanceAnalyzer::recordTrade(const TradeRecord& trade)
{
    _trades.push(trade);

    // Update totals
    _total_volume += trade.qty;
    _total_trades++;
    _last_trade_time = trade.timestamp;
    // V8-R3: 首笔成交锚定 _start_time (此前恒 0 -> trading_time_sec 恒 0)
    if (_start_time == 0)
        _start_time = trade.timestamp;

    // Calculate PnL impact
    double immediate_pnl = trade.immediatePnL();
    _total_pnl += immediate_pnl;

    // Track spread captured
    double spread_cap = trade.spreadCaptured();
    _total_spread_captured += spread_cap;

    // 旧 adverse 计算 (保留兼容, 但不再作为主要指标)
    double adverse = calculateAdverseSelection(trade);
    _total_adverse_selection += adverse;

    // 真实 adverse: 加入 pending 队列, N tick 后评估
    PendingAdverse pa;
    pa.code = trade.code;
    pa.mid_at_trade = trade.mid_at_trade;
    pa.qty = trade.qty;
    pa.is_buy = trade.is_buy;
    pa.trade_timestamp = trade.timestamp;
    pa.ticks_remaining = _adverse_eval_ticks;
    _pending_adverse.push_back(pa);

    // Track winning trades
    if (immediate_pnl > 0)
        _winning_trades++;

    // Track alpha performance
    if (std::abs(trade.alpha_at_trade) >= _config.strong_alpha_threshold) {
        _alpha_signals++;
        // Alpha is correct if trade direction matches alpha direction
        bool alpha_correct = (trade.alpha_at_trade > 0 && trade.is_buy) || (trade.alpha_at_trade < 0 && !trade.is_buy);
        if (alpha_correct) {
            _alpha_correct++;
            _alpha_pnl += immediate_pnl;
        }
    }

    // Update PnL history
    _pnl_history.push(immediate_pnl);

    // 按合约累计真实成交 PnL — 此前 PositionState.realized_pnl 从未写入,
    // 导致 getPnLAttribution 的 inventory_pnl 恒为 0. 归因公式自闭合
    // (timing_pnl 为残差, total() 恒等 _total_pnl), 此处填充真实数据不破坏一致性.
    _positions[trade.code].realized_pnl += immediate_pnl;

    // Update drawdown
    if (_total_pnl > _peak_pnl)
        _peak_pnl = _total_pnl;

    double drawdown = _peak_pnl - _total_pnl;
    if (drawdown > _max_drawdown)
        _max_drawdown = drawdown;
}

void PerformanceAnalyzer::onTickUpdate(const std::string& code, double mid, uint64_t timestamp)
{
    // 评估 pending adverse: 每个 tick 递减 ticks_remaining, 到 0 时用当前 mid 计算
    for (auto& pa : _pending_adverse) {
        if (pa.evaluated)
            continue;
        if (pa.code != code)
            continue; // 只评估同合约的 pending

        pa.ticks_remaining--;
        if (pa.ticks_remaining == 0) {
            // 计算成交后价格变动
            double price_change = mid - pa.mid_at_trade;

            // 真实 adverse: 成交后价格逆向移动
            // 买入(is_buy=true)后价格下跌 → adverse
            // 卖出(is_buy=false)后价格上涨 → adverse
            double real_adverse = 0;
            if (pa.is_buy && price_change < 0) {
                real_adverse = std::abs(price_change) * pa.qty; // 买入后跌的量
            } else if (!pa.is_buy && price_change > 0) {
                real_adverse = price_change * pa.qty; // 卖出后涨的量
            }

            _total_real_adverse += real_adverse;
            _real_adverse_count++;
            pa.evaluated = true;
        }
    }

    // 清理: 已评估 或 墙钟超时(低流动性合约久等无 tick) 的 pending 一律移除.
    // 旧代码只 pop 连续已评估 front — 中段已评估/低流动性未评估项永久残留,
    // _pending_adverse 无上限增长 (内存泄漏).
    {
        uint64_t now = timestamp;
        auto it =
            std::remove_if(_pending_adverse.begin(), _pending_adverse.end(), [now, this](const PendingAdverse& pa) {
                if (pa.evaluated)
                    return true;
                return _adverse_timeout_ms > 0 && now > pa.trade_timestamp &&
                       (now - pa.trade_timestamp) > _adverse_timeout_ms;
            });
        _pending_adverse.erase(it, _pending_adverse.end());
    }
}

void PerformanceAnalyzer::recordQuote(
    const std::string& code, double bidPrice, double askPrice, double bidQty, double askQty, uint64_t timestamp)
{
    // V8-R3: 仅计数 -- 报价维度统计待有真实消费需求时再接 (参数有意不用)
    (void)code;
    (void)bidPrice;
    (void)askPrice;
    (void)bidQty;
    (void)askQty;
    (void)timestamp;
    _quote_count++;
}

void PerformanceAnalyzer::updatePosition(const std::string& code, double position, double avgCost)
{
    PositionState& ps = _positions[code];
    ps.position = position;
    ps.avg_cost = avgCost;
}



PerformanceMetrics PerformanceAnalyzer::getMetrics() const
{
    PerformanceMetrics metrics;

    // Basic metrics
    metrics.total_pnl = _total_pnl;
    metrics.total_volume = _total_volume;
    metrics.total_trades = _total_trades;

    // Spread metrics
    if (_total_trades > 0) {
        metrics.avg_spread_captured = _total_spread_captured / _total_trades;
    }
    metrics.spread_capture_rate = metrics.avg_spread_captured;

    // Fill metrics
    if (_quote_count > 0) {
        metrics.fill_rate = static_cast<double>(_total_trades) / _quote_count;
    }

    // Win rate
    if (_total_trades > 0) {
        metrics.win_rate = static_cast<double>(_winning_trades) / _total_trades;
    }

    // Drawdown
    metrics.max_drawdown = _max_drawdown;

    // Sharpe ratio (simplified)
    if (_pnl_history.size() >= 10) {
        double mean = 0, variance = 0;
        for (size_t i = 0; i < _pnl_history.size(); ++i) {
            mean += _pnl_history[i];
        }
        mean /= _pnl_history.size();

        for (size_t i = 0; i < _pnl_history.size(); ++i) {
            double diff = _pnl_history[i] - mean;
            variance += diff * diff;
        }
        variance /= _pnl_history.size();

        double std_dev = std::sqrt(variance);
        if (std_dev > 0) {
            // _pnl_history 是 per-trade 收益, 非 per-day. 正确年化因子 = sqrt(每年成交笔数).
            // 每年笔数 ≈ 250 * 平均每日笔数; 旧代码 sqrt(250) 假设每日仅 1 笔,
            // HFT 每日数千笔 → Sharpe 被系统性低估 ~sqrt(笔数) 倍.
            double days = (_trading_days > 0) ? static_cast<double>(_trading_days) : 1.0;
            double trades_per_day = static_cast<double>(_total_trades) / days;
            if (trades_per_day < 1.0)
                trades_per_day = 1.0;
            metrics.sharpe_ratio = mean / std_dev * std::sqrt(250.0 * trades_per_day);
        }
    }

    // Adverse selection
    if (_total_pnl != 0) {
        metrics.adverse_ratio = _total_adverse_selection / std::abs(_total_pnl);
    }
    // 真实 adverse (成交后价格逆向 / 成交量) — 独立指标, 不被 PnL 放大
    if (_total_volume > 0) {
        metrics.real_adverse_per_vol = _total_real_adverse / _total_volume;
    }
    metrics.toxicity_events = _toxicity_events;

    // Alpha performance
    if (_alpha_signals > 0) {
        metrics.alpha_accuracy = static_cast<double>(_alpha_correct) / _alpha_signals;
        metrics.alpha_pnl_per_trade = _alpha_pnl / _alpha_signals;
    }

    // Time metrics
    if (_start_time > 0 && _last_trade_time > _start_time) {
        metrics.trading_time_sec = (_last_trade_time - _start_time) / 1000;
    }
    metrics.trading_days = _trading_days;

    return metrics;
}

PnLAttribution PerformanceAnalyzer::getPnLAttribution() const
{
    PnLAttribution attr;

    // Spread PnL: positive spread capture
    attr.spread_pnl = _total_spread_captured > 0 ? _total_spread_captured : 0;

    // Adverse selection: negative spread capture + crossing costs
    attr.adverse_selection = _total_adverse_selection;

    // Alpha PnL: PnL from trades with strong alpha signals
    attr.alpha_pnl = _alpha_pnl;

    // Inventory PnL: 各合约累计真实成交 PnL (recordTrade 按合约累加)
    double inv_pnl = 0;
    for (const auto& kv : _positions) {
        inv_pnl += kv.second.realized_pnl;
    }
    attr.inventory_pnl = inv_pnl;

    // Timing PnL: residual
    attr.timing_pnl = _total_pnl - attr.spread_pnl - attr.inventory_pnl - attr.alpha_pnl + attr.adverse_selection;

    return attr;
}


double PerformanceAnalyzer::calculateAdverseSelection(const TradeRecord& trade) const
{
    // Adverse selection = price moved against us after trade
    // Estimate using spread and crossing status

    double adverse = 0;

    // If we crossed the spread, we paid half spread
    if (trade.is_crossing) {
        adverse = trade.spread_at_trade / 2 * trade.qty;
    }

    // Additional adverse selection based on spread captured
    double spread_cap = trade.spreadCaptured();
    if (spread_cap < 0) {
        // Negative spread capture indicates adverse selection
        adverse += std::abs(spread_cap) * trade.spread_at_trade * trade.qty;
    }

    return adverse;
}



void PerformanceAnalyzer::reset()
{
    _trades.clear();
    _quote_count = 0;
    _positions.clear();
    _total_pnl = 0;
    _total_spread_captured = 0;
    _total_adverse_selection = 0;
    _total_real_adverse = 0;
    _real_adverse_count = 0;
    _pending_adverse.clear();
    _total_volume = 0;
    _total_trades = 0;
    _winning_trades = 0;
    _peak_pnl = 0;
    _max_drawdown = 0;
    _toxicity_events = 0;
    _alpha_signals = 0;
    _alpha_correct = 0;
    _alpha_pnl = 0;
    _pnl_history.clear();
    _start_time = 0;
    _last_trade_time = 0;
    _trading_days = 0;

}

void PerformanceAnalyzer::resetDaily()
{
    // Keep trade history but reset daily counters
    _trading_days++;

    // Reset daily PnL tracking
    _peak_pnl = _total_pnl; // Reset peak to current PnL
    _max_drawdown = 0;
}

} // namespace futu
