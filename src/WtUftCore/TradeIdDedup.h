/*!
 * \file TradeIdDedup.h
 * \brief 实时成交去重（按 交易日|交易所|TradeID）。
 *
 * CTP 的 TradeID 是交易所成交编号，单交易所内唯一；跨 SHFE/INE 不保证
 * 全局唯一，因此 key 必须带 exchange；跨交易日必须清空。
 * 空 TradeID 不做去重，交给调用方跳过。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace wtp {

class TradeIdDedup {
public:
    /// Returns true when this key was already seen (duplicate).
    bool seen(const std::string& key) {
        if (key.empty()) return false;
        return !m_keys.insert(key).second;
    }

    void clear() { m_keys.clear(); }
    size_t size() const { return m_keys.size(); }

    static std::string makeKey(uint32_t tradeDate, const char* exchange, const char* tradeId) {
        if (tradeId == nullptr || tradeId[0] == '\0') return {};
        std::string key;
        key.reserve(32);
        key.append(std::to_string(tradeDate));
        key.push_back('|');
        key.append(exchange ? exchange : "");
        key.push_back('|');
        key.append(tradeId);
        return key;
    }

private:
    std::unordered_set<std::string> m_keys;
};

} // namespace wtp
