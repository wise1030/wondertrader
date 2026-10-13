// FutuTime.h
// 19.35: v7.1 replay 时钟 tick→伪 epoch 转换的单一来源。
//
// 语义注记 (为何不用 TimeUtils::makeTime):
//   本函数产出"伪 epoch" (date*86400000 + 日内毫秒, day-0 起算), 与
//   RiskMonitor._current_time / coordinator._last_exchange_time_ms 等既有
//   决策域同口径, 严格线性单调 (回测跨日/夏令时安全), 纯整数运算 ~10ns。
//   makeTime 走 mktime 产真实 Unix epoch —— 语义不符、慢两个量级、引入
//   时区不确定性, 混用会使全部 replay 时钟域漂移 (违背 v7.1 回测可复现初衷)。
//
// 用法: 仅 on_tick 主入口转换一次, 经参数下发 (bridge/orchestrator onTick
//       均收 uint64_t now_ms); 禁止在下游重复转换。
#pragma once

#include <cstdint>
#include "../Includes/WTSDataDef.hpp" // WTSTickData (inline actiondate/actiontime)

namespace futu
{

/// tick actiondate(yyyymmdd)/actiontime(HHMMSSsss) → replay 伪 epoch 毫秒
inline uint64_t replayMsFromTick(const wtp::WTSTickData* tick)
{
    const uint32_t ad = tick->actiondate();
    const uint32_t at = tick->actiontime();
    return static_cast<uint64_t>(ad) * 86400000ULL + static_cast<uint64_t>(at / 10000000) * 3600000ULL +
           static_cast<uint64_t>((at / 100000) % 100) * 60000ULL + static_cast<uint64_t>((at / 1000) % 100) * 1000ULL +
           (at % 1000);
}

} // namespace futu
