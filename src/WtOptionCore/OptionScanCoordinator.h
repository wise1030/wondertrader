/*!
 * \file OptionScanCoordinator.h
 * \brief Phase 2 seam: owns scanner lifecycle and dispatch.
 *
 * HftOptionStrategy used to hold the scanner vector and repeat lifecycle /
 * dispatch loops in four places. The coordinator keeps one ownership point and
 * makes scanner dispatch unit-testable without constructing the whole strategy.
 */
#pragma once

#include "Scanners/IScanModule.h"

#include <memory>
#include <vector>

namespace wt_option {

class OptionGrid;

class OptionScanCoordinator {
public:
    void add(const IScanModulePtr& scanner);
    void clear();
    bool empty() const { return m_scanners.empty(); }
    size_t size() const { return m_scanners.size(); }
    const std::vector<IScanModulePtr>& scanners() const { return m_scanners; }

    void startAll();
    void stopAll();
    void panicAll();

    void dispatchOption(OptionData* option);
    void dispatchUnderlying(double price);
    void dispatchTick(const OptionGrid* grid);

private:
    std::vector<IScanModulePtr> m_scanners;
};

} // namespace wt_option
