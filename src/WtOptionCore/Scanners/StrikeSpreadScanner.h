#pragma once
/*!
 * \file StrikeSpreadScanner.h
 * \brief Thin alias over EdgeScannerBase (convergence 2.1) — identical edge
 * logic, only the default ident and hit-reason differ.
 */
#include "EdgeScannerBase.h"

namespace wt_option {

class StrikeSpreadScanner : public EdgeScannerBase {
public:
    explicit StrikeSpreadScanner(const ScannerConfig& sc)
        : EdgeScannerBase(sc, "StrikeSpreadScanner", "strike_spread_edge") {}
};

} // namespace wt_option
