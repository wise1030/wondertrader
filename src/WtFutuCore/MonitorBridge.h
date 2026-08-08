/*!
 * \file MonitorBridge.h
 * \project	WtFutuCore
 *
 * \brief	监控数据桥: 定期向 generated/stradata/{straId}.json 落地
 *			资金/持仓快照, 供 wtpy WtMonSvr GUI 读取 (DataMgr 60s 轮询)。
 *			数据契约与 CtaStraBaseCtx::save_data (WtCore) 一致。
 *
 * 设计要点:
 * - 热路径零开销: maybeFlush 仅做时间戳比较 (~ns), 到点才真正序列化写盘
 * - 线程安全: v7.4 框架源码核实实盘 on_tick(MdSpi) 与 on_trade(TdSpi)
 *   确实并发 (原假设正确); 策略层 _cb_mtx 已串行化回调, 本锁为纵深防御
 * - 原子写: tmp + rename, 避免 DataMgr 读到半文件
 */
#pragma once

#include <string>
#include <stdint.h>
#include <mutex>

#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class IUftStraCtx;
NS_WTP_END

namespace futu {

class FutuPortfolio;

class MonitorBridge
{
public:
	struct Config
	{
		bool		enabled;				///< 总开关 (默认关, 配置显式开)
		uint32_t	flush_interval_ms;		///< 落盘节流间隔

		Config() : enabled(false), flush_interval_ms(1000) {}
	};

	MonitorBridge() = default;

	/// on_init 时调用: 绑定组合数据源
	void	init(const char* straId, const FutuPortfolio* portfolio, const Config& cfg);

	/// 热路径调用 (on_tick/on_trade 末尾): 内部节流, 到点才写盘
	void	maybeFlush(wtp::IUftStraCtx* ctx);

	/// on_session_end 调用: 最终落盘 + 追加 funds.csv 历史资金曲线
	void	onSessionEnd(wtp::IUftStraCtx* ctx, uint32_t uTDate);

private:
	void	doFlush(uint32_t tdate);		// 调用方须已持锁
	void	appendFundsCsv(uint32_t tdate, double closeprofit,
						   double dynprofit, double fees);

private:
	const FutuPortfolio*	_portfolio = nullptr;
	std::string		_stra_id;
	Config			_cfg;
	uint64_t		_last_flush_ms = 0;
	std::mutex		_mtx;
};

} // namespace futu
