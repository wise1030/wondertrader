#pragma once
/*
 * ThetaGridStrategy.h
 *
 * 方案 B（价差信号反转网格套利）策略插件 — 立项 §20 (2026-10-13 批准)
 *
 * 信号口径（IM 跨期价差 θ 因子研究报告 + AO/EC 补充验证）：
 *   θ(t) = spread(t) − EMA₅₀(spread)，按日重置；λ = 前 5 交易日 |θ| 的 q80
 *   触带反向建仓（θ≥λ → 空 spread；θ≤−λ → 多 spread），带沿进出；
 *   lead-lag 确认（F1 先动/F2 未跟）+ 信息/压力型过滤（两腿 4s 动量同号跳过）；
 *   价差空间止损（stop 挂 spread 价格域，θ 仅作信号死亡判定 — 报告 v2 纪律）。
 *
 * P0 shadow 模式：完整计算信号并落 CSV 观测，一律不下单（return NONE）。
 * 线程模型与 MeanReversionStrategy 一致：update/generateSignal 均为 arb 线程
 * 单属主调用，内部状态不加锁。
 */
#include "ISpreadStrategy.h"

#include <deque>
#include <fstream>
#include <string>
#include <vector>

namespace futu
{

//==============================================================================
// 单格状态（live 模式）
//==============================================================================
struct ThetaGridCell
{
    int direction = 0;           ///< +1 = 多 spread（buy leg1 sell leg2），-1 = 空
    double qty = 0;              ///< 手数
    double entry_spread = 0;     ///< 建仓价差
    uint64_t entry_ms = 0;       ///< 建仓时间
};

//==============================================================================
// ThetaGridStrategy
//==============================================================================
class ThetaGridStrategy : public ISpreadStrategy
{
public:
    ThetaGridStrategy() = default;
    ~ThetaGridStrategy() override { flushDailyLambda(); }

    SpreadSignal generateSignal(const SpreadState& state, uint64_t current_time) override;
    void update(const SpreadState& state, uint64_t timestamp) override;
    void configure(const SpreadPairConfig& cfg) override;
    void reset() override;
    const char* typeName() const override { return "theta_grid"; }

private:
    ThetaGridConfig _cfg;
    std::string _pair_id;
    double _tick_size = 0.5;     ///< 腿 tick（lead-lag 阈值换算；configure 更新）

    // --- EMA 基线（日重置） ---
    double _ema = 0.0;
    bool _ema_valid = false;
    int _ema_day = -1;           ///< 最后更新的自然日（日重置锚）
    double _last_spread = 0.0;

    // --- 当日 |θ| 采样与日切 ---
    std::vector<double> _abs_theta_samples;
    int _sample_day = -1;
    uint64_t _last_sample_action = 0; ///< 采样节流的 actiontime 锚

    // --- λ（前 N 日分位） ---
    double _lambda = 0.0;        ///< 当日生效 λ（历史不足时 expanding）
    bool _lambda_loaded = false;
    std::vector<double> _daily_q_cache; ///< 启动加载的日 q80 序列

    // --- 触带簇 / 信号死亡 ---
    int _band_side = 0;          ///< 当前触带方向 (+1/-1)，0=带外
    uint64_t _band_enter_us = 0;
    
    uint64_t _death_since_us = 0; ///< 信号死亡计时（generateSignal µs 时钟）
    uint64_t _last_shadow_log_ms = 0;
    int _force_flat_logged_day = -1; ///< 时段门日志每日一次（防信号循环刷屏）
    int _time_block_logged_day = -1;

    // --- 格（live） ---
    std::vector<ThetaGridCell> _cells;

    // --- shadow CSV ---
    std::ofstream _shadow_csv;
    bool _shadow_csv_open = false;
    std::string _shadow_path;

    // --- λ 持久化 ---
    std::string _lambda_path;

    // 内部工具
    void updateEmaAndSample(const SpreadState& state, uint64_t ts_action);
    void loadDailyLambda();
    void flushDailyLambda();
    double computeLambda() const;
    int dayOf(uint64_t ts_us) const;        ///< epoch µs → 自然日 yyyymmdd
    uint32_t hmsOf(uint64_t ts_us) const;        ///< epoch µs → HHMMSS(本地)
    void shadowLog(uint64_t ts_us, const char* event, const SpreadState& state,
                   double theta, double f1_move, double f2_move, const char* note);
    SpreadSignal makeSignal(SpreadSignalType type, int direction, const SpreadState& state,
                            const char* reason, uint64_t ts_us) const;
    double quantile(const std::vector<double>& sorted_vals, double q) const;
};

} // namespace futu
