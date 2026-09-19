/*!
 * \file FutuConfigAudit.cpp
 * \brief 配置读取审计实现 (F-core P1-4 阶段 1)
 */
#include "FutuConfigAudit.h"

#include "../WTSTools/WTSLogger.h"

#include <algorithm>

namespace futu
{

namespace
{
/// 递归深度上限: 防止异常配置 (自引用/超深嵌套) 造成栈膨胀
constexpr int kMaxDepth = 8;
/// 单个源 WARN 输出的 unknown 上限 (避免报告本身刷屏)
constexpr size_t kMaxUnknownWarn = 40;
} // namespace

ConfigAudit& ConfigAudit::instance()
{
    static ConfigAudit inst;
    return inst;
}

void ConfigAudit::reset()
{
    std::lock_guard<std::mutex> _g(_mtx);
    _sources.clear();
    _node_path.clear();
    _node_source.clear();
    _whitelist.clear();
    _closed.store(false, std::memory_order_relaxed);
}

void ConfigAudit::beginSource(const std::string& source, wtp::WTSVariant* root)
{
    if (source.empty() || root == nullptr)
        return;

    std::lock_guard<std::mutex> _g(_mtx);
    Source& src = _sources[source];
    src.name = source;

    // 根节点自身登记为空路径 (noteRead 时回退为裸键名)
    _node_path[static_cast<const void*>(root)] = std::string();
    _node_source[static_cast<const void*>(root)] = source;

    walkSource(src, root, std::string(), 0);
}

void ConfigAudit::walkSource(Source& src, wtp::WTSVariant* node, const std::string& prefix, int depth)
{
    if (node == nullptr || depth > kMaxDepth)
        return;
    if (!node->isObject() && !node->isArray())
        return;

    // 容器节点本身也登记为 present (其子键被读时按前缀规则判定为已读)
    if (!prefix.empty())
        src.present.insert(prefix);

    if (node->isObject())
    {
        const auto names = node->memberNames();
        for (const auto& key : names)
        {
            wtp::WTSVariant* child = node->get(key.c_str());
            if (child == nullptr)
                continue;
            std::string path = prefix.empty() ? key : (prefix + "." + key);
            src.present.insert(path);
            _node_path[static_cast<const void*>(child)] = path;
            _node_source[static_cast<const void*>(child)] = src.name;
            if (child->isObject() || child->isArray())
                walkSource(src, child, path, depth + 1);
        }
    }
    else // array: 只登记数组本身 (元素由 get(idx) 读, 归属该数组路径)
    {
        const uint32_t n = node->size();
        for (uint32_t i = 0; i < n && i < 32; ++i)
        {
            wtp::WTSVariant* child = node->get(i);
            if (child == nullptr)
                continue;
            // 元素节点指向数组路径: 元素内键的读取按 "数组路径.键" 记账
            _node_path[static_cast<const void*>(child)] = prefix;
            _node_source[static_cast<const void*>(child)] = src.name;
            // 元素为对象时继续展开, 键路径与数组路径同前缀 (多元素取并集),
            //   使 "contracts.code" 这类键能被正确识别为 present/read
            if (child->isObject() || child->isArray())
                walkSource(src, child, prefix, depth + 1);
        }
    }
}

const char* ConfigAudit::pathOf(const void* node)
{
    auto& self = instance();
    auto it = self._node_path.find(node);
    return it == self._node_path.end() ? nullptr : it->second.c_str();
}

void ConfigAudit::noteRead(wtp::WTSVariant* node, const char* key)
{
    if (_closed.load(std::memory_order_relaxed) || key == nullptr)
        return;

    std::lock_guard<std::mutex> _g(_mtx);
    const void* np = static_cast<const void*>(node);

    std::string source;
    std::string prefix;
    auto sit = _node_source.find(np);
    if (sit != _node_source.end())
        source = sit->second;
    auto pit = _node_path.find(np);
    if (pit != _node_path.end())
        prefix = pit->second;

    if (source.empty())
        return; // 非登记源 (如单测临时构造的 variant) 不计入审计

    Source& src = _sources[source];
    if (src.name.empty())
        src.name = source;
    std::string path = prefix.empty() ? std::string(key) : (prefix + "." + key);
    src.read.insert(path);
}

void ConfigAudit::noteReadPath(const std::string& source, const std::string& path)
{
    if (_closed.load(std::memory_order_relaxed) || source.empty() || path.empty())
        return;

    std::lock_guard<std::mutex> _g(_mtx);
    Source& src = _sources[source];
    if (src.name.empty())
        src.name = source;
    src.read.insert(path);
    src.explicit_reads++;
}

void ConfigAudit::addWhitelistPrefix(const std::string& prefix)
{
    if (prefix.empty())
        return;
    std::lock_guard<std::mutex> _g(_mtx);
    _whitelist.push_back(prefix);
}

bool ConfigAudit::isWhitelisted(const std::string& path) const
{
    for (const auto& p : _whitelist)
    {
        if (path == p)
            return true;
        if (path.size() > p.size() && path.compare(0, p.size(), p) == 0 && path[p.size()] == '.')
            return true;
    }
    return false;
}

bool ConfigAudit::isRead(const Source& src, const std::string& path) const
{
    if (src.read.find(path) != src.read.end())
        return true;
    // 容器规则: 任一子路径被读 ⇒ 容器视为已读 (避免为节点级 get() 逐个插桩)
    const std::string dot = path + ".";
    auto it = src.read.lower_bound(dot);
    if (it != src.read.end() && it->compare(0, dot.size(), dot) == 0)
        return true;
    return false;
}

std::vector<std::string> ConfigAudit::unknownList(const std::string& source) const
{
    std::lock_guard<std::mutex> _g(_mtx);
    std::vector<std::string> out;
    for (const auto& kv : _sources)
    {
        if (!source.empty() && kv.first != source)
            continue;
        for (const auto& path : kv.second.present)
        {
            if (isWhitelisted(path) || isRead(kv.second, path))
                continue;
            out.push_back(kv.first + ":" + path);
        }
    }
    return out;
}

size_t ConfigAudit::unknownCount() const
{
    return unknownList().size();
}

size_t ConfigAudit::presentCount(const std::string& source) const
{
    std::lock_guard<std::mutex> _g(_mtx);
    size_t n = 0;
    for (const auto& kv : _sources)
    {
        if (!source.empty() && kv.first != source)
            continue;
        n += kv.second.present.size();
    }
    return n;
}

size_t ConfigAudit::readCount(const std::string& source) const
{
    std::lock_guard<std::mutex> _g(_mtx);
    size_t n = 0;
    for (const auto& kv : _sources)
    {
        if (!source.empty() && kv.first != source)
            continue;
        n += kv.second.read.size();
    }
    return n;
}

size_t ConfigAudit::report(const char* tag)
{
    size_t total_unknown = 0;
    {
        std::lock_guard<std::mutex> _g(_mtx);

        for (const auto& kv : _sources)
        {
            const Source& src = kv.second;
            std::vector<std::string> unknown;
            for (const auto& path : src.present)
            {
                if (isWhitelisted(path) || isRead(src, path))
                    continue;
                unknown.push_back(path);
            }

            // 已读取但文件里不存在的键 (拼错/层级错 → 静默回退默认值) 同样值得告警
            std::vector<std::string> missing;
            for (const auto& path : src.read)
            {
                if (src.present.find(path) != src.present.end())
                    continue;
                const std::string dot = path + ".";
                bool container_present = false;
                auto it = src.present.lower_bound(path);
                if (it != src.present.end())
                {
                    if (*it == path || it->compare(0, dot.size(), dot) == 0)
                        container_present = true;
                }
                if (!container_present)
                {
                    // 白名单键 (文档化可选/归档) 不计入 actionable 计数与告警
                    if (!isWhitelisted(path))
                        missing.push_back(path);
                }
            }

            WTSLogger::info("[CONFIG-AUDIT] tag={} source={} present={} read={} unknown={} read_missing={}",
                            tag ? tag : "",
                            src.name,
                            src.present.size(),
                            src.read.size(),
                            unknown.size(),
                            missing.size());

            size_t shown = 0;
            for (const auto& path : unknown)
            {
                if (shown++ >= kMaxUnknownWarn)
                    break;
                WTSLogger::warn("[CONFIG-AUDIT] source={} UNKNOWN key '{}' -- 配了但无读取点 (拼错? 死键? 需加白名单或接线)",
                                src.name,
                                path);
            }
            if (unknown.size() > shown)
                WTSLogger::warn("[CONFIG-AUDIT] source={} ...另有 {} 个 unknown 未逐条打印",
                                src.name,
                                unknown.size() - shown);

            for (const auto& path : missing)
            {
                WTSLogger::warn("[CONFIG-AUDIT] source={} READ-MISSING key '{}' -- 代码读了但文件里没有 (配置缺失 → 静默用默认值)",
                                src.name,
                                path);
            }

            total_unknown += unknown.size();
        }

        _closed.store(true, std::memory_order_relaxed); // 启动报告后关闭, 后续 note* 近零开销
    }

    return total_unknown;
}

} // namespace futu
