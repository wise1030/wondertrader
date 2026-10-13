/*
 * ThetaGridStrategy.cpp
 *
 * 方案 B（价差信号反转网格套利）策略插件 — 立项 §20 (2026-10-13 批准)
 *
 * 时钟约定（混合时钟，策略内部不跨时钟比较）：
 *   - update(state, ts) 的 ts = actiontime（HHMMSSmmm 整数，日内相对节奏）
 *     —— 本插件面向无夜盘品种（EC 09:00-15:00 无跨零）；腿史窗口/EMA 采样用它。
 *   - generateSignal(state, ts) 的 ts = µs（arbNowUs；实盘=墙钟，回测=replay）
 *     —— 日切检测/λ flush/死亡计时/时段门用它（可转自然日与 HHMMSS）。
 *   - 回测路径 replay 时钟为真实 epoch 域（19.35 FutuTime 语义），localtime_r 可用。
 *
 * P0 shadow：信号完整计算并落 ./generated/outputs/theta_grid_<pair>_shadow.csv，
 * 一律 return NONE（执行链零接触）。live 由 grid.shadow=false 打开。
 */
#include "ThetaGridStrategy.h"

#include "WTSLogger.h"
#include <algorithm>
#include <cmath>
#include <ctime>

namespace futu
{

namespace
{
// actiontime(HHMMSSmmm) → 当日毫秒
inline uint32_t actionMsOf(uint64_t actiontime)
{
    uint32_t t = static_cast<uint32_t>(actiontime % 1000000000ULL);
    uint32_t h = t / 10000000;
    uint32_t m = (t / 100000) % 100;
    uint32_t s = (t / 1000) % 100;
    uint32_t ms = t % 1000;
    return ((h * 60 + m) * 60 + s) * 1000 + ms;
}

// µs epoch → 本地 HHMMSS（时段门）
inline uint32_t hmsOfUs(uint64_t us)
{
    time_t sec = static_cast<time_t>(us / 1000000ULL);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    return static_cast<uint32_t>((tmv.tm_hour * 10000) + (tmv.tm_min * 100) + tmv.tm_sec);
}

// µs epoch → 本地自然日 yyyymmdd（日切锚）
inline int dayOfUs(uint64_t us)
{
    time_t sec = static_cast<time_t>(us / 1000000ULL);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    return (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
}
} // namespace

void ThetaGridStrategy::configure(const SpreadPairConfig& cfg)
{
    _cfg = cfg.grid;
    _pair_id = cfg.pair_id;
    loadDailyLambda();

    _lambda_path = "./generated/outputs/theta_grid_" + _pair_id + "_lambda.csv";
    _shadow_path = "./generated/outputs/theta_grid_" + _pair_id + "_shadow.csv";

    WTSLogger::info("[ThetaGrid] pair={} shadow={} ema={} q{} lookback={} exit={} stop={} lots={}x{}",
                    _pair_id.c_str(), _cfg.shadow ? 1 : 0, _cfg.ema_period, _cfg.lambda_quantile_pct,
                    _cfg.lambda_lookback_days, _cfg.band_exit_ratio, _cfg.stop_loss_ratio, _cfg.grid_lot,
                    _cfg.max_grids);
}

void ThetaGridStrategy::reset()
{
    // session 切换：EMA/采样/腿史/格清空；λ 历史保留（跨日文件）
    _ema = 0.0;
    _ema_valid = false;
    _ema_day = -1;
    _abs_theta_samples.clear();
    _sample_day = -1;
    _band_side = 0;
    _death_since_us = 0;
    _cells.clear();
}

void ThetaGridStrategy::update(const SpreadState& state, uint64_t timestamp)
{
    // 腿史维护已移除（lead-lag 上移至 CrossLegContext, §20 迁移v2）
    // EMA 更新（样本序推进；周期=ema_period 个样本）
    updateEmaAndSample(state, timestamp);

    // 3) 记录最新价差供 generateSignal 使用
    _last_spread = state.current_spread;
}

void ThetaGridStrategy::updateEmaAndSample(const SpreadState& state, uint64_t ts_action)
{
    if (state.current_spread == 0 || state.leg1_price <= 0 || state.leg2_price <= 0)
        return;

    double alpha = 2.0 / (_cfg.ema_period + 1.0);
    if (!_ema_valid) {
        _ema = state.current_spread;
        _ema_valid = true;
    } else {
        _ema = alpha * state.current_spread + (1.0 - alpha) * _ema;
    }

    // |θ| 采样节流：actiontime 差 ≥ 500ms 采一点（日内相对节奏）
    uint32_t cur_ams = actionMsOf(ts_action);
    uint32_t last_ams = actionMsOf(_last_sample_action);
    uint32_t diff = (cur_ams >= last_ams) ? (cur_ams - last_ams) : (cur_ams + 86400000u - last_ams);
    if (_last_sample_action == 0 || diff >= 500) {
        _last_sample_action = ts_action;
        double theta = state.current_spread - _ema;
        _abs_theta_samples.push_back(std::fabs(theta));
        if (_abs_theta_samples.size() > 200000)
            _abs_theta_samples.erase(_abs_theta_samples.begin());
    }
}

int ThetaGridStrategy::dayOf(uint64_t ts_us) const { return dayOfUs(ts_us); }
uint32_t ThetaGridStrategy::hmsOf(uint64_t ts_us) const { return hmsOfUs(ts_us); }

double ThetaGridStrategy::quantile(const std::vector<double>& vals, double q) const
{
    if (vals.empty())
        return 0.0;
    std::vector<double> s = vals;
    std::sort(s.begin(), s.end());
    size_t idx = static_cast<size_t>(q * (s.size() - 1));
    return s[idx];
}

void ThetaGridStrategy::loadDailyLambda()
{
    // 启动加载历史日分位：./generated/outputs/theta_grid_<pair>_lambda.csv (date,q80,q90,n)
    _daily_q_cache.clear();
    std::string path = "./generated/outputs/theta_grid_" + _pair_id + "_lambda.csv";
    std::ifstream f(path);
    if (!f)
        return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == 'd')
            continue; // header
        // date,q80,q90,n —— 取 q80 字段（索引 1；兼容逗号）
        size_t p1 = line.find(',');
        if (p1 == std::string::npos)
            continue;
        size_t p2 = line.find(',', p1 + 1);
        double q = atof(line.substr(p1 + 1, p2 == std::string::npos ? std::string::npos : p2 - p1 - 1).c_str());
        if (q > 0)
            _daily_q_cache.push_back(q);
    }
    _lambda_loaded = true;
}

void ThetaGridStrategy::flushDailyLambda()
{
    if (_pair_id.empty() || _abs_theta_samples.empty())
        return;
    if (_lambda_path.empty())
        _lambda_path = "./generated/outputs/theta_grid_" + _pair_id + "_lambda.csv";

    double q = _cfg.lambda_quantile_pct / 100.0;
    double q80 = quantile(_abs_theta_samples, 0.80);
    double q90 = quantile(_abs_theta_samples, 0.90);

    std::ofstream f(_lambda_path, std::ios::app);
    if (f) {
        if (f.tellp() == 0)
            f << "date,q80,q90,n\n";
        // 日期用当日最后样本不可得——用文件追加日的日切由调用方保证一天一次
        f << _sample_day << "," << q80 << "," << q90 << "," << _abs_theta_samples.size() << "\n";
    }
    // 并入缓存（截断到回看窗）
    if (q80 > 0)
        _daily_q_cache.push_back(q80);
    while (_daily_q_cache.size() > _cfg.lambda_lookback_days)
        _daily_q_cache.erase(_daily_q_cache.begin());
    _abs_theta_samples.clear();
    (void)q;
}

double ThetaGridStrategy::computeLambda() const
{
    // 历史优先：最近 ≤N 个日分位均值；历史不足 → 当日 expanding q80（shadow 暖机期）
    if (!_daily_q_cache.empty()) {
        double sum = 0;
        for (double v : _daily_q_cache)
            sum += v;
        return sum / _daily_q_cache.size();
    }
    if (!_abs_theta_samples.empty()) {
        std::vector<double> s = _abs_theta_samples;
        return quantile(s, _cfg.lambda_quantile_pct / 100.0);
    }
    return 0.0;
}

// lead-lag 确认 / 信息型过滤已上移至 CrossLegContext（方案 B §20 迁移v2）：
// 主线程计算 lag_ratio/co_move/lead_dir 原子快照，manager 填 SpreadState，
// 策略直接消费 state.cross_* 三分量（fail-safe 默认不确认/未知）。

void ThetaGridStrategy::shadowLog(uint64_t ts_us, const char* event, const SpreadState& state,
                                  double theta, double f1_move, double f2_move, const char* note)
{
    if (!_cfg.shadow)
        return;
    if (_shadow_path.empty())
        _shadow_path = "./generated/outputs/theta_grid_" + _pair_id + "_shadow.csv";
    if (!_shadow_csv_open) {
        _shadow_csv.open(_shadow_path, std::ios::app);
        _shadow_csv_open = _shadow_csv.good();
        if (_shadow_csv_open && _shadow_csv.tellp() == 0) {
            _shadow_csv << "hms,event,spread,ema,theta,lambda,f1move,f2move,pos,would_size,entry_spread,note\n";
        }
    }
    if (!_shadow_csv_open)
        return;
    _shadow_csv << hmsOf(ts_us) << "," << event << "," << state.current_spread << "," << _ema << ","
                << theta << "," << _lambda << "," << f1_move << "," << f2_move << ","
                << state.spread_position << "," << _cfg.grid_lot << "," << state.entry_spread << ","
                << (note ? note : "") << "\n";
    _shadow_csv.flush();
}

SpreadSignal ThetaGridStrategy::makeSignal(SpreadSignalType type, int direction, const SpreadState& state,
                                           const char* reason, uint64_t ts_us) const
{
    SpreadSignal sig;
    sig.type = type;
    sig.pair_id = _pair_id;
    sig.source = ArbitrageStrategy::THETA_GRID;
    sig.timestamp = ts_us;
    sig.confidence = 0.8;
    sig.suggested_size = _cfg.grid_lot;
    sig.entry_zscore = state.current_spread; // θ 网格复用字段：entry=spread 快照
    sig.target_zscore = _ema;                // target=EMA（回归目标）
    sig.leg1_code = state.leg1_code;
    sig.leg2_code = state.leg2_code;
    sig.leg1_price = state.leg1_price;
    sig.leg2_price = state.leg2_price;
    sig.leg1_qty = _cfg.grid_lot;
    sig.leg2_qty = _cfg.grid_lot;
    sig.reason = reason;

    // 入场 FAK 限价封顶（P2）: 信号快照价 ± entry_max_slippage 为最差可接受价，
    // 执行器对 THETA_GRID 入场单转 FAK → 限价内立即成交、剩余撤销（=滑点硬顶+弃格语义）
    if (type == SpreadSignalType::OPEN_LONG_SPREAD) {   // buy leg1, sell leg2
        sig.leg1_price += _cfg.entry_max_slippage;
        sig.leg2_price -= _cfg.entry_max_slippage;
    } else if (type == SpreadSignalType::OPEN_SHORT_SPREAD) { // sell leg1, buy leg2
        sig.leg1_price -= _cfg.entry_max_slippage;
        sig.leg2_price += _cfg.entry_max_slippage;
    }
    (void)direction;
    return sig;
}

SpreadSignal ThetaGridStrategy::generateSignal(const SpreadState& state, uint64_t current_time)
{
    SpreadSignal none;
    none.pair_id = _pair_id;
    none.timestamp = current_time;

    if (!state.is_active || !_ema_valid || state.leg1_price <= 0 || state.leg2_price <= 0)
        return none;

    // --- 日切：EMA 重置 + λ 采样 flush（generateSignal 时钟 µs 可提日期） ---
    int today = dayOf(current_time); // current_time 为 µs
    if (_ema_day != today) {
        if (_ema_day != -1 && !_abs_theta_samples.empty())
            flushDailyLambda();
        _ema_day = today;
        _sample_day = today;
        _ema_valid = false; // 日内首样本重新播种
        _band_side = 0;
        _death_since_us = 0;
    }

    double theta = state.current_spread - _ema;
    _lambda = computeLambda();
    if (_lambda <= 0)
        return none;

    uint32_t hms = hmsOf(current_time);
    double abs_theta = std::fabs(theta);
    int side = (theta >= _lambda) ? +1 : (theta <= -_lambda) ? -1 : 0;
    double exit_band = _cfg.band_exit_ratio * _lambda;
    double death_band = _cfg.death_ratio * _lambda;
    bool has_pos = state.hasPosition();
    double f1_move = 0, f2_move = 0;

    // --- 时段门 ---
    bool time_block_new = (hms >= _cfg.no_new_after && hms < _cfg.force_flat_at);
    bool time_force_flat = (hms >= _cfg.force_flat_at);

    // --- 1. 持仓管理（exit 优先） ---
    if (has_pos) {
        // 价差空间止损（对整体 entry_spread；格级细分 P2 细化）
        // 不利方向: 多 spread(pos>0) 亏于 spread 下行; 空 spread 亏于上行
        //   → (current - entry) × pos < 0 为亏损方向
        double adverse = std::fabs(state.current_spread - state.entry_spread);
        if (adverse >= _cfg.stop_loss_ratio * _lambda &&
            (state.current_spread - state.entry_spread) * (state.spread_position > 0 ? 1.0 : -1.0) < 0) {
            shadowLog(current_time, "STOP_LOSS", state, theta, f1_move, f2_move,
                      "spread-space stop");
            return _cfg.shadow ? none
                               : makeSignal(SpreadSignalType::STOP_LOSS, 0, state,
                                            "theta grid spread-space stop", current_time);
        }
        // 带沿出场
        if (abs_theta <= exit_band) {
            shadowLog(current_time, "EXIT_BAND", state, theta, f1_move, f2_move, "|theta|<=exit");
            return _cfg.shadow ? none
                               : makeSignal(state.spread_position > 0 ? SpreadSignalType::CLOSE_LONG_SPREAD
                                                                      : SpreadSignalType::CLOSE_SHORT_SPREAD,
                                            0, state, "theta grid band exit", current_time);
        }
        // 信号死亡
        if (abs_theta <= death_band) {
            if (_death_since_us == 0)
                _death_since_us = current_time; // µs
            else if (current_time - _death_since_us >= _cfg.death_hold_ms * 1000ULL) {
                shadowLog(current_time, "DEATH_EXIT", state, theta, f1_move, f2_move,
                          "signal death");
                return _cfg.shadow ? none
                                   : makeSignal(state.spread_position > 0
                                                    ? SpreadSignalType::CLOSE_LONG_SPREAD
                                                    : SpreadSignalType::CLOSE_SHORT_SPREAD,
                                                0, state, "theta grid signal death", current_time);
            }
        } else {
            _death_since_us = 0;
        }
        // 强退时段
        if (time_force_flat) {
            // 日志每日一次; CLOSE 信号每 tick 照发（live 首笔未成需重试, 防持仓过夜）
            if (_force_flat_logged_day != today) {
                _force_flat_logged_day = today;
                shadowLog(current_time, "FORCE_FLAT", state, theta, f1_move, f2_move, "14:55");
            }
            return _cfg.shadow ? none
                               : makeSignal(state.spread_position > 0 ? SpreadSignalType::CLOSE_LONG_SPREAD
                                                                      : SpreadSignalType::CLOSE_SHORT_SPREAD,
                                            0, state, "theta grid 14:55 force flat", current_time);
        }
        return none; // 持仓中不加新格（单层网格纪律：回归期不摊平）
    }

    // --- 2. 无仓：入场评估 ---
    if (side == 0) {
        if (_band_side != 0) {
            _band_side = 0; // 出带，簇结束
            shadowLog(current_time, "BAND_LEAVE", state, theta, f1_move, f2_move, "left band");
        }
        return none;
    }

    // 触带簇状态
    if (_band_side != side) {
        _band_side = side;
        _band_enter_us = current_time; // µs
        shadowLog(current_time, side > 0 ? "BAND_ENTER+" : "BAND_ENTER-", state, theta, f1_move,
                  f2_move, "entered band");
    } else {
        // 簇内周期状态行（节流）
        if (current_time - _last_shadow_log_ms >= _cfg.shadow_log_interval_ms * 1000ULL) {
            _last_shadow_log_ms = current_time;
            shadowLog(current_time, side > 0 ? "IN_BAND+" : "IN_BAND-", state, theta, f1_move,
                      f2_move, "in band");
        }
    }

    if (time_block_new || time_force_flat) {
        if (_time_block_logged_day != today) {
            _time_block_logged_day = today;
            shadowLog(current_time, "TIME_BLOCK", state, theta, f1_move, f2_move, "after 14:50");
        }
        return none;
    }

    // lead-lag 确认 + 信息型过滤（CrossLegContext 输出, manager 已填 state）
    bool ll_ok = (state.cross_lag_ratio <= _cfg.lag_ratio_max);
    bool info = (state.cross_co_move == 1);
    f1_move = state.cross_lag_ratio; // shadow 日志复用列: 记 lag_ratio
    f2_move = state.cross_lead_dir;  // 记 lead_dir

    if (state.cross_co_move < 0) {
        shadowLog(current_time, "SIGNAL_STALE", state, theta, f1_move, f2_move, "cross-leg snapshot stale");
        return none;
    }
    if (info) {
        shadowLog(current_time, "FILTER_INFO", state, theta, f1_move, f2_move,
                  "informational, skip");
        return none;
    }
    if (!ll_ok) {
        shadowLog(current_time, "LL_WAIT", state, theta, f1_move, f2_move, "lead-lag not confirmed");
        return none;
    }

    // 入场信号：θ≥+λ → 空 spread（卖 F1 买 F2）；θ≤−λ → 多 spread
    shadowLog(current_time, side > 0 ? "ENTRY_SHORT" : "ENTRY_LONG", state, theta, f1_move, f2_move,
              "confirmed");
    if (_cfg.shadow)
        return none;
    return makeSignal(side > 0 ? SpreadSignalType::OPEN_SHORT_SPREAD : SpreadSignalType::OPEN_LONG_SPREAD,
                      side, state, "theta grid entry", current_time);
}

} // namespace futu
