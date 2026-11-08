#pragma once
/*!
 * \file SimplexScanner.h
 * \brief Thin alias over EdgeScannerBase (convergence 2.1) — identical edge
 * logic, only the default ident and hit-reason differ.
 */
#include "EdgeScannerBase.h"

namespace wt_option {

class SimplexScanner : public EdgeScannerBase {
public:
    explicit SimplexScanner(const ScannerConfig& sc)
        : EdgeScannerBase(sc, "SimplexScanner", "simplex_edge") {}
};

} // namespace wt_option
