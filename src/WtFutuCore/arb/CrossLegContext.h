#pragma once
/*
 * CrossLegContext.h  (arb 层 pair 级通用基础模块; 方案 B §20 架构迁移v2)
 *
 * 跨腿上下文：输入两腿 mid 流（arb 线程 manager.onTick 驱动 —— 与价差策略
 * 插件同一使能前提，无外部隐藏依赖），输出
 *   lag_ratio = Σ|ΔF2平滑| / Σ|ΔF1平滑|  （比较窗内; 0=未跟, 1=已同步）
 *   co_move   = ΔF1·ΔF2 同号            （信息型过滤; 1=信息型）
 *   lead_dir  = sign(ΣΔF1)
 * 任意 ISpreadStrategy 插件经 SpreadState.cross_* 通用消费（不消费则无依赖）。
 *
 * 线程模型：update* 与读取均在 arb 线程（manager.onTick / generateSignal 同
 * 线程），普通成员即可，无锁无原子。
 *
 * 时钟：timestamp 为 actiontime（HHMMSSmmm）。窗宽/守卫用其差分（日内等价
 * 毫秒；EC 无夜盘不跨零 —— 与插件既有约束一致）。
 *
 * 算法骨架承袭 CrossLegSignalSource/LeadLagSignalSource 成熟件：RingBuffer
 * 线性衰减加权平滑 + 跨间隙守卫 + 时效老化。
 */
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace futu
{

class CrossLegContext
{
public:
    struct Config
    {
        uint32_t smooth_window;      ///< mid_change 加权平滑窗口（样本数）
        uint32_t compare_window_ms;  ///< lag_ratio / lead_dir 比较窗（actiontime 差分）
        uint32_t co_move_window_ms;  ///< 信息型判定窗
        uint32_t max_age_ms;         ///< 两腿数据最大时龄，超龄输出失效（0=关闭）
        uint32_t gap_guard_ms;       ///< 跨间隙守卫：tick 间隔超此值清历史跳帧

        Config()
            : smooth_window(20), compare_window_ms(1000), co_move_window_ms(4000),
              max_age_ms(3000), gap_guard_ms(5000)
        {}
    };

    struct Output
    {
        double lag_ratio = 1.0; ///< 1.0 = 默认不确认（fail-safe）
        int co_move = -1;       ///< -1 未知, 0 压力型, 1 信息型
        double lead_dir = 0.0;  ///< sign(ΣΔF1)
        uint64_t ts = 0;        ///< 最后计算时刻
    };

    explicit CrossLegContext(Config cfg = Config()) : _cfg(cfg) {}

    void setLegs(const std::string& leg1, const std::string& leg2)
    {
        _leg1 = leg1;
        _leg2 = leg2;
    }

    bool hasLeg(const std::string& code) const { return code == _leg1 || code == _leg2; }

    /// arb 线程：任一腿 mid 更新
    void onTick(const std::string& code, double mid, uint64_t ts_action)
    {
        if (code != _leg1 && code != _leg2)
            return;
        LegState& ls = (code == _leg1) ? _leg1_state : _leg2_state;

        // 跨间隙守卫：陈旧 prev-mid 差分是伪跳变，跳帧
        if (ls.last_mid > 0 && ts_action > ls.last_ts && ts_action - ls.last_ts > _cfg.gap_guard_ms) {
            ls.changes.clear();
            ls.last_mid = mid;
            ls.last_ts = ts_action;
            return;
        }
        if (ls.last_mid > 0 && mid > 0) {
            ls.changes.push_back({ts_action, mid - ls.last_mid});
            while (ls.changes.size() > _cfg.smooth_window)
                ls.changes.pop_front();
            recompute(ts_action);
        }
        ls.last_mid = mid;
        ls.last_ts = ts_action;
    }

    const Output& output() const { return _out; }

private:
    struct LegChange
    {
        uint64_t ts;
        double delta;
    };
    struct LegState
    {
        double last_mid = 0;
        uint64_t last_ts = 0;
        std::deque<LegChange> changes;
    };

    static double smoothedSum(const LegState& ls, uint64_t now, uint32_t window_ms, double& abs_sum)
    {
        double weighted_sum = 0, weight_sum = 0, abs_weighted = 0;
        const auto& c = ls.changes;
        size_t n = c.size();
        for (size_t i = 0; i < n; ++i) {
            if (now - c[i].ts > window_ms)
                continue;
            double w = static_cast<double>(i + 1); // 最新权重最大
            weighted_sum += w * c[i].delta;
            abs_weighted += w * std::fabs(c[i].delta);
            weight_sum += w;
        }
        if (weight_sum <= 0) {
            abs_sum = 0;
            return 0;
        }
        abs_sum = abs_weighted / weight_sum;
        return weighted_sum / weight_sum;
    }

    void recompute(uint64_t now)
    {
        if (_cfg.max_age_ms > 0 &&
            (now - _leg1_state.last_ts > _cfg.max_age_ms || now - _leg2_state.last_ts > _cfg.max_age_ms)) {
            _out.co_move = -1;
            _out.lag_ratio = 1.0;
            _out.ts = now;
            return;
        }

        double abs1 = 0, dummy2 = 0;
        double sum1 = smoothedSum(_leg1_state, now, _cfg.compare_window_ms, abs1);
        smoothedSum(_leg2_state, now, _cfg.compare_window_ms, dummy2);

        _out.lag_ratio = (abs1 > 1e-9) ? dummy2 / abs1 : 1.0;
        _out.lead_dir = (sum1 > 1e-9) ? 1.0 : (sum1 < -1e-9 ? -1.0 : 0.0);

        double a1 = 0, a2 = 0;
        double m1 = smoothedSum(_leg1_state, now, _cfg.co_move_window_ms, a1);
        double m2 = smoothedSum(_leg2_state, now, _cfg.co_move_window_ms, a2);
        int co = -1;
        if (std::fabs(m1) > 1e-9 && std::fabs(m2) > 1e-9)
            co = (m1 * m2 > 0) ? 1 : 0;
        _out.co_move = co;
        _out.ts = now;
    }

    Config _cfg;
    std::string _leg1, _leg2;
    LegState _leg1_state, _leg2_state;
    Output _out;
};

} // namespace futu
