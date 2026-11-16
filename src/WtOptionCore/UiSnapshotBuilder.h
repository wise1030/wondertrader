/*!
 * \file UiSnapshotBuilder.h
 * \brief Pure snapshot/event -> POD view mapping + JSON Lines encoding.
 *
 * Build functions run on the AEP worker thread (read-only on engine state);
 * encode functions run on the UiBridge IO thread. No IO, no locking here.
 * Encoding uses rapidjson on Linux; other platforms emit a stub frame so the
 * library still compiles where rapidjson is unavailable (Windows UI is out of
 * scope for P1 by design).
 */
#pragma once

#include "UiSnapshotView.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wt_option {

class OptionData;
class ExpiryData;
class OptionExpiryGreeks;
struct StrikeData;
using StrikeDataPtr = std::shared_ptr<StrikeData>;

/// Parsed inbound command (JSON preferred, CSV CommandBus lines accepted).
struct UiParsedCommand
{
    bool valid = false;
    std::string cmd;            // lowercased verb, e.g. "refresh"
    uint64_t cmdSeq = 0;
    bool hasCmdSeq = false;
    std::string err;            // parse error detail
};

namespace UiSnapshotBuilder
{
    /// One call/put leg. atmVol in decimal (e.g. 0.131); vshift = theoVol-atmVol.
    void buildLeg(const OptionData& od, double atmVol, UiLegView& out);

    /// One expiry: strikes + readiness flags + per-expiry greeks (nullable).
    void buildExpiry(const std::vector<StrikeDataPtr>& strikes,
                     const ExpiryData& ed,
                     double atmVol, double atmVolSprd,
                     const OptionExpiryGreeks* expGreeks,
                     UiExpiryView& out);

    /// Inbound line -> command. Accepts {"cmd":"refresh","cmdSeq":n} JSON or
    /// bare CSV verbs ("REFRESH", "ORDER,B,..."). Only the verb is parsed here;
    /// argument validation stays in the worker-side dispatcher.
    bool parseCommand(const std::string& line, UiParsedCommand& out);

    void encodeHello(const std::string& sid, bool readonly, std::string& out);
    void encodeSnapshot(const UiSnapshotView& v, const std::string& sid, std::string& out);
    void encodeFills(const std::vector<UiFillView>& fills, const std::string& sid, uint64_t seq, std::string& out);
    void encodeEvent(const UiEventFrame& ev, std::string& out);
}

} // namespace wt_option
