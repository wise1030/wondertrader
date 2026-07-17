#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace wt_option {

enum class FilterResult { APPROVED, REJECTED, MODIFIED };

struct FilterContext {
    std::string code;
    bool isBuy = false;
    double price = 0;
    uint32_t qty = 0;
    uint32_t modifiedQty = 0;

    int32_t currentPosition = 0;
    int32_t potentialPosition = 0;
    int32_t numCancels = 0;
    int32_t numNewOrders = 0;
    int32_t numFills = 0;

    std::string rejectReason;
};

class IRiskFilter {
public:
    virtual ~IRiskFilter() = default;
    virtual FilterResult process(FilterContext& ctx) = 0;
    virtual const char* name() const = 0;
};

class RiskFilterChain {
public:
    void add(std::unique_ptr<IRiskFilter> f);
    FilterResult execute(FilterContext& ctx);
    bool empty() const { return m_filters.empty(); }
    size_t size() const { return m_filters.size(); }

private:
    std::vector<std::unique_ptr<IRiskFilter>> m_filters;
};

class MaxOrderSizeFilter : public IRiskFilter {
public:
    MaxOrderSizeFilter(uint32_t maxSize, bool reject = false)
        : m_maxSize(maxSize), m_bReject(reject) {}
    FilterResult process(FilterContext& ctx) override;
    const char* name() const override { return "MaxOrderSize"; }
private:
    uint32_t m_maxSize;
    bool m_bReject;
};

class MinSellPriceFilter : public IRiskFilter {
public:
    explicit MinSellPriceFilter(double minPrice) : m_minPrice(minPrice) {}
    FilterResult process(FilterContext& ctx) override;
    const char* name() const override { return "MinSellPrice"; }
private:
    double m_minPrice;
};

class MaxPositionFilter : public IRiskFilter {
public:
    enum Mode { REJECT_ON_OVERFLOW = 0, ALLOW_OVERFLOW = 1, MODIFY_TO_MAX = 2 };

    MaxPositionFilter(uint32_t maxPos, Mode mode = REJECT_ON_OVERFLOW)
        : m_maxPos(maxPos), m_mode(mode) {}
    FilterResult process(FilterContext& ctx) override;
    const char* name() const override { return "MaxPosition"; }
private:
    uint32_t m_maxPos;
    Mode m_mode;
};

class MaxCancelFilter : public IRiskFilter {
public:
    MaxCancelFilter(int32_t softLimit, int32_t hardLimit)
        : m_softLimit(softLimit), m_hardLimit(hardLimit) {}
    FilterResult process(FilterContext& ctx) override;
    const char* name() const override { return "MaxCancel"; }
private:
    int32_t m_softLimit;
    int32_t m_hardLimit;
};

class MaxNewOrdersFilter : public IRiskFilter {
public:
    MaxNewOrdersFilter(int32_t hardFlatLimit, int32_t rejectLimit)
        : m_hardFlatLimit(hardFlatLimit), m_rejectLimit(rejectLimit) {}
    FilterResult process(FilterContext& ctx) override;
    const char* name() const override { return "MaxNewOrders"; }
private:
    int32_t m_hardFlatLimit;
    int32_t m_rejectLimit;
};

using RiskFilterChainPtr = std::shared_ptr<RiskFilterChain>;

} // namespace wt_option
