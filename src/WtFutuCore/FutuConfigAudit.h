/*!
 * \file FutuConfigAudit.h
 * \brief 配置读取审计 (F-core P1-4 阶段 1, 2026-09-19)
 *
 * 目标: 消除"配置写了不生效 / 键名拼错静默回退默认值"这一整类隐性缺陷。
 *   历史实证: `alertThrottleMs` 需 5 处编辑; V8-R3 perf 阈值"注入后无逻辑读取";
 *   `setMinProfitThreshold` 曾无调用者(A10); 本周 `arbWaitMode` 无 yaml 入口;
 *   hotparams snake_case/camelCase 整套静默回退。
 *
 * 口径（三档, 对应 §19.5 执行约束 2）:
 *   - 生产运行: 每个源输出 `present/read/unknown` 统计, unknown 逐条 WARN（白名单除外）;
 *   - fixture 单测: `unknownCount() == 0` 硬断言;
 *   - BT 配置: 与生产同口径, 归档死键由白名单前缀兜住。
 *
 * 覆盖范围: config.yaml（策略节点）、coordinator.yaml、hotparams.yaml,
 *   以及两个路径键（coordinatorConfig/spreadArbitrageConfig 本身）。
 *
 * 线程与性能: 读取发生在启动期与热参重载（低频）; 启动报告后自动 close()，
 *   之后所有 note* 调用退化为一次原子读返回（热路径近零开销）。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../Includes/WTSVariant.hpp"

namespace futu
{

class ConfigAudit
{
public:
    static ConfigAudit& instance();

    /// 登记一个 yaml 源: 递归展开键路径 (含容器路径) 与"节点指针 → 路径"映射,
    /// 用于后续 noteRead(node, key) 反查完整路径。
    void beginSource(const std::string& source, wtp::WTSVariant* root);

    /// 记录一次读取。node 为读取所在容器节点 (可为 nullptr → 回退裸键名)。
    void noteRead(wtp::WTSVariant* node, const char* key);

    /// 直接记录某源下的完整键路径 (注册表驱动的键, 如热参名)。
    void noteReadPath(const std::string& source, const std::string& path);

    /// 白名单前缀: 命中的"存在但未读取"路径不计入 unknown。
    /// 用途: CONFIG.md §7 死键、框架/外部拥有键、运行态差异键。
    void addWhitelistPrefix(const std::string& prefix);

    /// 输出报告 (tag 仅用于日志标识); 返回 unknown 总数。
    /// 首个 report() 之后自动 close()（后续 note* 近零开销）。
    size_t report(const char* tag);

    // ---- 查询/测试接口 ----
    size_t unknownCount() const;
    size_t presentCount(const std::string& source = "") const;
    size_t readCount(const std::string& source = "") const;
    std::vector<std::string> unknownList(const std::string& source = "") const;
    void reset();
    void close() { _closed.store(true, std::memory_order_relaxed); }
    bool closed() const { return _closed.load(std::memory_order_relaxed); }

private:
    ConfigAudit() = default;

    struct Source
    {
        std::string name;
        std::set<std::string> present; ///< 文件里存在的键路径 (含容器)
        std::set<std::string> read;    ///< 被读取的键路径
        size_t explicit_reads = 0;     ///< noteReadPath 计数 (注册表驱动)
    };

    void walkSource(Source& src, wtp::WTSVariant* node, const std::string& prefix, int depth);
    bool isWhitelisted(const std::string& path) const;
    bool isRead(const Source& src, const std::string& path) const;
    static const char* pathOf(const void* node);

    mutable std::mutex _mtx;
    std::map<std::string, Source> _sources;
    std::map<const void*, std::string> _node_path;   ///< node ptr → 源内路径
    std::map<const void*, std::string> _node_source; ///< node ptr → 源名
    std::vector<std::string> _whitelist;
    std::atomic<bool> _closed{false};
};

} // namespace futu
