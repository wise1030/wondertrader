//==============================================================================
// F-core P1-4 阶段 3(2026-09-19): 配置契约测试
//   目的: 把"权威配置必须与代码读取集一致"变成可执行断言, 防住两类历史缺陷:
//     ① 代码读了但权威配置漏键 ⇒ 静默用默认值 (19.10 审计发现 live 22 处; 本次又发现
//        权威 src 漏 performance.alertThrottleMs / zombieLatch.maxHoldMs 两个键)
//     ② hotparams.yaml 出现"未注册的第三类键" ⇒ 热更静默失效 (当前已知死键仅 2 个)
//   另外固化 P1-4 阶段 2 的"派生值单一来源"行为 (min_profit_threshold_ticks)。
//==============================================================================

#include <gtest/gtest.h>

#include <string>
#include <algorithm>
#include <vector>

#include "../WTSUtils/WTSCfgLoader.h"
#include "../WtFutuCore/FutuHotParamManager.h"
#include "../WtFutuCore/arb/AsyncArbitrageExecutor.h"
#include "../WtFutuCore/arb/SpreadArbitrageManager.h"

using namespace futu;

namespace
{
/// 由测试源文件路径推导权威配置目录 (src/WtFutuCore/config/)
std::string authorityDir()
{
	const std::string src(__FILE__);
	const size_t slash = src.find_last_of('/');
	const std::string dir = (slash == std::string::npos) ? std::string(".") : src.substr(0, slash);
	return dir + "/../WtFutuCore/config/";
}

bool hasKey(wtp::WTSVariant* node, const char* key) { return node != nullptr && node->get(key) != nullptr; }

/// 断言一组键存在; 失败信息逐键列出, 便于定位
void expectKeys(wtp::WTSVariant* node, const std::vector<const char*>& keys, const char* what)
{
	ASSERT_NE(node, nullptr) << what << ": 节点缺失";
	for (const char* k : keys)
		EXPECT_TRUE(hasKey(node, k)) << what << ": 权威配置缺键 '" << k << "' (代码读了但配置没写 ⇒ 静默用默认值)";
}
} // namespace

// 权威 src 配置: 四源存在 + 关键键齐备 (P1-4 显式化清单)
TEST(FutuConfigContract, AuthorityConfigsContainAllExplicitKeys)
{
	const std::string dir = authorityDir();

	// ---- 主配置: 策略参数节点 ----
	wtp::WTSVariant* root = WTSCfgLoader::load_from_file(dir + "config.yaml");
	ASSERT_NE(root, nullptr) << "无法加载 " << dir << "config.yaml";
	wtp::WTSVariant* strategies = root->get("strategies");
	ASSERT_NE(strategies, nullptr);
	wtp::WTSVariant* uft = strategies->get("uft");
	ASSERT_NE(uft, nullptr);
	wtp::WTSVariant* params = uft->get(static_cast<uint32_t>(0));
	ASSERT_NE(params, nullptr);
	params = params->get("params");

	expectKeys(params, {"maxOrders", "maxPendingPerSide"}, "config.yaml 根级");
	expectKeys(params->get("logging"), {"hotQuoteEnabled", "hotQuoteSampleEvery"}, "config.yaml logging");
	expectKeys(params->get("performance"), {"alertThrottleMs", "criticalThresholdNs"}, "config.yaml performance");
	expectKeys(params->get("quoting"),
	           {"levelQtyMultiplier", "obligationLevel", "scoutQty", "improveRetreatRatio", "maxPriceDeviation",
	            "useBilateralQuote", "obligationMaxSpreadTicks"},
	           "config.yaml quoting");
	expectKeys(params->get("closeout"),
	           {"closeOnly", "drainTimeoutMs", "sweepThresholdMs", "sweepTicks", "depthRatioPassive", "depthRatioMid",
	            "depthRatioAggressive"},
	           "config.yaml closeout");
	expectKeys(params->get("risk")->get("frequency"),
	           {"checkIntervalMs", "cooldownMs", "recoveryThreshold", "orderRatePauseMs",
	            "autoClearIrreversibleOnReset", "maxConsecutiveSameSide", "sameSideWindowMs", "sameSidePauseMs",
	            "positionWarningL1", "positionWarningL2", "positionHardBlockRatio"},
	           "config.yaml risk.frequency");
	root->release();

	// ---- coordinator.yaml ----
	wtp::WTSVariant* croot = WTSCfgLoader::load_from_file(dir + "coordinator.yaml");
	ASSERT_NE(croot, nullptr);
	wtp::WTSVariant* coord = croot->get("coordinator");
	ASSERT_NE(coord, nullptr);
	wtp::WTSVariant* modules = coord->get("modules");
	expectKeys(modules->get("zombieLatch"), {"reprobeMs", "telemetryMs", "maxHoldMs"}, "coordinator zombieLatch");
	wtp::WTSVariant* sig = modules->get("signalAggregator");
	expectKeys(sig->get("model"), {"rollingWindow", "rollingInterval", "icUpdateInterval"}, "coordinator model");
	expectKeys(sig->get("signals")->get("lead_lag"), {"window", "maxAgeMs"}, "coordinator lead_lag");
	croot->release();

	// ---- spread_arbitrage.yaml ----
	wtp::WTSVariant* aroot = WTSCfgLoader::load_from_file(dir + "spread_arbitrage.yaml");
	ASSERT_NE(aroot, nullptr);
	wtp::WTSVariant* arb = aroot->get("spread_arbitrage");
	ASSERT_NE(arb, nullptr);
	expectKeys(arb, {"minProfitThresholdTicks", "minSignalConfidence", "signalCooldownMs"}, "spread_arbitrage 根级");
	expectKeys(arb->get("asyncExecutor"),
	           {"signalIntervalUs", "maxWaitUs", "ticksPerSignal", "waitMode", "tickQueueSize", "orderQueueSize"},
	           "spread_arbitrage asyncExecutor");
	wtp::WTSVariant* pair0 = arb->get("pairs")->get(static_cast<uint32_t>(0));
	expectKeys(pair0, {"leg1Multiplier", "leg2Multiplier", "ratio2"}, "spread_arbitrage pairs[0]");
	aroot->release();
}

// hotparams: 文件键 = 注册表 ∪ 已知死键 (不得出现"未注册的第三类键")
TEST(FutuConfigContract, HotParamsFileMatchesRegistryAndKnownDeadKeys)
{
	const std::string dir = authorityDir();
	wtp::WTSVariant* hroot = WTSCfgLoader::load_from_file(dir + "hotparams.yaml");
	ASSERT_NE(hroot, nullptr);

	std::vector<std::string> registered;
	const char* const* names = FutuHotParamManager::paramNames();
	for (uint32_t i = 0; i < static_cast<uint32_t>(HP_COUNT); ++i)
		registered.push_back(names[i]);

	// 已知死键 (审计确认; 见 CONFIG.md §7)
	const std::vector<std::string> known_dead = {"delta_skew_threshold", "delta_skew_factor"};

	uint32_t unknown = 0;
	for (const auto& key : hroot->memberNames())
	{
		const bool reg = std::find(registered.begin(), registered.end(), key) != registered.end();
		const bool dead = std::find(known_dead.begin(), known_dead.end(), key) != known_dead.end();
		if (!reg && !dead)
		{
			++unknown;
			ADD_FAILURE() << "hotparams.yaml 出现未注册键 '" << key
			              << "' (热更静默失效) —— 请注册或从文件删除, 并同步 CONFIG.md §7";
		}
	}

	// 反向: 注册表里的键都应在文件里出现 (否则该参数恒取 config/coordinator 默认值)
	for (const auto& key : registered)
	{
		bool found = false;
		for (const auto& k : hroot->memberNames())
			if (k == key)
			{
				found = true;
				break;
			}
		if (!found)
			ADD_FAILURE() << "注册热参 '" << key << "' 未出现在 hotparams.yaml";
	}

	EXPECT_EQ(unknown, 0u);
	hroot->release();
}

// 派生值单一来源: executor 实时读 manager (P1-4 阶段 2), manager 未接线时回退注入值
TEST(FutuConfigContract, MinProfitThresholdHasSingleSource)
{
	SpreadArbitrageManager mgr;
	SpreadArbitrageConfig cfg;
	cfg.min_profit_threshold_ticks = 2.5;
	mgr.setConfig(cfg);

	AsyncArbitrageExecutor exec;
	exec.setArbitrageManager(&mgr);
	EXPECT_DOUBLE_EQ(exec.minProfitThreshold(), 2.5);

	// manager 侧变更必须立刻可见 (此前是 assembly 期一次性拷贝 ⇒ 变更不传导)
	cfg.min_profit_threshold_ticks = 7.0;
	mgr.setConfig(cfg);
	EXPECT_DOUBLE_EQ(exec.minProfitThreshold(), 7.0);

	// 未接线回退到注入值 (保持既有行为/单测可用性)
	exec.setArbitrageManager(nullptr);
	exec.setMinProfitThreshold(3.0);
	EXPECT_DOUBLE_EQ(exec.minProfitThreshold(), 3.0);
}
