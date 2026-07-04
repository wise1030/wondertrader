/*!
 * \file ScannerInfo.h
 * \brief Simplified scanner info struct (migrated from quantbox ScannerInfo.h)
 *
 * Original: longbeach::trading::ScannerInfo : public OrderInfoSlot
 *   — carried LONGBEACH_DECLARE_ORDER_INFO, bson logFill, etc.
 *
 * Migration: stripped to a plain POD struct. Only the fields used downstream
 * are retained: scannerName, legId, timedOut.
 */
#pragma once

#include <cstdint>
#include <string>

namespace wt_option {

struct ScannerInfo
{
    std::string scannerName;
    int32_t     legId     = 0;
    bool        timedOut  = false;

    ScannerInfo() = default;
    explicit ScannerInfo(const std::string& name)
        : scannerName(name), legId(0), timedOut(false) {}
    ScannerInfo(const std::string& name, int32_t leg_id)
        : scannerName(name), legId(leg_id), timedOut(false) {}

    const std::string& getScannerName() const { return scannerName; }
    int32_t getLegId() const { return legId; }
    bool isTimedOut() const { return timedOut; }
    void setTimedOut(bool b) { timedOut = b; }
};

} // namespace wt_option
