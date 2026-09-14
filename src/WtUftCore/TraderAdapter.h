/*!
 * \file TraderAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once

#include "../Includes/FasterDefs.h"
#include "../Includes/ITraderApi.h"
#include "../Share/BoostFile.hpp"
#include "../Share/StdUtils.hpp"
#include "../Includes/WTSCollection.hpp"
#include "../Share/SpinMutex.hpp"
#include "CloseFreezeLedger.h"
#include <mutex>

NS_WTP_BEGIN
class WTSVariant;
class WTSContractInfo;
class WTSCommodityInfo;
class ITrdNotifySink;
class ActionPolicyMgr;

typedef std::vector<uint32_t> OrderIDs;
typedef WTSMap<uint32_t> OrderMap;

class TraderAdapter : public ITraderSpi
{
public:
	TraderAdapter();
	~TraderAdapter();

	typedef enum tagAdapterState
	{
		AS_NOTLOGIN,		//未登录
		AS_LOGINING,		//正在登录
		AS_LOGINED,			//已登录
		AS_LOGINFAILED,		//登录失败
		AS_POSITION_QRYED,	//仓位已查
		AS_ORDERS_QRYED,	//订单已查
		AS_TRADES_QRYED,	//成交已查
		AS_ALLREADY			//全部就绪
	} AdapterState;

	typedef struct _PosItem
	{
		//多仓数据
		double	l_newvol;
		double	l_newavail;
		double	l_prevol;
		double	l_preavail;

		//空仓数据
		double	s_newvol;
		double	s_newavail;
		double	s_prevol;
		double	s_preavail;

		_PosItem()
		{
			memset(this, 0, sizeof(_PosItem));
		}

		double total_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newvol + l_prevol;
			else
				return s_newvol + s_prevol;
		}

		double avail_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newavail + l_preavail;
			else
				return s_newavail + s_preavail;
		}

	} PosItem;

	typedef struct _RiskParams
	{
		uint32_t	_order_times_boundary;
		uint32_t	_order_stat_timespan;
		uint32_t	_order_total_limits;

		uint32_t	_cancel_times_boundary;
		uint32_t	_cancel_stat_timespan;
		uint32_t	_cancel_total_limits;

		_RiskParams()
		{
			memset(this, 0, sizeof(_RiskParams));
		}
	} RiskParams;

public:
	bool init(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	bool initExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	void release();

	bool run();

	inline const char* id() const{ return _id.c_str(); }

	AdapterState state() const{ return _state; }

	void addSink(ITrdNotifySink* sink)
	{
		_sinks.insert(sink);
	}

private:
	uint32_t doEntrust(WTSEntrust* entrust);
	uint32_t doQuoteEntrust(WTSEntrust* bidEntrust, WTSEntrust* askEntrust);
	bool	doCancel(WTSOrderInfo* ordInfo);

	inline void	printPosition(const char* stdCode, const PosItem& pItem);

	inline WTSContractInfo* getContract(const char* stdCode);

	inline void updateUndone(const char* stdCode, double qty);

	// F-core(2026-09-04): 平仓可平量 预扣/归还 辅助 —— 下单即预扣, 桥接
	// "报单→柜台首笔推送" 的缓存冻结窗口(该窗口内柜台冻结尚未反映到 PosItem,
	// 同 burst 多档平仓拆分按旧快照 sizing 会并发超卖 → CTP-50/51 拒单风暴)。
	// 语义与 onPushOrder 首推送扣减/撤单归还完全对齐:
	//   平今→只动 newavail; 平昨→先 preavail、溢出动 newavail (预扣);
	//   归还反向: preavail 加回到 prevol 封顶、溢出加 newavail。
	static void deductCloseAvail(PosItem& pItem, bool isLong, bool isToday, double qty);
	// F-core v3.1 C4: 归还一律钳制(newavail<=newvol / preavail<=prevol); 返回 true = 发生钳制
	static bool restoreCloseAvail(PosItem& pItem, bool isLong, bool isToday, double qty);

	// F-core v3.1 C2/C3: 平仓预冻结与按台账 exactly-once 归还
	void freezeCloseAvail(const char* stdCode, bool isLong, bool isToday, double qty, uint32_t localid);
	bool releaseCloseAvail(uint32_t localid);

	// F-core v3.1 E(D-batch-E): 平仓异常(拒单/台账未命中/归还钳制) -> 节流重查持仓再基线
	void requestPosRefresh(const char* reason);

	// F-core v3.1 D(方案1/2): 持仓/未完成量账本串行化
	//   SpinMutex 短临界区; 全部为叶子函数(不调用回调/API), 无递归加锁;
	//   下单拆分改读"快照"(锁内拷贝, 锁外发单), 回调遍历同样先快照后回调
	void updateUndoneLocked(const char* stdCode, double qty);
	void freezeCloseAvailLocked(const char* stdCode, bool isLong, bool isToday, double qty, uint32_t localid);
	PosItem getPosSnapshot(const char* stdCode);
	void deductCloseAvailByCode(const char* stdCode, bool isLong, bool isToday, double qty);
	// F-core v3.2 修复(2026-09-13): _known_localids 因 P1 变为跨线程(发单线程写 / 回报线程读)
	//   -> 读写统一走短锁(叶子函数), 杜绝 wt_hashset 并发 insert/find(rehash) 的 UB
	void markKnownLocalId(uint32_t localid);
	bool isKnownLocalId(uint32_t localid) const;

	const RiskParams* getRiskParams(const char* stdCode);

public:
	double	getPosition(const char* stdCode, bool bValidOnly, int32_t flag = 3);
	double	enumPosition(const char* stdCode = "");
	OrderMap* getOrders(const char* stdCode);
	inline double getUndoneQty(const char* stdCode)
	{
		// F-core v3.1 D: 未完成量跨线程串行化(短临界区; MdSpi/TdSpi 两侧都会读)
		SpinLock lock(_mtx_pos);
		auto it = _undone_qty.find(stdCode);
		if (it != _undone_qty.end())
			return it->second;

		return 0;
	}

	// F-core v3.2 E④(2026-09-13 修订): 周期再基线改由 **tick 入口** 驱动
	//   (原 piggyback onPushOrder 在停报/无推送期间失效, 恰是需要自愈的场景);
	//   引擎每 tick 调用, 内部仅一次时间比较, 10min 到点才发 queryPositions
	void onTickPiggyback();

	uint32_t getInfos(const char* stdCode);

	OrderIDs buy(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);
	OrderIDs sell(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);

	/*
	 *	下单接口: 开多
	 *	
	 *	@stdCode	合约代码
	 *	@price		下单价格，0则是市价单
	 *	@qty		下单数量
	 *	@flag		下单标志: 0-normal，1-fak，2-fok
	 */
	uint32_t openLong(const char* stdCode, double price, double qty, int flag);

	/*
	 *	下单接口: 开空
	 *
	 *	@stdCode	合约代码
	 *	@price		下单价格，0则是市价单
	 *	@qty		下单数量
	 *	@flag		下单标志: 0-normal，1-fak，2-fok
	 */
	uint32_t openShort(const char* stdCode, double price, double qty, int flag);

	/*
	 *	下单接口: 平多
	 *
	 *	@stdCode	合约代码
	 *	@price		下单价格，0则是市价单
	 *	@qty		下单数量
	 *	@isToday	是否今仓，默认false
	 *	@flag		下单标志: 0-normal，1-fak，2-fok，默认0
	 */
	uint32_t closeLong(const char* stdCode, double price, double qty, bool isToday, int flag);

	/*
	 *	下单接口: 平空
	 *
	 *	@stdCode	合约代码
	 *	@price		下单价格，0则是市价单
	 *	@qty		下单数量
	 *	@isToday	是否今仓，默认false
	 *	@flag		下单标志: 0-normal，1-fak，2-fok，默认0
	 */
	uint32_t closeShort(const char* stdCode, double price, double qty, bool isToday, int flag);
	
	/*
	 *	做市双边报价接口
	 *
	 *	@stdCode	合约代码
	 *	@bidPrice	买价
	 *	@bidQty		买量
	 *	@askPrice	卖价
	 *	@askQty		卖量
	 *	@flag		下单标志, 0=normal
	 *	@return		本地单号 localid (0=失败)
	 *
	 *	实现走 ITraderApi::quoteInsert 抽象, 由 broker 适配层映射到具体协议
	 *	(CTP -> ReqQuoteInsert; XTP/盈透同理). TraderAdapter 不感知 broker 类型.
	 */
	uint32_t quote(const char* stdCode, double bidPrice, double bidQty,
				   double askPrice, double askQty, int flag = 0);

	/*
	 *	撤销做市报价 (走 ITraderApi::quoteAction 抽象)
	 *	@localid	quote 本地单号
	 *	@return		是否成功提交撤单请求
	 */
	bool	cancelQuote(uint32_t localid);

	bool	cancel(uint32_t localid);
	OrderIDs cancelAll(const char* stdCode);

	inline bool	isTradeEnabled(const char* stdCode) const;

	bool	checkCancelLimits(const char* stdCode);
	bool	checkOrderLimits(const char* stdCode);

public:
	//////////////////////////////////////////////////////////////////////////
	//ITraderSpi接口
	virtual void handleEvent(WTSTraderEvent e, int32_t ec) override;

	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override;

	virtual void onLogout() override;

	virtual void onRspEntrust(WTSEntrust* entrust, WTSError *err) override;

	virtual void onRspAccount(WTSArray* ayAccounts) override;

	virtual void onRspPosition(const WTSArray* ayPositions) override;

	virtual void onRspOrders(const WTSArray* ayOrders) override;

	virtual void onRspTrades(const WTSArray* ayTrades) override;

	virtual void onPushOrder(WTSOrderInfo* orderInfo) override;

	virtual void onPushTrade(WTSTradeInfo* tradeRecord) override;

	virtual void onPushQuote(WTSEntrust* quoteInfo) override;

	virtual void onTraderError(WTSError* err, void* pData = NULL) override;

	virtual IBaseDataMgr* getBaseDataMgr() override;

	virtual void handleTraderLog(WTSLogLevel ll, const char* message) override;

private:
	WTSVariant*			_cfg;
	std::string			_id;
	std::string			_order_pattern;

	uint32_t			_trading_day;

	ITraderApi*			_trader_api;
	FuncDeleteTrader	_remover;
	AdapterState		_state;

	wt_hashset<ITrdNotifySink*>	_sinks;

	IBaseDataMgr*		_bd_mgr;
	ActionPolicyMgr*	_policy_mgr;

	wt_hashmap<std::string, PosItem> _positions;
	// F-core v3.1 C1: 平仓可用量预冻结台账 (可用量 = 柜台基线 - 在途平仓未终态量)
	CloseFreezeLedger _close_freeze;
	// F-core v3.1 D(2026-09-13 复核修订): 持仓/未完成量的跨线程串行化
	//   MdSpi 发单路径(下单即预冻结) 与 TdSpi 回报路径(归还/成交/快照) 并发访问同一容器;
	//   改用项目惯用 SpinMutex + 短临界区(无递归): 所有持锁函数均为叶子函数
	mutable SpinMutex _mtx_pos;

	// F-core v3.1 E: 持仓重查(自愈兜底)节流与计数
	uint64_t	_pos_refresh_last_ms = 0;	///< 上次触发时刻(ms), 0=尚未触发
	uint32_t	_pos_refresh_count = 0;		///< 累计触发次数
	uint32_t	_pos_refresh_skipped = 0;	///< 节流窗口内被跳过次数
	// F-core v3.2 E④(2026-09-13): 周期再基线节拍(piggyback onPushOrder, 10min)
	uint64_t	_pos_refresh_periodic_ms = 0;

	SpinMutex	_mtx_orders;
	OrderMap*	_orders;
	wt_hashset<std::string> _orderids;	//主要用于标记有没有处理过该订单
	// R15-c(2026-09-10): 本进程所下订单的 localid 集合 —— v3.2-P1 起仅 doEntrust/doQuoteEntrust 写入
	// (孤儿成交告警判据: usertag 匹配本通道但不在册 = 幽灵/残留单成交;
	//  不再于登录回放/任意推送写入, 避免幽灵单被误判"已知"而哑火)
	wt_hashset<uint32_t> _known_localids;

	wt_hashmap<std::string, double> _undone_qty;	//未完成数量

	typedef WTSHashMap<std::string>	TradeStatMap;
	TradeStatMap*	_stat_map;	//统计数据

	//这两个缓存时间内的容器,主要是为了控制瞬间流量而设置的
	typedef std::vector<uint64_t> TimeCacheList;
	typedef wt_hashmap<std::string, TimeCacheList> CodeTimeCacheMap;
	CodeTimeCacheMap	_order_time_cache;	//下单时间缓存
	CodeTimeCacheMap	_cancel_time_cache;	//撤单时间缓存

	//如果被风控了,就会进入到排除队列
	wt_hashset<std::string>	_exclude_codes;

	typedef wt_hashmap<std::string, RiskParams>	RiskParamsMap;
	RiskParamsMap	_risk_params_map;
	bool			_risk_mon_enabled;
};

typedef std::shared_ptr<TraderAdapter>					TraderAdapterPtr;
typedef wt_hashmap<std::string, TraderAdapterPtr>	TraderAdapterMap;


//////////////////////////////////////////////////////////////////////////
//TraderAdapterMgr
class TraderAdapterMgr
{
public:
	void	release();

	void	run();

	const TraderAdapterMap& getAdapters() const { return _adapters; }

	TraderAdapterPtr getAdapter(const char* tname);

	bool	addAdapter(const char* tname, TraderAdapterPtr& adapter);

private:
	TraderAdapterMap	_adapters;
};

NS_WTP_END
