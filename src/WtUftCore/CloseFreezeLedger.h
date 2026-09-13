/*!
 * \file CloseFreezeLedger.h
 * \project WonderTrader
 *
 * \brief 平仓可用量预冻结台账 (F-core v3.1 / C1)
 *
 * 不变量：可用量 = 柜台基线 - 本通道在途平仓未终态量
 *
 * 语义约定（2026-09-11 用户裁定版）：
 *   1) **下单成功即冻结**（reserve，拿到有效 localid 时）——与"等待订单状态回报再冻结"
 *      相反：回报延迟/丢失期间，同一 burst 的后续订单会看到未扣减的可用量而超量平仓
 *      （09-11 22:25:35 实证：scout 1 手与义务层 10 手各按未扣减量拆出平仓腿 → CTP 50）
 *   2) 拒单/撤单**必须命中台账才归还**（exactly-once）；未命中的终态事件（重复推送、
 *      乱序回报、他源镜像推送）一律不归还，只计数 → 幻影注入在结构上不可能发生
 *   3) 成交只累计 traded；撤单归还 qty - traded；全部成交则销账
 *   4) 归还量由台账给出（委托量 - 成交量），调用方不得自行推断
 */
#pragma once

#include <cstdint>
#include <climits>
#include <string>
#include <unordered_map>
#include <mutex>

namespace wtp
{

class CloseFreezeLedger
{
public:
	struct Entry
	{
		std::string	stdCode;
		double		qty = 0.0;			///< 委托量（冻结量）
		double		traded = 0.0;		///< 已成交量（不可归还部分）
		bool		isLong = true;		///< 平多头？
		bool		isToday = false;	///< 平今？（false = 平昨 / 不区分平今平昨）
	};

	struct Stats
	{
		uint64_t	reserved = 0;		///< 累计冻结笔数
		uint64_t	released = 0;		///< 累计归还笔数（拒单 + 撤单）
		uint64_t	filled_out = 0;		///< 累计全成交销账笔数
		uint64_t	unmatched = 0;		///< 终态事件未命中台账次数（重复推送/他源/乱序）
		uint64_t	clamped = 0;		///< 归还被钳制次数（账本/柜台发散探针）
		double		released_qty = 0.0;	///< 累计归还数量
	};

public:
	/// 冻结登记。返回 false = localid 非法或重复登记（调用方应告警，不再冻结）
	bool reserve(uint32_t localid, const std::string& stdCode,
		bool isLong, bool isToday, double qty)
	{
		if (localid == UINT_MAX || qty <= 0.0)
			return false;

		std::lock_guard<std::mutex> lock(_mtx);
		auto ret = _entries.emplace(localid, Entry());
		if (!ret.second)
			return false;

		Entry& e = ret.first->second;
		e.stdCode = stdCode;
		e.qty = qty;
		e.isLong = isLong;
		e.isToday = isToday;
		_stats.reserved++;
		return true;
	}

	/// 拒单/撤单归还：命中则销账并回填明细，返回 true；未命中只计数，返回 false
	bool release(uint32_t localid, Entry& outEntry, double& outQty)
	{
		outEntry = Entry();
		outQty = 0.0;

		std::lock_guard<std::mutex> lock(_mtx);
		auto it = _entries.find(localid);
		if (it == _entries.end())
		{
			_stats.unmatched++;
			return false;
		}

		outEntry = it->second;
		outQty = it->second.qty - it->second.traded;
		if (outQty < 0.0)
			outQty = 0.0;

		_stats.released++;
		_stats.released_qty += outQty;
		_entries.erase(it);
		return true;
	}

	/// 成交回报：累计成交量；全部成交则销账
	void onTrade(uint32_t localid, double vol)
	{
		if (vol <= 0.0)
			return;

		std::lock_guard<std::mutex> lock(_mtx);
		auto it = _entries.find(localid);
		if (it == _entries.end())
			return;

		it->second.traded += vol;
		if (it->second.traded >= it->second.qty - 1e-9)
		{
			_stats.filled_out++;
			_entries.erase(it);
		}
	}

	/// 登录回放播种：存活平仓单的冻结已含在登录查询的 avail 净值中，视同已预扣
	void seed(uint32_t localid, const std::string& stdCode,
		bool isLong, bool isToday, double qty)
	{
		if (localid == UINT_MAX || qty <= 0.0)
			return;

		std::lock_guard<std::mutex> lock(_mtx);
		Entry& e = _entries[localid];
		e.stdCode = stdCode;
		e.qty = qty;
		e.isLong = isLong;
		e.isToday = isToday;
	}

	bool contains(uint32_t localid) const
	{
		std::lock_guard<std::mutex> lock(_mtx);
		return _entries.find(localid) != _entries.end();
	}

	size_t size() const
	{
		std::lock_guard<std::mutex> lock(_mtx);
		return _entries.size();
	}

	/// 在途冻结量合计（C6 快照重放用）；todayFilter: 0=仅平今, 1=仅平昨, 2=不限
	double pendingQty(const std::string& stdCode, bool isLong, int todayFilter = 2) const
	{
		std::lock_guard<std::mutex> lock(_mtx);
		double total = 0.0;
		for (const auto& kv : _entries)
		{
			const Entry& e = kv.second;
			if (e.stdCode != stdCode || e.isLong != isLong)
				continue;
			if (todayFilter == 0 && !e.isToday)
				continue;
			if (todayFilter == 1 && e.isToday)
				continue;
			total += (e.qty - e.traded);
		}
		return total;
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(_mtx);
		_entries.clear();
	}

	Stats stats() const
	{
		std::lock_guard<std::mutex> lock(_mtx);
		return _stats;
	}

	/// 记录一次"归还被钳制"（由调用方在 restoreCloseAvail 返回 true 时调用）
	void noteClamp()
	{
		std::lock_guard<std::mutex> lock(_mtx);
		_stats.clamped++;
	}

private:
	mutable std::mutex					_mtx;
	std::unordered_map<uint32_t, Entry>	_entries;
	Stats								_stats;
};

} // namespace wtp
