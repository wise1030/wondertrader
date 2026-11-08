#pragma once
/*!
 * \file LowBidsScanner.h
 * \brief Thin alias over EdgeScannerBase (convergence 2.1) — identical edge
 * logic, only the default ident and hit-reason differ.
 */
#include "EdgeScannerBase.h"

namespace wt_option {

class LowBidsScanner : public EdgeScannerBase {
public:
    explicit LowBidsScanner(const ScannerConfig& sc)
        : EdgeScannerBase(sc, "LowBidsScanner", "low_bid_edge") {}
};

} // namespace wt_option
