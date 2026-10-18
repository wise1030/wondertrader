/*
 * test_theta_grid.cpp
 *
 * ThetaGridStrategy（方案 B 立项 §20）信号级回放验证：
 *   KS tick → 500ms 网格（/tmp/opencode/ec_grid_500ms.csv）→ 直接驱动策略插件
 *   → 信号事件 + 简化成交模拟（入场 FAK 限价封顶 / 出场 mid 近似 B-3 消耗）
 *
 * 定位：策略逻辑验证（非精确撮合）。成本口径：滑点含在成交模拟里，
 * 费率以参数计入（默认 0，待 P0 费率闸门确认后填真实值）。
 *
 * 时钟：CSV ts_ms = UTC epoch ms。update 用 actiontime（HHMMSSmmm 本地），
 * generateSignal 用 µs。测试机需 TZ=Asia/Shanghai（WSL 默认即 +8）。
 */
#include "../WtFutuCore/arb/ThetaGridStrategy.h"
#include "../WtFutuCore/arb/SpreadArbitrageTypes.h"
#include "../WtFutuCore/arb/CrossLegContext.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace futu;

namespace
{
struct GridRow
{
    int day;
    uint64_t ts_ms; // UTC epoch ms
    double l1_px, l1_bid, l1_ask;
    double l2_px, l2_bid, l2_ask;
};

uint64_t toActionMs(uint64_t ts_ms)
{
    time_t sec = static_cast<time_t>(ts_ms / 1000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    return ((tmv.tm_hour * 10000ULL) + (tmv.tm_min * 100ULL) + tmv.tm_sec) * 1000ULL +
           (ts_ms % 1000);
}

std::vector<GridRow> loadCsv(const char* path)
{
    std::vector<GridRow> rows;
    std::ifstream f(path);
    if (!f)
        return rows;
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        GridRow r;
        std::stringstream ss(line);
        std::string cell;
        std::vector<double> v;
        std::getline(ss, cell, ','); r.day = atoi(cell.c_str());
        std::getline(ss, cell, ','); r.ts_ms = strtoull(cell.c_str(), nullptr, 10);
        while (std::getline(ss, cell, ',')) v.push_back(atof(cell.c_str()));
        if (v.size() < 6)
            continue;
        r.l1_px = v[0]; r.l1_bid = v[1]; r.l1_ask = v[2];
        r.l2_px = v[3]; r.l2_bid = v[4]; r.l2_ask = v[5];
        rows.push_back(r);
    }
    return rows;
}
} // namespace

TEST(test_theta_grid, replay_ec_4d)
{
    auto rows = loadCsv("/tmp/opencode/ec_grid_500ms.csv");
    ASSERT_GT(rows.size(), 1000) << "grid csv missing (run ks2wtpy pull first)";

    ThetaGridStrategy strat;
    SpreadPairConfig cfg;
    cfg.pair_id = "ec1112";
    cfg.leg1_code = "INE.ec2611";
    cfg.leg2_code = "INE.ec2612";
    cfg.grid.shadow = false; // 回放需要真实信号（执行链由本测试模拟）
    cfg.grid.ema_period = 50;
    cfg.grid.grid_lot = 1.0;
    cfg.grid.entry_max_slippage = 1.0; // = 生产配置
    cfg.grid.lag_ratio_max = 0.5;
    strat.configure(cfg);

    // 跨腿上下文（与生产同架构：arb onTick 链驱动, 策略读 output；§20 迁移v2）
    CrossLegContext ctx_xleg;
    ctx_xleg.setLegs("INE.ec2611", "INE.ec2612");

    // 模拟状态
    int pos = 0;          // spread 持仓（+1 多 spread / -1 空 / 0）
    double entry_spread = 0, entry_cost_basis = 0; // 两腿建仓成本（用于价差空间止损与盈亏）
    int entry_day = 0;

    // 统计
    int entries = 0, abandoned = 0, exits = 0, stops = 0;
    double pnl_sum = 0, pnl_max_dd = 0, pnl_peak = 0;
    int win = 0, loss = 0;

    auto closeAt = [&](double leg1_exit, double leg2_exit, const char* why, uint64_t ts_ms) {
        // 多 spread: buy leg1 @ ask, sell leg2 @ bid → exit 卖 leg1 @ bid, 买 leg2 @ ask
        double close_pnl;
        if (pos > 0)
            close_pnl = (leg1_exit - 0) - (leg2_exit - 0); // spread 差值变动
        else
            close_pnl = -(leg1_exit - 0) + (leg2_exit - 0);
        // 用 spread 变动结算: entry spread = l1_entry - l2_entry
        double exit_spread = leg1_exit - leg2_exit;
        close_pnl = (pos > 0 ? 1 : -1) * (exit_spread - entry_cost_basis); // 盈亏按 touch 成本结算
        pnl_sum += close_pnl;
        pnl_peak = std::max(pnl_peak, pnl_sum);
        pnl_max_dd = std::max(pnl_max_dd, pnl_peak - pnl_sum);
        if (close_pnl > 0) win++; else loss++;
        if (strcmp(why, "STOP") == 0) stops++; else exits++;
        printf("[GRID-REPLAY] %llu CLOSE(%s) entry_sp=%.2f exit_sp=%.2f pnl=%.2f sum=%.2f\n",
               (unsigned long long)ts_ms, why, entry_spread, exit_spread, close_pnl, pnl_sum);
        pos = 0;
        (void)entry_cost_basis;
    };

    int prev_day = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        const auto& r = rows[i];
        if (r.day != prev_day) { prev_day = r.day; entry_day = r.day; }

        SpreadState st;
        st.pair_id = "ec1112";
        st.leg1_code = "INE.ec2611";
        st.leg2_code = "INE.ec2612";
        st.leg1_price = r.l1_px;
        st.leg2_price = r.l2_px;
        st.current_spread = r.l1_px - r.l2_px;
        st.is_active = true;
        st.leg1_bid = r.l1_bid; st.leg1_ask = r.l1_ask;
        st.leg2_bid = r.l2_bid; st.leg2_ask = r.l2_ask;
        st.spread_position = pos;
        st.entry_spread = entry_spread; // mid 基准（策略止损参照系）
        st.last_update = toActionMs(r.ts_ms);

        strat.update(st, st.last_update);

        // 跨腿上下文驱动（与生产 manager.onTick 同路径; actiontime 时钟）
        auto midOf = [](double b, double a, double px) { return (b > 0 && a > 0) ? (b + a) / 2.0 : px; };
        ctx_xleg.onTick("INE.ec2611", midOf(r.l1_bid, r.l1_ask, r.l1_px), st.last_update);
        ctx_xleg.onTick("INE.ec2612", midOf(r.l2_bid, r.l2_ask, r.l2_px), st.last_update);
        {
            const auto& xo = ctx_xleg.output();
            st.cross_lag_ratio = xo.lag_ratio;
            st.cross_co_move = xo.co_move;
            st.cross_lead_dir = xo.lead_dir;
        }

        // 墙钟注入（等同生产 bridge ctx->stra_get_time/date：回放时刻）
        {
            time_t sec = (time_t)(r.ts_ms / 1000);
            struct tm tmv;
            localtime_r(&sec, &tmv);
            st.wall_hms = (uint32_t)(tmv.tm_hour * 10000 + tmv.tm_min * 100 + tmv.tm_sec);
            st.wall_date = (uint32_t)(tmv.tm_year + 1900) * 10000 + (uint32_t)(tmv.tm_mon + 1) * 100 +
                           (uint32_t)tmv.tm_mday;
        }
        auto sig = strat.generateSignal(st, r.ts_ms * 1000ULL); // µs

        if (!sig.isActionable())
            continue;

        if (sig.type == SpreadSignalType::OPEN_LONG_SPREAD || sig.type == SpreadSignalType::OPEN_SHORT_SPREAD) {
            // FAK 限价封顶成交判定：两腿同时成交才成格（弃格语义）
            bool buy1 = sig.type == SpreadSignalType::OPEN_LONG_SPREAD; // long spread: buy leg1 sell leg2
            // 用下一根网格的盘口成交（信号在网格点生成，成交在之后）
            if (i + 1 >= rows.size()) break;
            const auto& nx = rows[i + 1];
            bool leg1_ok = buy1 ? (nx.l1_ask > 0 && nx.l1_ask <= sig.leg1_price)
                                : (nx.l1_bid > 0 && nx.l1_bid >= sig.leg1_price);
            bool leg2_ok = !buy1 ? (nx.l2_ask > 0 && nx.l2_ask <= sig.leg2_price)
                                 : (nx.l2_bid > 0 && nx.l2_bid >= sig.leg2_price);
            if (leg1_ok && leg2_ok) {
                pos = buy1 ? 1 : -1;
                entry_cost_basis = (buy1 ? nx.l1_ask : nx.l1_bid) - (buy1 ? nx.l2_bid : nx.l2_ask); // 经济成本(touch)
                entry_spread = nx.l1_px - nx.l2_px; // 策略可见基准(mid, 与生产 manager entry_spread=current_spread 一致)
                entries++;
                printf("[GRID-REPLAY] %llu ENTRY %s sig_l1=%.2f sig_l2=%.2f entry_sp=%.2f\n",
                       (unsigned long long)r.ts_ms, buy1 ? "LONG" : "SHORT", sig.leg1_price,
                       sig.leg2_price, entry_spread);
            } else {
                abandoned++;
            }
        } else if (sig.type == SpreadSignalType::CLOSE_LONG_SPREAD ||
                   sig.type == SpreadSignalType::CLOSE_SHORT_SPREAD ||
                   sig.type == SpreadSignalType::STOP_LOSS) {
            if (pos != 0 && entry_day == r.day) { // 不跨日持仓
                const auto& nx = rows[i + 1 < rows.size() ? i + 1 : i];
                closeAt(nx.l1_px, nx.l2_px, sig.type == SpreadSignalType::STOP_LOSS ? "STOP" : "EXIT",
                        r.ts_ms);
            }
        }
    }

    printf("[GRID-REPLAY-SUM] entries=%d abandoned=%d exits=%d stops=%d win=%d loss=%d "
           "pnl=%.2f maxdd=%.2f\n",
           entries, abandoned, exits, stops, win, loss, pnl_sum, pnl_max_dd);

    // 验证基线（4 天小样本，阈值从宽；重点是"逻辑跑通 + 事件可解释"）
    EXPECT_GT(entries + abandoned, 0) << "无任何入场尝试，信号链异常";
    EXPECT_EQ(pos, 0) << "回放结束仍有持仓（14:55 强退未生效）";
    SUCCEED();
}

// §20 融合设计（10-14 批准）: shadow 模式网格自跟踪虚拟仓位生命周期验证。
// 模拟 MM 恒持 +8 手多价差对锁库存: 网格 LONG 入场(θ≤-λ) → 库存同号 →
// 正常带沿/止损退出; 网格 SHORT 入场(θ≥+λ) → 库存反号 → EXTERNAL_CLOSE。
TEST(test_theta_grid, replay_ec_4d_shadow_lifecycle)
{
    auto rows = loadCsv("/tmp/opencode/ec_grid_500ms.csv");
    ASSERT_GT(rows.size(), 1000) << "grid csv missing";

    std::filesystem::create_directories("./generated/outputs");
    const char* csv = "./generated/outputs/theta_grid_ec1112s_shadow.csv";
    std::remove(csv);
    std::remove("./generated/outputs/theta_grid_ec1112s_lambda.csv"); // λ 跨日持久化, 保证 hermetic

    ThetaGridStrategy strat;
    SpreadPairConfig cfg;
    cfg.pair_id = "ec1112s"; // 独立文件, 不污染基线用例
    cfg.leg1_code = "INE.ec2611";
    cfg.leg2_code = "INE.ec2612";
    cfg.grid.shadow = true; // shadow: 虚拟仓位自跟踪
    cfg.grid.ema_period = 50;
    cfg.grid.grid_lot = 1.0;
    cfg.grid.entry_max_slippage = 1.0;
    cfg.grid.lag_ratio_max = 0.5;
    strat.configure(cfg);

    CrossLegContext ctx_xleg;
    ctx_xleg.setLegs("INE.ec2611", "INE.ec2612");
    auto midOf = [](double b, double a, double px) { return (b > 0 && a > 0) ? (b + a) / 2.0 : px; };

    for (const auto& r : rows) {
        SpreadState st;
        st.pair_id = "ec1112s";
        st.leg1_code = "INE.ec2611";
        st.leg2_code = "INE.ec2612";
        st.leg1_price = r.l1_px;
        st.leg2_price = r.l2_px;
        st.current_spread = r.l1_px - r.l2_px;
        st.is_active = true;
        st.leg1_bid = r.l1_bid; st.leg1_ask = r.l1_ask;
        st.leg2_bid = r.l2_bid; st.leg2_ask = r.l2_ask;
        st.spread_position = 8; // MM 恒持 +8 多价差对锁库存
        st.entry_spread = st.current_spread;
        st.last_update = toActionMs(r.ts_ms);
        strat.update(st, st.last_update);
        ctx_xleg.onTick("INE.ec2611", midOf(r.l1_bid, r.l1_ask, r.l1_px), st.last_update);
        ctx_xleg.onTick("INE.ec2612", midOf(r.l2_bid, r.l2_ask, r.l2_px), st.last_update);
        {
            const auto& xo = ctx_xleg.output();
            st.cross_lag_ratio = xo.lag_ratio;
            st.cross_co_move = xo.co_move;
            st.cross_lead_dir = xo.lead_dir;
        }
        {
            time_t sec = (time_t)(r.ts_ms / 1000);
            struct tm tmv;
            localtime_r(&sec, &tmv);
            st.wall_hms = (uint32_t)(tmv.tm_hour * 10000 + tmv.tm_min * 100 + tmv.tm_sec);
            st.wall_date = (uint32_t)(tmv.tm_year + 1900) * 10000 + (uint32_t)(tmv.tm_mon + 1) * 100 +
                           (uint32_t)tmv.tm_mday;
        }
        strat.generateSignal(st, r.ts_ms * 1000ULL);
    }

    // 解析 shadow CSV 验证生命周期
    std::ifstream f(csv);
    ASSERT_TRUE(f.good()) << "shadow csv not produced";
    std::string line;
    std::getline(f, line); // header
    int entry_long = 0, entry_short = 0, exits = 0, stops = 0, ext_close = 0, force_flat = 0;
    // 状态机平衡校验（无日志节流, 事件流即完整生命周期）:
    // flat →ENTRY→ inpos →退出类(EXIT/STOP/DEATH/EXTERNAL/FORCE)→ flat
    bool inpos = false;
    while (std::getline(f, line)) {
        if (line.empty())
            continue;
        auto field = [&](int idx) {
            size_t p = 0;
            for (int i = 0; i < idx; i++) { p = line.find(',', p); if (p == std::string::npos) return std::string(); p++; }
            size_t e = line.find(',', p);
            return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
        };
        std::string ev = field(1);
        if (ev == "ENTRY_LONG" || ev == "ENTRY_SHORT") {
            ASSERT_FALSE(inpos) << "持仓中再次入场(单层纪律破坏): " << line;
            inpos = true;
            if (ev == "ENTRY_LONG") entry_long++; else entry_short++;
        } else if (ev == "EXIT_BAND" || ev == "STOP_LOSS" || ev == "DEATH_EXIT" ||
                   ev == "EXTERNAL_CLOSE" || ev == "FORCE_FLAT") {
            ASSERT_TRUE(inpos) << "空仓状态下退出事件: " << line;
            inpos = false;
            if (ev == "EXIT_BAND" || ev == "DEATH_EXIT") exits++;
            else if (ev == "STOP_LOSS") stops++;
            else if (ev == "EXTERNAL_CLOSE") ext_close++;
            else force_flat++;
        }
    }
    printf("[SHADOW-LIFECYCLE] entry_long=%d entry_short=%d exits=%d stops=%d ext_close=%d "
           "force_flat=%d final_inpos=%d\n",
           entry_long, entry_short, exits, stops, ext_close, force_flat, inpos ? 1 : 0);

    EXPECT_GT(entry_long + entry_short, 0) << "无虚拟入场";
    EXPECT_GT(ext_close, 0) << "库存反号时应出现 EXTERNAL_CLOSE(SHORT 入场遇 MM 多仓)";
    EXPECT_GT(exits + stops, 0) << "库存同号时应正常走带沿/止损退出(LONG 侧)";
    EXPECT_FALSE(inpos) << "回放结束虚拟仓位未归零";
}
