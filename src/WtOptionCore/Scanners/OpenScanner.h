#pragma once
/*!
 * \file OpenScanner.h
 * \brief Thin alias over EdgeScannerBase (convergence 2.1) — identical edge
 * logic, only the default ident and hit-reason differ.
 */
#include "EdgeScannerBase.h"

namespace wt_option {

class OpenScanner : public EdgeScannerBase {
public:
    explicit OpenScanner(const ScannerConfig& sc)
        : EdgeScannerBase(sc, "OpenScanner", "open_edge") {}
};

} // namespace wt_option
