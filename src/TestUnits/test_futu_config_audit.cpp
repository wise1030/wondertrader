//==============================================================================
// F-core P1-4 阶段 1(2026-09-19): 配置读取审计 (fixture 口径 = unknown 硬断言)
//   背景: "配置写了不生效"整类隐性缺陷 —— alertThrottleMs 需 5 处编辑、
//   V8-R3 perf 阈值注入后无逻辑读取、A10 setMinProfitThreshold 无调用者、
//   hotparams 键名整套静默回退。
//   口径: fixture → unknown == 0 硬断言; 生产 → 统计 + WARN + 白名单。
//==============================================================================

#include <gtest/gtest.h>

#include "../WtFutuCore/FutuConfigAudit.h"
#include "../WTSUtils/WTSCfgLoader.h"

using namespace futu;

namespace
{
const char* kFixture = R"YAML(
anchorCode: SHFE.ao.ao2611
quoting:
  baseSpread: 2.0
  baseQty: 10.0
portfolio:
  maxDelta: 30
strayKey: 1
)YAML";
} // namespace

// 未读取的键必须被识别为 unknown (fixture 口径)
TEST(FutuConfigAudit, UnknownKeyDetected)
{
	auto& audit = ConfigAudit::instance();
	audit.reset();

	wtp::WTSVariant* root = WTSCfgLoader::load_from_content(kFixture, true);
	ASSERT_NE(root, nullptr);
	audit.beginSource("t.yaml", root);

	audit.noteRead(root, "anchorCode");
	audit.noteRead(root->get("quoting"), "baseSpread");
	audit.noteRead(root->get("quoting"), "baseQty");
	audit.noteRead(root->get("portfolio"), "maxDelta");

	// quoting/portfolio 容器由子键读取覆盖; strayKey 无人读 ⇒ 1 个 unknown
	const auto unknown = audit.unknownList();
	ASSERT_EQ(unknown.size(), 1u);
	EXPECT_NE(unknown[0].find("strayKey"), std::string::npos) << unknown[0];

	root->release();
	audit.reset();
}

// 只读部分子键时, 未读的兄弟键仍应暴露 (防止"容器有读就整体放过")
TEST(FutuConfigAudit, PartiallyReadContainerStillReportsUnreadSibling)
{
	auto& audit = ConfigAudit::instance();
	audit.reset();

	wtp::WTSVariant* root = WTSCfgLoader::load_from_content(kFixture, true);
	ASSERT_NE(root, nullptr);
	audit.beginSource("t.yaml", root);

	audit.noteRead(root, "anchorCode");
	audit.noteRead(root->get("quoting"), "baseSpread"); // baseQty 未读
	audit.noteRead(root->get("portfolio"), "maxDelta");

	const auto unknown = audit.unknownList();
	ASSERT_EQ(unknown.size(), 2u); // quoting.baseQty + strayKey

	root->release();
	audit.reset();
}

// 白名单前缀 (死键/外部键) 计入豁免
TEST(FutuConfigAudit, WhitelistPrefixExempts)
{
	auto& audit = ConfigAudit::instance();
	audit.reset();

	wtp::WTSVariant* root = WTSCfgLoader::load_from_content(kFixture, true);
	ASSERT_NE(root, nullptr);
	audit.beginSource("t.yaml", root);
	audit.addWhitelistPrefix("strayKey");
	audit.addWhitelistPrefix("quoting");

	audit.noteRead(root, "anchorCode");
	audit.noteRead(root->get("portfolio"), "maxDelta");

	EXPECT_EQ(audit.unknownCount(), 0u);

	root->release();
	audit.reset();
}

// 注册表驱动的键 (热参名) 通过 noteReadPath 记账
TEST(FutuConfigAudit, RegisteredPathCountsAsRead)
{
	auto& audit = ConfigAudit::instance();
	audit.reset();

	wtp::WTSVariant* root = WTSCfgLoader::load_from_content(kFixture, true);
	ASSERT_NE(root, nullptr);
	audit.beginSource("t.yaml", root);
	audit.noteRead(root, "anchorCode");
	audit.noteRead(root->get("quoting"), "baseSpread");
	audit.noteRead(root->get("quoting"), "baseQty");
	audit.noteRead(root->get("portfolio"), "maxDelta");
	// 未随代码读取的键, 以"注册表驱动"方式记账 (热参名走这条路)
	audit.noteReadPath("t.yaml", "strayKey");

	EXPECT_EQ(audit.unknownCount(), 0u);
	EXPECT_GT(audit.readCount("t.yaml"), 0u);

	root->release();
	audit.reset();
}

// report() 之后自动 close: 后续记账近零开销 (不再改状态)
TEST(FutuConfigAudit, ReportClosesAndStopsRecording)
{
	auto& audit = ConfigAudit::instance();
	audit.reset();

	wtp::WTSVariant* root = WTSCfgLoader::load_from_content(kFixture, true);
	ASSERT_NE(root, nullptr);
	audit.beginSource("t.yaml", root);
	audit.noteRead(root, "anchorCode");

	const size_t unknown_before = audit.unknownCount();
	const size_t recorded = audit.report("unit-test");
	EXPECT_EQ(recorded, unknown_before);
	EXPECT_TRUE(audit.closed());

	const size_t reads_after = audit.readCount("t.yaml");
	audit.noteRead(root->get("portfolio"), "maxDelta"); // 已 close ⇒ 不记账
	EXPECT_EQ(audit.readCount("t.yaml"), reads_after);

	root->release();
	audit.reset();
}
