/*!
 * \file test_hot_param_manager.cpp
 * \brief V8-P0-1 热参数收编 unit tests
 *
 * 背景: watcher 线程曾直接 applyAll 裸写主链路状态 (唯一绕过 _cb_mtx 的写者),
 *       且无值校验 ("abc"->0.0、负值照收)、mtime 秒粒度丢同秒二次修改。
 *
 * 收编后契约:
 *   - syncFromFile 只写共享内存 + 置 pending, 不再 applyAll
 *   - 值比对去重 (重复 sync / 同值修改零写入)
 *   - parseHotParamFile: 越界/NaN/非数值类型拒收, 未知键忽略, 解析失败显式返回
 */
#include "../WtFutuCore/FutuHotParamManager.h"
#include "gtest/gtest.h"

#include <cstdio>
#include <fstream>
#include <vector>

using namespace futu;

namespace
{

// 绑定自有 double 槽位作为共享内存替身 (绕开需要 IUftStraCtx 的 registerParams)
class TestableHotParamManager : public FutuHotParamManager
{
public:
    void bind(uint32_t idx, double init)
    {
        _slots[idx] = init;
        _hot_params[idx].ptr = &_slots[idx];
    }
    double slot(uint32_t idx) const { return _slots[idx]; }

private:
    double _slots[HP_COUNT]{};
};

// 写临时 yaml 并返回路径
std::string writeTmpYaml(const char* content)
{
    static int seq = 0;
    std::string path = "/tmp/opencode/hp_test_" + std::to_string(++seq) + ".yaml";
    std::ofstream f(path);
    f << content;
    return path;
}

} // namespace

// 合法值 + 未知键忽略
TEST(HotParamFile, ParsesValidValuesAndIgnoresUnknownKeys)
{
    std::string path = writeTmpYaml("base_spread: 3.5\nbase_qty: 6\nfoo: 1\n");
    std::vector<std::pair<uint32_t, double>> out;
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(path.c_str(), out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first, static_cast<uint32_t>(HP_BASE_SPREAD));
    EXPECT_DOUBLE_EQ(out[0].second, 3.5);
    EXPECT_EQ(out[1].first, static_cast<uint32_t>(HP_BASE_QTY));
    EXPECT_DOUBLE_EQ(out[1].second, 6.0);
}

// 越界拒收 (负值 / 超上界)
TEST(HotParamFile, RejectsOutOfRangeValues)
{
    std::string path = writeTmpYaml("base_spread: -1\nphi: 2.0\nbase_qty: 5\n");
    std::vector<std::pair<uint32_t, double>> out;
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(path.c_str(), out));
    // 仅 base_qty 存活
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].first, static_cast<uint32_t>(HP_BASE_QTY));
}

// 非数值类型拒收 (此前字符串 getDouble 静默返回 0.0 污染参数)
TEST(HotParamFile, RejectsNonNumericTypes)
{
    std::string path = writeTmpYaml("base_spread: \"abc\"\nprotect_ticks: true\nbase_qty: 5\n");
    std::vector<std::pair<uint32_t, double>> out;
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(path.c_str(), out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].first, static_cast<uint32_t>(HP_BASE_QTY));
}

// 文件不存在 / 解析失败
TEST(HotParamFile, ParseFailureReturnsFalse)
{
    std::vector<std::pair<uint32_t, double>> out;
    EXPECT_FALSE(FutuHotParamManager::parseHotParamFile("/tmp/opencode/__no_such_hp__.yaml", out));
    EXPECT_TRUE(out.empty());
}

// syncFromFile: 值变更 -> 写共享内存 + 置 pending; 重复同步同值 -> 零写入
TEST(HotParamSync, ValueDiffSetsPendingAndDedups)
{
    TestableHotParamManager mgr;
    mgr.bind(HP_BASE_SPREAD, 2.0);
    mgr.bind(HP_BASE_QTY, 5.0);

    std::string path = writeTmpYaml("base_spread: 3.5\n");

    // 无绑定的键不写 (ptr null); 有绑定且值不同 -> 1 变更
    EXPECT_EQ(mgr.syncFromFile(path.c_str()), 1);
    EXPECT_DOUBLE_EQ(mgr.slot(HP_BASE_SPREAD), 3.5);
    EXPECT_TRUE(mgr.consumePendingApply());

    // 同值再同步: 0 变更, 不置 pending
    EXPECT_EQ(mgr.syncFromFile(path.c_str()), 0);
    EXPECT_FALSE(mgr.consumePendingApply());
    EXPECT_DOUBLE_EQ(mgr.slot(HP_BASE_SPREAD), 3.5);

    // consumePendingApply 只消费一次
    std::string path2 = writeTmpYaml("base_spread: 4.0\n");
    EXPECT_EQ(mgr.syncFromFile(path2.c_str()), 1);
    EXPECT_TRUE(mgr.consumePendingApply());
    EXPECT_FALSE(mgr.consumePendingApply());
    EXPECT_DOUBLE_EQ(mgr.slot(HP_BASE_SPREAD), 4.0);
}

// 全部键值非法 -> 0 变更, 不置 pending
TEST(HotParamSync, AllInvalidValuesNoPending)
{
    TestableHotParamManager mgr;
    mgr.bind(HP_BASE_SPREAD, 2.0);

    std::string path = writeTmpYaml("base_spread: -100\n");
    EXPECT_EQ(mgr.syncFromFile(path.c_str()), 0);
    EXPECT_FALSE(mgr.consumePendingApply());
    EXPECT_DOUBLE_EQ(mgr.slot(HP_BASE_SPREAD), 2.0); // 旧值保留
}

// 解析失败: -1, 不置 pending
TEST(HotParamSync, ParseFailureNoPending)
{
    TestableHotParamManager mgr;
    EXPECT_EQ(mgr.syncFromFile("/tmp/opencode/__no_such_hp2__.yaml"), -1);
    EXPECT_FALSE(mgr.consumePendingApply());
}
