#include "OptionScanCoordinator.h"
#include "OptionData.h"
#include "OptionGrid.h"

namespace wt_option {

void OptionScanCoordinator::add(const IScanModulePtr& scanner)
{
    if (scanner)
        m_scanners.push_back(scanner);
}

void OptionScanCoordinator::clear()
{
    m_scanners.clear();
}

void OptionScanCoordinator::startAll()
{
    for (auto& s : m_scanners)
        if (s) s->onStart();
}

void OptionScanCoordinator::stopAll()
{
    for (auto& s : m_scanners)
        if (s) s->onStop();
}

void OptionScanCoordinator::panicAll()
{
    for (auto& s : m_scanners)
        if (s) s->onPanic();
}

void OptionScanCoordinator::dispatchOption(OptionData* option)
{
    if (!option) return;
    for (auto& s : m_scanners) {
        if (s && s->isEnabled() && s->isExpiryEnabled(option->getExpiry()))
            s->onOptionUpdate(option);
    }
}

void OptionScanCoordinator::dispatchUnderlying(double price)
{
    if (!(price > 0)) return;
    for (auto& s : m_scanners) {
        if (s && s->isEnabled())
            s->onUnderlyingUpdate(price);
    }
}

void OptionScanCoordinator::dispatchTick(const OptionGrid* grid)
{
    if (!grid) return;
    for (auto& s : m_scanners) {
        if (s && s->isEnabled())
            s->onTick(grid);
    }
}

} // namespace wt_option
