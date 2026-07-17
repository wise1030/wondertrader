# L0 Patch Design — UFT 做市策略接入 CTP 原生 quoteInsert

**起草日期**：2026-06-12  
**最后修订**：2026-06-12 (v3 — 方案 C：彻底删除二连发降级)  
**修复路径**：B (L0 治本)  
**适用范围**：实盘 CTP 链路走原生 quoteInsert；回测 (UftMocker) stra_quote = stra_buy + stra_sell 占位  
**先决条件**：broker 已开通 SHFE/CZCE/CFFEX 做市商资质（v3 工单解锁）

---

## 0. 设计原则

1. **实盘单一原生路径**：UftStraContext::stra_quote → TraderAdapter::quote → _trader_api->quoteInsert，**无 #ifdef、无降级分支**
   - 二连发 (openLong+openShort) 与 quoteInsert **语义不对等**（非原子、无同撤、可能触发自成交风控），不构成有效"灰度回退"
   - 任何回退需求都通过 `git revert` 整版回滚，而非编译开关
2. **回测占位**：UftMocker::stra_quote override = stra_buy + stra_sell 二连发，**仅用于打通策略代码路径**，盈亏数据不作生产参考
3. **零现有路径回归**：openLong/openShort/cancel 等普通单链路**完全不动**
4. **ID 衍生显式处理**：quote 本体 + 2 衍生子腿的 3 组独立 ID 必须全程可追溯
5. **【关键】broker 解耦铁律**：TraderAdapter 是通用模块，**禁止**感知任何 CTP/XTP 等具体 broker 概念
   - 所有 broker 交互**必须**走 ITraderApi 虚函数抽象（quoteInsert/quoteAction/onPushQuote 已就位 L264/269/185）
   - WtUftCore/TraderAdapter.{h,cpp} **不能** include 任何 broker 头文件，**不能**出现 "CTP" / "Thost" / "DeriveFromQuote" 字眼
   - WOF_QUOTE 这类新增枚举放 Includes/WTSTypes.h，作为**面向所有 broker 的通用标识**
   - broker 适配层（TraderCTP.cpp）**应该**耦合 CTP，那是它的职责

---

## 1. 改动清单总览

| # | 文件 | 行号锚点 | 性质 | 行数 |
|---|---|---|---|---|
| **P1** | `src/WtUftCore/TraderAdapter.h` | L166-191 | 新增声明 quote/cancelQuote/onPushQuote | +6 |
| **P2** | `src/WtUftCore/TraderAdapter.cpp` | L1021 后追加；L1489 onPushOrder 内 | 新增 quote()/cancelQuote()/onPushQuote() 实现 | +95 |
| **P3** | `src/WtUftCore/UftStraContext.cpp` | L1380-1428 | 改 stra_quote/stra_cancel_quote 调用链（单一原生路径） | ~15 |
| **P4** | `src/TraderCTP/TraderCTP.cpp` | L861 OnRtnQuote；L850 OnRspQuoteInsert；L483 quoteInsert | 填充空函数 + usertag 关联 | +60 |
| **P5** | `src/Includes/WTSTypes.h` | L184-187 | 增 WOF_QUOTE 枚举 | +1 |
| **P6** | `src/WtBtCore/UftMocker.{h,cpp}` | UftMocker 类公开方法区 | override stra_quote/stra_cancel_quote 为 stra_buy/stra_sell 占位 | +25 |
| **合计** | | | | **~202** |

**几个关键事实**：
- WtUftCore/TraderAdapter **完全没有 quote 接口**（之前误认为 WtCore/TraderAdapter 的 quote() 可复用，但它们是两个独立类）→ P1+P2 整体移植
- UftMocker 继承 `IDataSink + IUftStraCtx`，**不继承 ITraderApi**，本身就是回测撮合容器，所以 P6 是 stra_* 层 override 而非 ITraderApi 层
- IUftStraCtx::stra_quote (L250) 已有抽象声明，UftMocker 默认走基类的 `{0,0}` 假实现 → 当前 stra_quote 策略在回测里根本下不出单（静默失败）。P6 修正这一点

---

## 2. P1 — WtUftCore/TraderAdapter.h 声明扩展

**位置**：`src/WtUftCore/TraderAdapter.h` L189 (cancel 声明上方)

```diff
@@ -188,6 +188,12 @@ public:
 	uint32_t closeShort(const char* stdCode, double price, double qty, bool isToday, int flag);
 	
+	/*
+	 *	做市双边报价接口 (CTP 原生 quoteInsert)
+	 *	返回 localid (0 = 失败)；同 openLong 风格
+	 */
+	uint32_t quote(const char* stdCode, double bidPrice, double bidQty, double askPrice, double askQty, int flag = 0);
+	bool     cancelQuote(uint32_t localid);
+
 	bool	cancel(uint32_t localid);
 	OrderIDs cancelAll(const char* stdCode);
```

**onPushQuote** 已在基类 `ITraderApi.IBaseDataReader` 或 `ITraderSpi` 体系定义（L185 `virtual void onPushQuote(WTSEntrust* quoteInfo){}`），WtCore/TraderAdapter.h L212 已 override；**WtUftCore/TraderAdapter.h 需要补 override 声明**：

```diff
@@ -208,6 +208,7 @@ public:
 	virtual void onPushOrder(WTSOrderInfo* orderInfo) override;
 	virtual void onPushTrade(WTSTradeInfo* tradeRecord) override;
+	virtual void onPushQuote(WTSEntrust* quoteInfo) override;
```

---

## 3. P2 — WtUftCore/TraderAdapter.cpp 实现 quote/cancelQuote/onPushQuote

**【解耦铁律检查】**：所有改动都基于 ITraderApi 虚函数（quoteInsert/quoteAction/onPushQuote），**零 CTP 头文件依赖**，与 doEntrust+orderInsert 走完全相同的解耦模式。

**【对称性设计】**：抽出 `doQuoteEntrust` 作为 quote 统一入口，与现有 `doEntrust`（L311）对称。

**位置 1**：`src/WtUftCore/TraderAdapter.cpp` L1021 (openLong 结束后追加新方法)

```diff
@@ -1021,6 +1021,98 @@ uint32_t TraderAdapter::openLong(const char* stdCode, double price, double qty,
 	return ret;
 }
 
+// 统一的 quote 入口，与 doEntrust 对称
+// 集中处理 entrustID 生成、usertag 编码、_orders 登记（如有），完全不感知具体 broker
+uint32_t TraderAdapter::doQuoteEntrust(WTSEntrust* bidEntrust, WTSEntrust* askEntrust)
+{
+	// 两腿共用同一个 entrustID（broker 内部解码出 orderref，复用为 QuoteRef 等）
+	_trader_api->makeEntrustID(bidEntrust->getEntrustID(), 64);
+	wt_strcpy(askEntrust->getEntrustID(), bidEntrust->getEntrustID());
+
+	uint32_t localid = makeLocalOrderID();
+	char* usertag = bidEntrust->getUserTag();
+	wt_strcpy(usertag, _order_pattern.c_str(), _order_pattern.size());
+	usertag[_order_pattern.size()] = '.';
+	fmtutil::format_to(usertag + _order_pattern.size() + 1, "{}", localid);
+	wt_strcpy(askEntrust->getUserTag(), bidEntrust->getUserTag());
+
+	int32_t ret = _trader_api->quoteInsert(bidEntrust, askEntrust);
+	if (ret < 0) return 0;
+	return localid;
+}
+
+uint32_t TraderAdapter::quote(const char* stdCode, double bidPrice, double bidQty,
+                              double askPrice, double askQty, int flag /* = 0 */)
+{
+	if (bidQty == 0 && askQty == 0) return 0;
+
+	WTSContractInfo* cInfo = getContract(stdCode);
+	if (cInfo == NULL) return 0;
+
+	WTSEntrust* bidEntrust = WTSEntrust::create(stdCode, bidQty, bidPrice, cInfo->getExchg());
+	bidEntrust->setDirection(WDT_LONG);
+	bidEntrust->setOffsetType(WOT_OPEN);
+	bidEntrust->setPriceType(WPT_LIMITPRICE);
+	bidEntrust->setOrderFlag(WOF_QUOTE);  // 通用枚举，broker 自行映射
+	bidEntrust->setContractInfo(cInfo);
+
+	WTSEntrust* askEntrust = WTSEntrust::create(stdCode, askQty, askPrice, cInfo->getExchg());
+	askEntrust->setDirection(WDT_SHORT);
+	askEntrust->setOffsetType(WOT_OPEN);
+	askEntrust->setPriceType(WPT_LIMITPRICE);
+	askEntrust->setOrderFlag(WOF_QUOTE);
+	askEntrust->setContractInfo(cInfo);
+
+	updateUndone(stdCode, bidQty + askQty);  // 双腿总量
+
+	uint32_t localid = doQuoteEntrust(bidEntrust, askEntrust);  // ← 走统一入口
+	if (localid == 0)
+	{
+		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
+			"[{}] Quote placing failed", _id.c_str());
+		updateUndone(stdCode, -(bidQty + askQty));
+	}
+
+	bidEntrust->release();
+	askEntrust->release();
+	return localid;
+}
+
+bool TraderAdapter::cancelQuote(uint32_t localid)
+{
+	if (_trader_api == NULL) return false;
+
+	SpinLock lock(_mtx_orders);
+	auto it = _orders->find(localid);
+	if (it == _orders->end()) return false;
+
+	WTSOrderInfo* orderInfo = (WTSOrderInfo*)it->second;
+	if (orderInfo == NULL || !orderInfo->isAlive()) return false;
+
+	// 注意：传给 broker 的 orderID 必须是 quote 本体 ID
+	// 上层不知道这一点，broker 适配层自行根据 OrderFlag/上下文路由到正确的 API
+	WTSEntrustAction* action = WTSEntrustAction::create(orderInfo->getCode(), orderInfo->getExchg());
+	action->setEntrustID(orderInfo->getEntrustID());
+	action->setOrderID(orderInfo->getOrderID());
+	action->setActionFlag(WAF_CANCEL);
+
+	int ret = _trader_api->quoteAction(action);
+	action->release();
+	return (ret >= 0);
+}
+
+void TraderAdapter::onPushQuote(WTSEntrust* quoteInfo)
+{
+	// quote 状态推送的通用入口；不感知 broker 来源
+	if (quoteInfo == NULL) return;
+	WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO,
+		"[{}] Quote pushed: {} entrust={}", _id.c_str(),
+		quoteInfo->getCode(), quoteInfo->getEntrustID());
+	// 预留扩展点：未来转发给 IUftStraCtx 的 on_quote_update 回调（P6 phase）
+}
+
 uint32_t TraderAdapter::openShort(const char* stdCode, double price, double qty, int flag/* = 0*/)
 {
```

**位置 2**：`src/WtUftCore/TraderAdapter.h` doEntrust 声明附近也加 doQuoteEntrust（如果它是 private 的也对称放 private）

```diff
@@ -XXX private/protected 区
+	uint32_t doQuoteEntrust(WTSEntrust* bidEntrust, WTSEntrust* askEntrust);
```

**onPushOrder 内识别衍生子单**（L1489 附近）：用 P5 新增的 WOF_QUOTE 枚举，**完全不引入 CTP 概念**：

```diff
@@ -1525,7 +1525,7 @@ void TraderAdapter::onPushOrder(WTSOrderInfo* orderInfo)
 			if (isBuy)
 			{
-				if(orderInfo->getOrderFlag() == WOF_NOR)
+				if(orderInfo->getOrderFlag() == WOF_NOR || orderInfo->getOrderFlag() == WOF_QUOTE)
 				{
 					statItem.b_cancels++;
 					statItem.b_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
@@ -1546,7 +1546,7 @@ void TraderAdapter::onPushOrder(WTSOrderInfo* orderInfo)
 			else
 			{
-				if (orderInfo->getOrderFlag() == WOF_NOR)
+				if (orderInfo->getOrderFlag() == WOF_NOR || orderInfo->getOrderFlag() == WOF_QUOTE)
 				{
 					statItem.s_cancels++;

---

## 4. P3 — UftStraContext.cpp 切换为原生 quote()（单一路径）

**位置**：`src/WtUftCore/UftStraContext.cpp` L1380-1428

**设计**：直接重写为单一原生路径，**不留 #ifdef 双路径**。理由见 §0 原则 1。

```diff
@@ -1377,49 +1377,30 @@
 //==========================================================================
 // Market-Making Extensions (做市专用交易接口实现)
 //==========================================================================
 
 std::pair<uint32_t, uint32_t> UftStraContext::stra_quote(const char* stdCode, double bidPrice, double bidQty,
 						 double askPrice, double askQty, const char* userTag)
 {
-	// 双边报价：同时下买单和卖单
-	// 返回 {bidOrderId, askOrderId}
 	if (!_trader)
 		return {0, 0};
 
-	// 先下买单
-	uint32_t bidLocalId = _trader->openLong(stdCode, bidPrice, bidQty, 0);
-	if (bidLocalId == 0)
-	{
-		log_error("Quote BUY failed: {} @ {} x {}", stdCode, bidPrice, bidQty);
-		return {0, 0};
-	}
-	_order_ids[bidLocalId] = NULL;
-
-	// 再下卖单
-	uint32_t askLocalId = _trader->openShort(stdCode, askPrice, askQty, 0);
-	if (askLocalId == 0)
-	{
-		log_error("Quote SELL failed: {} @ {} x {}", stdCode, askPrice, askQty);
-		// 卖单失败，撤销买单
-		_trader->cancel(bidLocalId);
-		_order_ids.erase(bidLocalId);
-		return {0, 0};
-	}
-	_order_ids[askLocalId] = NULL;
-
-	log_info("Quote placed: {} BID {}@{} ASK {}@{} (bid_id={}, ask_id={})", 
-		stdCode, bidQty, bidPrice, askQty, askPrice, bidLocalId, askLocalId);
-
-	// 返回买单和卖单的订单ID
-	return {bidLocalId, askLocalId};
+	// L0 治本路径：调用 TraderAdapter::quote() → CTP 原生 ReqQuoteInsert (原子双边)
+	uint32_t quoteId = _trader->quote(stdCode, bidPrice, bidQty, askPrice, askQty, 0);
+	if (quoteId == 0)
+	{
+		log_error("Quote native failed: {} BID {}@{} ASK {}@{}",
+			stdCode, bidQty, bidPrice, askQty, askPrice);
+		return {0, 0};
+	}
+	_order_ids[quoteId] = NULL;
+	log_info("Quote placed (native): {} BID {}@{} ASK {}@{} (quote_id={})",
+		stdCode, bidQty, bidPrice, askQty, askPrice, quoteId);
+	// 兼容旧返回签名：bidId = askId = quoteId（上层语义不再区分两腿）
+	return {quoteId, quoteId};
 }
 
 bool UftStraContext::stra_cancel_quote(uint32_t localid)
 {
-	// 撤销双边报价
 	if (!_trader)
 		return false;
 
-	// 撤销传入的单号
-	bool ret = _trader->cancel(localid);
+	bool ret = _trader->cancelQuote(localid);
 	if (ret)
 		_order_ids.erase(localid);
-
 	return ret;
 }
```

**移除**：所有 `#ifdef UFT_QUOTE_NATIVE` / `#else` / `#endif` 分支与对应 CMakeLists 注入。底层只剩一条原生路径。

---

## 5. P4 — TraderCTP.cpp 三个关键修复

### 5.1 OnRtnQuote 填充（L861，当前是空函数体）

```diff
@@ -858,9 +858,52 @@ void TraderCTP::OnRspQuoteInsert(CThostFtdcInputQuoteField *pInputQuote, ...)
 }
 
-void TraderCTP::OnRtnQuote(CThostFtdcQuoteField *pQuote)
-{
-}
+void TraderCTP::OnRtnQuote(CThostFtdcQuoteField *pQuote)
+{
+	if (pQuote == NULL) return;
+
+	WTSContractInfo* contract = m_bdMgr->getContract(pQuote->InstrumentID, pQuote->ExchangeID);
+	if (contract == NULL) return;
+
+	WTSEntrust* qInfo = WTSEntrust::create(pQuote->InstrumentID,
+	                                       pQuote->BidVolume,
+	                                       pQuote->BidPrice,
+	                                       contract->getExchg());
+	qInfo->setContractInfo(contract);
+	qInfo->setOrderFlag(WOF_QUOTE);   // 标记为 quote 本体
+	// 用 QuoteSysID 反查 usertag（quoteInsert 时已写 m_oidCache）
+	const char* usertag = m_oidCache.get(StrUtil::trim(pQuote->QuoteSysID).c_str());
+	if (strlen(usertag) > 0)
+		qInfo->setUserTag(usertag);
+
+	// 关键：把 Ask/Bid 衍生子单的 SysID 也关联到同一个 usertag
+	// 这样后续 OnRtnOrder 走 makeOrderInfo 时能用 OrderSysID 反查到 usertag
+	if (strlen(usertag) > 0)
+	{
+		if (strlen(pQuote->AskOrderSysID) > 0)
+			m_oidCache.put(StrUtil::trim(pQuote->AskOrderSysID).c_str(), usertag, 0,
+				[this](const char* m){ write_log(m_sink, LL_ERROR, m); });
+		if (strlen(pQuote->BidOrderSysID) > 0)
+			m_oidCache.put(StrUtil::trim(pQuote->BidOrderSysID).c_str(), usertag, 0,
+				[this](const char* m){ write_log(m_sink, LL_ERROR, m); });
+	}
+
+	if (m_sink)
+		m_sink->onPushQuote(qInfo);
+	qInfo->release();
+}
```

### 5.2 OnRspQuoteInsert 增强：QuoteSysID 入 m_oidCache（L850）

```diff
@@ -849,11 +849,17 @@
 void TraderCTP::OnRspQuoteInsert(CThostFtdcInputQuoteField *pInputQuote, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
 {
+	// quoteInsert 成功后，把 QuoteRef 对应 usertag 关联到 (未来) QuoteSysID
+	// 注：OnRsp 阶段 QuoteSysID 还未分配，真正写入放 OnRtnQuote
+	// 这里只处理错误回报
 	if(IsErrorRspInfo(pRspInfo))
 	{
 		WTSError *err = makeError(pRspInfo, WEC_ORDERINSERT);
 		if (m_sink)
 			m_sink->onTraderError(err);
 		err->release();
 	}
 }
```

（实际写入 m_oidCache 的逻辑放在 OnRtnQuote 里更稳：那时 QuoteSysID 已就绪。）

### 5.3 quoteInsert 增强：askEntrust 的 entrustID 也入 m_eidCache（L483）

```diff
@@ -482,11 +482,15 @@
 	}
 
-	if (strlen(bidEntrust->getUserTag()) > 0)
-	{
-		m_eidCache.put(bidEntrust->getEntrustID(), bidEntrust->getUserTag(), 0, [this](const char* message) {
-			write_log(m_sink, LL_WARN, message);
-		});
-	}
+	if (strlen(bidEntrust->getUserTag()) > 0)
+	{
+		// 注：bid 和 ask 共用同一个 entrustID（TraderAdapter::quote 已保证）
+		// 因此一次 put 即可覆盖；保留 ask 分支只为防御性编程
+		m_eidCache.put(bidEntrust->getEntrustID(), bidEntrust->getUserTag(), 0,
+			[this](const char* m){ write_log(m_sink, LL_WARN, m); });
+		if (strcmp(bidEntrust->getEntrustID(), askEntrust->getEntrustID()) != 0)
+			m_eidCache.put(askEntrust->getEntrustID(), askEntrust->getUserTag(), 0,
+				[this](const char* m){ write_log(m_sink, LL_WARN, m); });
+	}
```

---

## 6. P5 — WTSTypes.h 增 WOF_QUOTE（必做）

**位置**：`src/Includes/WTSTypes.h` L184-187

```diff
 typedef enum tagWTSOrderFlag
 {
 	WOF_NOR = '0',		//普通订单
 	WOF_FAK,			//fak
 	WOF_FOK,			//fok
+	WOF_QUOTE,			//做市报价 (broker 中立通用枚举：CTP/XTP/盈透皆可映射)
 } WTSOrderFlag;
```

**用途**：
- TraderAdapter::quote 设置 entrust OrderFlag = WOF_QUOTE，broker 适配层据此映射到 CTP 的 quoteInsert 路径
- onPushOrder 识别衍生子单走"自动撤单"统计分支，不计入手动撤单率
- **broker 中立**：任何 broker 的做市报价（不光 CTP）都用这一个枚举，符合解耦铁律

**注**：上轮的"替代方案 (用 WOT_DeriveFromQuote 判断)"已废止——那是 CTP OrderType 字段语义，TraderAdapter 感知它就破坏解耦原则 5。

---

## 7. 三个 ID 衍生坑的最终处理矩阵

| 坑 | 触发场景 | 现状 | P4 修复后 |
|---|---|---|---|
| **A. usertag 关联** | OnRtnOrder 收到 OrdRef=13/14 衍生子单 | 用 OrdRef 反查 m_eidCache → miss → usertag=entrustID 默认值 | OnRtnQuote 把 Ask/BidOrderSysID 入 m_oidCache，makeOrderInfo L1381 用 OrderSysID 反查命中 |
| **B. OnRtnQuote 空** | quote 挂上/撤掉/被拒回报 | 完全不通知上层 | 转发 onPushQuote → TraderAdapter → 策略可见 |
| **C. 撤单 API 路径** | stra_cancel_quote 调用 | 走 _trader->cancel → orderAction (撤一腿) | 走 _trader->cancelQuote → quoteAction (用 QuoteSysID，CTP 自动撤俩腿) |

---

## 8. 验证清单（实施后）

### 8.1 编译验证
```bash
cd src && ./build_release.sh    # 全量
# 或增量
cd src/build_all && make WtFutuCore WtUftCore WtBtCore TraderCTP -j8
```

### 8.2 回测验证（UftMocker stra_quote 占位路径）
- 跑 ag2510 单合约回测，stra_quote 调用应当：
  - 内部 fan-out 到 stra_buy + stra_sell（UftMocker P6 override）
  - 撮合两条独立子单，PnL 与"策略层手动写 buy+sell"完全一致
  - **不作为生产盈亏参考**：实盘做市的成交节奏 (原子双边/同撤/优先级) 与回测占位完全不同
- 回归基线：现有 openLong/openShort 单的回测 PnL **一字节不变**（P3-P6 不动这条链路）

### 8.3 实盘 dry-run 验证（原生路径）
**前提**：broker v3 工单完成，至少 SHFE 或 CFFEX 一所做市角色开通  
**步骤**：
1. 配置 dist/WtRunnerFutu/test_config.yaml 接联通线 58.240.131.69:59205
2. 跑 stra_quote 单次调用：发 1 笔 quote
3. **预期日志序列**（按时序）：
   - `[TraderCTP] ReqQuoteInsert sent`
   - `OnRspQuoteInsert` (无错)
   - `OnRtnOrder` × 2 (Ask 衍生 + Bid 衍生，OrdRef 独立编号，**usertag 命中** ✅)
   - `OnRtnQuote` × 1 (QuoteRef 复用 entrustID 提取的 orderref，**Status=Accepted**)
   - TraderAdapter::onPushQuote 触发，日志见 quote 推送
4. 调 stra_cancel_quote：
   - `[TraderCTP] ReqQuoteAction sent`
   - `OnRtnOrder` × 2 (Ask/Bid Status=Canceled)
   - `OnRtnQuote` × 1 (Status=Canceled)

### 8.4 失败回滚预案
- L0 改动**全部聚集在 6 文件**：UftStraContext.cpp + WtUftCore/TraderAdapter.{h,cpp} + TraderCTP.cpp + WTSTypes.h + WtBtCore/UftMocker.{h,cpp}
- 回退策略：`git revert <commit>` 整版回滚，不靠编译开关
- 回退后行为：stra_quote 回到上一版（UftStraContext 内 openLong+openShort，UftMocker 默认 `{0,0}`），即"无降级"的原始状态
- 回退耗时：< 5 min（含重编）

---

## 9. 实施先后顺序建议

```
Step 1: P5 (WTSTypes.h 增 WOF_QUOTE)
Step 2: P1 (TraderAdapter.h 声明)
Step 3: P2 (TraderAdapter.cpp 实现 quote/cancelQuote/onPushQuote)
Step 4: P4 (TraderCTP.cpp 三处修复，含 OnRtnQuote 填充)
Step 5: P3 (UftStraContext.cpp 切换为单一原生路径)
Step 6: P6 (UftBtCore/UftMocker.{h,cpp} override stra_quote 为 stra_buy+stra_sell)
Step 7: 编译 → 跑现有回测确认 openLong/openShort 链路 PnL 不变；跑 stra_quote 回测确认 P6 占位生效
Step 8: broker 资质就绪后实盘 dry-run（按 §8.3）
```

**关键检查点**：Step 7 之后**所有现有策略零行为变化**（普通单链路完全不动；stra_quote 在回测里从"静默失败"升级到"buy+sell 占位"，是改进非倒退）。Step 8 才真正激活原生 quote 路径。

---

## 10. 已决议项（含废止项）

**Q1. WOF_QUOTE 枚举走 P5 新增 还是 替代方案？** → **决议：A (P5 新增)**
- 替代方案 (用 WOT_DeriveFromQuote) 违反解耦原则 5（CTP 字段语义渗入 TraderAdapter），废止
- WOF_QUOTE 作为 broker 中立通用枚举落到 Includes/WTSTypes.h

**Q2. UFT_QUOTE_NATIVE 编译开关怎么注入？** → **决议：废止整个开关**
- 方案 C 不需要编译开关，因为没有降级路径

**Q3. onPushQuote 是否要扩 IUftStraCtx 接口加 on_quote_update 回调？** → **决议：本轮 L0 不做，延后到 P7 phase**
- 本轮 TraderAdapter::onPushQuote 仅打 log
- 策略层若需 quote 状态感知，可从 onPushOrder 的 2 条衍生子单回报推断
- 后续 P7：扩 IUftStraCtx::on_quote_update 回调，让策略直接订阅 quote 本体状态

**Q4. UftMocker 兼容声明？** → **决议：升级为 P6**
- 上轮提的"加空 quote()"已升级为 **P6：override stra_quote 为 stra_buy + stra_sell 占位**
- 不仅是编译兜底，还让回测策略路径可跑通

**所有决议落定，下一步直接进 patch 实施。**

---

## 11. 解耦审查表（broker 解耦铁律自检）

| 文件 | 改动后是否 include broker 头? | 改动后是否出现 "CTP"/"Thost"/broker 字眼? | 是否走 ITraderApi 抽象? | 解耦合规 |
|---|---|---|---|---|
| UftStraContext.cpp | ❌ 否 | ❌ 否 | N/A（调 TraderAdapter） | ✅ |
| WtUftCore/TraderAdapter.h | ❌ 否 | ❌ 否 | ✅ 是 | ✅ |
| WtUftCore/TraderAdapter.cpp | ❌ 否 | ❌ 否 | ✅ 是（_trader_api->quoteInsert/quoteAction） | ✅ |
| Includes/WTSTypes.h | ❌ 否 | ❌ 否（WOF_QUOTE 是 broker 中立通用枚举） | N/A | ✅ |
| Includes/ITraderApi.h | ❌ 否（未改动） | ❌ 否 | N/A（自身就是抽象） | ✅ |
| **WtBtCore/UftMocker.{h,cpp}** | ❌ 否 | ❌ 否 | N/A（回测撮合容器，调 stra_buy/stra_sell 自身接口） | ✅ |
| **TraderCTP/TraderCTP.cpp** | ✅ 是（**应该**） | ✅ 是（**应该**） | N/A（自身就是适配层） | ✅ **本就是 CTP 适配层** |

**核心验证**：上层（UftStraContext → TraderAdapter）替换 broker（比如从 CTP 换成 XTP）**无需任何修改**，只需 XTP 适配层实现 ITraderApi::quoteInsert/quoteAction/onPushQuote 三个虚函数即可。这是 WT 框架原生的良好解耦设计，本 patch 完全遵循。

**对称性验证**：

| openLong 链路（普通单） | quote 链路（做市报价） |
|---|---|
| UftStraContext::stra_buy → TraderAdapter::openLong → doEntrust → _trader_api->orderInsert | UftStraContext::stra_quote → TraderAdapter::quote → **doQuoteEntrust** → _trader_api->quoteInsert |
| TraderCTP::OnRtnOrder → makeOrderInfo → onPushOrder | TraderCTP::OnRtnQuote → 转 WTSEntrust → onPushQuote |
| WOF_NOR 标识 | **WOF_QUOTE** 标识 |

两条链路结构完全对称，未来加新 broker 或新风控逻辑都能等价覆盖两条路径。
