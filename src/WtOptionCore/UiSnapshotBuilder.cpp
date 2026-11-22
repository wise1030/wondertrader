/*!
 * \file UiSnapshotBuilder.cpp
 * \brief UiSnapshotBuilder implementation (see header for the contract).
 */
#include "UiSnapshotBuilder.h"

#include "OptionData.h"
#include "ExpiryData.h"
#include "StrikeData.h"
#include "OptionExpiryGreeks.h"
#include "OptionTradingData.h"
#include "UnderlyingTradingData.h"
#include "PnlTracker.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#ifndef _WIN32
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/document.h>
#endif

namespace wt_option {

void UiSnapshotBuilder::buildLeg(const OptionData& od, double atmVol, UiLegView& out)
{
    out.code = od.getCode();

    const OptionMarket& mkt = od.getMarket();
    out.bid = mkt.bid;    out.bidSz = mkt.bidSize;
    out.ask = mkt.ask;    out.askSz = mkt.askSize;
    out.last = mkt.last;

    // P1 follow-up: five-level book (cap at the view's DEPTH)
    {
        const int32_t nb = std::min(mkt.numBidLevels, UiLegView::DEPTH);
        const int32_t na = std::min(mkt.numAskLevels, UiLegView::DEPTH);
        out.numBid = nb; out.numAsk = na;
        for (int32_t i = 0; i < nb; ++i) {
            out.bidPx[i] = mkt.bidPrices[i];
            out.bidSz5[i] = mkt.bidQty[i];
        }
        for (int32_t i = 0; i < na; ++i) {
            out.askPx[i] = mkt.askPrices[i];
            out.askSz5[i] = mkt.askQty[i];
        }
    }

    // 0 is the engine's "not computed" sentinel -> JSON null downstream.
    const double theo = od.getTheoPrice();
    if (theo != 0.0) { out.hasTheo = true; out.theo = theo; }

    const double iv = od.getImpliedVol();
    if (iv != 0.0) { out.hasIv = true; out.iv = iv; }

    const double delta = od.greeks().delta();
    if (delta != 0.0) { out.hasDelta = true; out.delta = delta; }

    out.pos = od.getPosition();

    const double theoVol = od.activeValues().theoVol();
    if (theoVol > 0.0 && atmVol > 0.0) {
        out.hasVshift = true;
        out.vshift = theoVol - atmVol;
    }

    out.pos = od.getPosition();

    auto otd = od.getTradingData();
    if (otd) {
        out.hasQmode = true;
        out.qmode = static_cast<int32_t>(otd->getQuoteMode());
        auto pt = otd->getPnlTracker();
        if (pt) { out.hasPnl = true; out.pnl = pt->getCurPnl(); }

        // P1 batch-2: our resting quote + queue position (quantbox bqpos/aqpos
        // semantics, price-gap approximation — no book depth needed).
        const MultiMarket& our = otd->multiMarket();
        const double ourBid = our.getBestBid().px();
        const double ourAsk = our.getBestAsk().px();
        double tick = od.getTickSize();
        if (tick <= 0) tick = 1.0;
        if (ourBid > 0) {
            out.hasOb = true; out.obid = ourBid;
            if (mkt.ask > 0 && ourBid >= mkt.ask - tick * 0.5) out.bqp = -1;   // crossing
            else if (mkt.bid > 0 && ourBid >= mkt.bid - tick * 0.5) out.bqp = 0; // at best
            else if (mkt.bid > 0) out.bqp = (int32_t)std::ceil((mkt.bid - ourBid) / tick);
            else out.bqp = 0;   // no market bid — our quote IS the best
        }
        if (ourAsk > 0) {
            out.hasOb = true; out.oask = ourAsk;
            if (mkt.bid > 0 && ourAsk <= mkt.bid + tick * 0.5) out.aqp = -1;   // crossing
            else if (mkt.ask > 0 && ourAsk <= mkt.ask + tick * 0.5) out.aqp = 0; // at best
            else if (mkt.ask > 0) out.aqp = (int32_t)std::ceil((ourAsk - mkt.ask) / tick);
            else out.aqp = 0;   // no market ask — our quote IS the best
        }
    }
}

void UiSnapshotBuilder::buildExpiry(const std::vector<StrikeDataPtr>& strikes,
                                    const ExpiryData& ed,
                                    double atmVol, double atmVolSprd,
                                    const OptionExpiryGreeks* expGreeks,
                                    UiExpiryView& out)
{
    out.expiry = ed.getExpiry();
    out.fwd = ed.getForward();
    out.fwdReady = ed.isForwardReady();
    out.fitReady = ed.isFitReady();
    out.underlyingValid = ed.isUnderlyingPriceValid();
    out.daysToExpiry = ed.daysToExpiry();

    if (atmVol != 0.0)     { out.hasAtmVol = true; out.atmVol = atmVol; }
    if (atmVolSprd != 0.0) { out.hasAtmVolSprd = true; out.atmVolSprd = atmVolSprd; }

    const UnderlyingTradingData* utd = ed.getHedgeUTD();
    if (utd) {
        out.hasUnd = true;
        out.undCode = ed.getHedgeCode();
        out.undBid = utd->getBid();
        out.undAsk = utd->getAsk();
        out.undBidSz = utd->getBidSize();
        out.undAskSz = utd->getAskSize();
        out.undPos = utd->getPosition();
        out.undDelta = utd->getPosition() * utd->getContractSize();
        // P1 UI: our futures-leg quotes (symmetric to the option legs'
        // ordBid/ordAsk)
        const auto& our = utd->ourMarket();
        const auto& ob = our.getBestBid();
        const auto& oa = our.getBestAsk();
        if (!ob.empty()) { out.hasUndOur = true; out.undOurBid = ob.px(); out.undOurBidSz = ob.sz(); }
        if (!oa.empty()) { out.hasUndOur = true; out.undOurAsk = oa.px(); out.undOurAskSz = oa.sz(); }
    }

    if (expGreeks) {
        out.hasGreeks = true;
        out.greeksDelta = expGreeks->delta();
        out.greeksVega = expGreeks->vega();
        out.greeksGamma = expGreeks->gamma();   // P1 batch-5.2
        out.greeksTheta = expGreeks->theta();
    }

    out.strikes.reserve(strikes.size());
    for (const auto& sd : strikes) {
        if (!sd) continue;
        UiStrikeView sv;
        sv.k = static_cast<double>(sd->getStrikePrice());
        if (sd->call()) buildLeg(*sd->call(), atmVol, sv.C);
        if (sd->put())  buildLeg(*sd->put(),  atmVol, sv.P);
        // FITGATE metric (HftOptionStrategy.cpp): leg counted when getBid() > 0
        for (const OptionData* leg : {sd->call().get(), sd->put().get()}) {
            if (!leg) continue;
            ++out.booksTotal;
            if (leg->getBid() > 0) ++out.booksWithBid;
        }
        out.strikes.push_back(std::move(sv));
    }
}

namespace {

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

bool UiSnapshotBuilder::parseCommand(const std::string& line, UiParsedCommand& out)
{
    out = UiParsedCommand{};
    const std::string t = trim(line);
    if (t.empty()) { out.err = "empty line"; return false; }

    if (t[0] == '{') {
#ifndef _WIN32
        rapidjson::Document doc;
        if (doc.Parse(t.c_str()).HasParseError() || !doc.IsObject()) {
            out.err = "invalid JSON";
            return false;
        }
        auto it = doc.FindMember("cmd");
        if (it == doc.MemberEnd() || !it->value.IsString()) {
            out.err = "missing cmd";
            return false;
        }
        out.cmd = toLower(it->value.GetString());
        auto is = doc.FindMember("cmdSeq");
        if (is != doc.MemberEnd() && is->value.IsUint64()) {
            out.cmdSeq = is->value.GetUint64();
            out.hasCmdSeq = true;
        }
        if (out.cmd.empty()) { out.err = "empty cmd"; return false; }
        out.valid = true;
        return true;
#else
        out.err = "JSON commands unsupported on this platform";
        return false;
#endif
    }

    // Bare CSV verb (CommandBus wire format): verb up to first ',' or space.
    size_t e = t.find_first_of(", \t");
    std::string verb = (e == std::string::npos) ? t : t.substr(0, e);
    out.cmd = toLower(verb);
    if (out.cmd.empty()) { out.err = "empty cmd"; return false; }
    out.valid = true;
    return true;
}

#ifndef _WIN32
namespace {

struct JsonWriter : rapidjson::Writer<rapidjson::StringBuffer>
{
    using Base = rapidjson::Writer<rapidjson::StringBuffer>;
    using Base::Base;

    void key(const char* k) { Key(k); }

    /// Number when finite, null otherwise — JSON has no NaN/Inf.
    void numOrNull(const char* k, double v) {
        key(k);
        if (std::isfinite(v)) Double(v); else Null();
    }
    void numOrNull(double v) {
        if (std::isfinite(v)) Double(v); else Null();
    }
    void nullable(const char* k, bool has, double v) {
        key(k);
        if (has && std::isfinite(v)) Double(v); else Null();
    }
};

void writeLeg(JsonWriter& w, const UiLegView& leg)
{
    w.StartObject();
    w.key("code"); w.String(leg.code.c_str());
    if (leg.bidSz > 0) { w.key("bid"); w.numOrNull(leg.bid); w.key("bidSz"); w.numOrNull(leg.bidSz); }
    else { w.key("bid"); w.Null(); w.key("bidSz"); w.Null(); }
    if (leg.askSz > 0) { w.key("ask"); w.numOrNull(leg.ask); w.key("askSz"); w.numOrNull(leg.askSz); }
    else { w.key("ask"); w.Null(); w.key("askSz"); w.Null(); }
    w.key("last"); if (leg.last != 0) w.numOrNull(leg.last); else w.Null();
    w.nullable("theo", leg.hasTheo, leg.theo);
    w.nullable("iv", leg.hasIv, leg.iv);
    w.nullable("delta", leg.hasDelta, leg.delta);
    w.nullable("vshift", leg.hasVshift, leg.vshift);
    w.key("pos"); w.numOrNull(leg.pos);
    w.key("qmode"); if (leg.hasQmode) w.Int(leg.qmode); else w.Null();
    if (leg.hasOb) { w.key("obid"); w.numOrNull(leg.obid); w.key("oask"); w.numOrNull(leg.oask); }
    else { w.key("obid"); w.Null(); w.key("oask"); w.Null(); }
    w.key("bqp"); w.Int(leg.bqp);
    w.key("aqp"); w.Int(leg.aqp);
    if (leg.numBid > 1 || leg.numAsk > 1) {   // P1 follow-up: depth beyond best
        // Audit 11-22 (round 2): emit levels BEYOND the best only (i>=1) —
        // level 0 is already rendered as bidSz/askSz, so including it made
        // the frontend "+" aggregate double-count the best level.
        w.key("bd"); w.StartArray();
        for (int32_t i = 1; i < leg.numBid; ++i)
            { w.StartArray(); w.Double(leg.bidPx[i]); w.Double(leg.bidSz5[i]); w.EndArray(); }
        w.EndArray();
        w.key("ad"); w.StartArray();
        for (int32_t i = 1; i < leg.numAsk; ++i)
            { w.StartArray(); w.Double(leg.askPx[i]); w.Double(leg.askSz5[i]); w.EndArray(); }
        w.EndArray();
    }
    w.nullable("pnl", leg.hasPnl, leg.pnl);
    w.EndObject();
}

} // namespace

void UiSnapshotBuilder::encodeHello(const std::string& sid, bool readonly, std::string& out)
{
    rapidjson::StringBuffer sb;
    JsonWriter w(sb);
    w.StartObject();
    w.key("type"); w.String("hello");
    w.key("proto"); w.Uint(1);
    w.key("sid"); w.String(sid.c_str());
    w.key("readonly"); w.Bool(readonly);
    w.EndObject();
    out.assign(sb.GetString(), sb.GetSize());
    out.push_back('\n');
}

void UiSnapshotBuilder::encodeSnapshot(const UiSnapshotView& v, const std::string& sid, std::string& out)
{
    rapidjson::StringBuffer sb;
    JsonWriter w(sb);
    w.StartObject();
    w.key("type"); w.String("snapshot");
    w.key("proto"); w.Uint(1);
    w.key("seq"); w.Uint64(v.seq);
    w.key("ts"); w.Uint64(v.tsWallMs);
    w.key("sid"); w.String(sid.c_str());

    w.key("state"); w.StartObject();
    w.key("trading"); w.String(v.stateType.c_str());
    w.key("reason"); w.String(v.stateReason.c_str());
    w.key("channelReady"); w.Bool(v.channelReady);
    w.key("underlyingReady"); w.Bool(v.underlyingReady);
    w.key("enabled"); w.Bool(v.enabled);
    w.key("panicked"); w.Bool(v.panicked);
    w.key("tdate"); w.Uint(v.tdate);
    w.EndObject();

    w.key("greeks"); w.StartObject();
    w.nullable("delta", v.hasGreeks, v.gDelta);
    w.nullable("gamma", v.hasGreeks, v.gGamma);
    w.nullable("vega", v.hasGreeks, v.gVega);
    w.nullable("vegaTW", v.hasGreeks, v.gVegaTW);
    w.nullable("theta", v.hasGreeks, v.gTheta);
    // option delta + futures-hedge delta — the hedger's actual trigger
    // quantity (see UiSnapshotView::pfDelta).
    w.nullable("pfDelta", v.hasGreeks, v.pfDelta);
    w.EndObject();

    w.key("pnl"); w.StartObject();
    w.nullable("cur", v.hasPnl, v.pnlCur);
    w.nullable("realized", v.hasPnl, v.pnlRealized);
    w.nullable("turnover", v.hasPnl, v.pnlTurnover);
    w.EndObject();

    w.key("risk"); w.StartObject();
    w.key("limitsBreached"); w.Bool(v.limitsBreached);
    w.key("tps"); w.Int(v.tps);
    w.key("tpsMax"); w.Int(v.tpsMax);
    w.key("ordersSent"); w.Uint(v.ordersSent);
    {   // P1 batch-4.3 per-source buckets
        w.key("liveByTag"); w.StartArray();
        for (int i = 0; i < 4; ++i) w.Uint(v.liveByTag[i]);
        w.EndArray();
        w.key("fillsByTag"); w.StartArray();
        for (int i = 0; i < 4; ++i) w.Uint(v.fillsByTag[i]);
        w.EndArray();
    }
    w.key("fills"); w.Uint(v.fills);
    w.key("cancels"); w.Uint(v.cancels);
    w.key("rejects"); w.Uint(v.rejects);
    w.key("cancelRatio"); w.numOrNull(v.cancelRatio);
    w.key("fillRatio"); w.numOrNull(v.fillRatio);
    w.key("sessionStopInSec"); if (v.hasSessionStop) w.Int(v.sessionStopInSec); else w.Null();
    w.EndObject();

    // P1 batch-5.1: delta-hedge fire threshold for the hedge panel (TOP
    // LEVEL — the frontend reads snapshot.hedgeDeltaThreshold; an earlier
    // revision encoded it inside "risk" where nothing could see it).
    w.numOrNull("hedgeDeltaThreshold", v.hedgeDeltaThreshold);
    w.key("pred"); w.StartObject();
    w.key("resolved"); w.Uint(v.predResolved);
    w.numOrNull("hitRate", v.predHitRate);
    w.numOrNull("mae", v.predMae);
    w.key("scanResolved"); w.Uint(v.scanResolved);
    w.numOrNull("scanHitRate", v.scanHitRate);
    w.numOrNull("scanIc", v.scanIc);
    w.EndObject();
    w.key("perf"); w.StartObject();
    w.key("batchP50"); w.numOrNull(v.batchP50);
    w.key("batchP95"); w.numOrNull(v.batchP95);
    w.key("batchP99"); w.numOrNull(v.batchP99);
    w.key("aepEvents"); w.Uint64(v.aepEvents);
    w.key("queueDrops"); w.Uint64(v.queueDrops);
    w.key("mdOverflow"); w.Uint64(v.mdOverflow);
    w.key("traderDepth"); w.Uint64(v.traderDepth);
    w.key("eventDrops"); w.Uint64(v.eventDrops);
    w.key("snapshotCoalesced"); w.Uint64(v.snapshotCoalesced);
    w.EndObject();

    w.key("expiries"); w.StartArray();
    for (const auto& e : v.expiries) {
        w.StartObject();
        w.key("expiry"); w.Uint(e.expiry);
        w.key("quoting"); w.Bool(e.quoting);
        w.numOrNull("fwd", e.fwd);
        w.key("fwdReady"); w.Bool(e.fwdReady);
        w.key("fitReady"); w.Bool(e.fitReady);
        w.key("underlyingValid"); w.Bool(e.underlyingValid);
        w.nullable("atmVol", e.hasAtmVol, e.atmVol);
        w.nullable("atmVolSprd", e.hasAtmVolSprd, e.atmVolSprd);
        w.key("daysToExpiry"); w.Int(e.daysToExpiry);
        w.key("booksWithBid"); w.Int(e.booksWithBid);
        w.key("booksTotal"); w.Int(e.booksTotal);
        w.key("greeks"); w.StartObject();
        w.nullable("delta", e.hasGreeks, e.greeksDelta);
        w.nullable("vega", e.hasGreeks, e.greeksVega);
        w.nullable("gamma", e.hasGreeks, e.greeksGamma);   // P1 batch-5.2
        w.nullable("theta", e.hasGreeks, e.greeksTheta);
        w.EndObject();
        w.nullable("targDelta", e.hasTargDelta, e.targDelta);
        w.nullable("targVega", e.hasTargVega, e.targVega);
        w.key("und"); if (e.hasUnd) {
            w.StartObject();
            w.key("code"); w.String(e.undCode.c_str());
            w.numOrNull("bid", e.undBid);
            w.numOrNull("ask", e.undAsk);
            w.numOrNull("bidSz", e.undBidSz);
            w.numOrNull("askSz", e.undAskSz);
            w.numOrNull("pos", e.undPos);
            // hedge-leg position delta (underlying units) — see UiSnapshotView
            w.numOrNull("undDelta", e.undDelta);
            if (e.hasUndOur) {   // P1 UI: our futures-leg quotes
                w.numOrNull("ourBid", e.undOurBid);
                w.numOrNull("ourAsk", e.undOurAsk);
                w.numOrNull("ourBidSz", e.undOurBidSz);
                w.numOrNull("ourAskSz", e.undOurAskSz);
            }
            w.EndObject();
        } else w.Null();
        w.key("strikes"); w.StartArray();
        for (const auto& s : e.strikes) {
            w.StartObject();
            w.numOrNull("k", s.k);
            w.key("C"); writeLeg(w, s.C);
            w.key("P"); writeLeg(w, s.P);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();

    w.key("orders"); w.StartArray();
    for (const auto& o : v.orders) {
        w.StartObject();
        w.key("id"); w.Uint(o.id);
        w.key("code"); w.String(o.code.c_str());
        w.key("buy"); w.Bool(o.buy);
        w.numOrNull("px", o.px);
        w.key("qty"); w.Uint(o.qty);
        w.key("filled"); w.Uint(o.filled);
        w.numOrNull("t", o.t);
        if (o.tag) { w.key("tag"); w.Uint(o.tag); }
        w.EndObject();
    }
    w.EndArray();

    w.key("meta"); w.StartObject();
    w.key("contracts"); w.StartObject();
    for (const auto& c : v.contracts) {
        w.key(c.first.c_str());
        w.StartObject();
        w.numOrNull("mult", c.second.first);
        w.numOrNull("priceTick", c.second.second);
        w.EndObject();
    }
    w.EndObject();
    w.EndObject();

    // P2 hot-param echo (SETPARAM whitelist — sticky_base / improve_retreat
    // are init-time only and deliberately absent)
    w.key("params"); w.StartObject();
    w.numOrNull("wgt_vegaflow", v.pWgtVegaflow);
    w.numOrNull("wgt_frontfut_skew", v.pWgtFrontfutSkew);
    w.numOrNull("wgt_deltaflow", v.pWgtDeltaflow);
    w.numOrNull("wgt_atmsig", v.pWgtAtmsig);
    w.numOrNull("wgt_rollema", v.pWgtRollema);
    w.key("max_tps"); w.Int(v.pMaxTps);
    w.EndObject();

    w.EndObject();
    out.assign(sb.GetString(), sb.GetSize());
    out.push_back('\n');
}

void UiSnapshotBuilder::encodeFills(const std::vector<UiFillView>& fills,
                                    const std::string& sid, uint64_t seq, std::string& out)
{
    rapidjson::StringBuffer sb;
    JsonWriter w(sb);
    w.StartObject();
    w.key("type"); w.String("fills");
    w.key("sid"); w.String(sid.c_str());
    w.key("seq"); w.Uint64(seq);
    w.key("items"); w.StartArray();
    for (const auto& f : fills) {
        w.StartObject();
        w.key("code"); w.String(f.code.c_str());
        w.key("buy"); w.Bool(f.buy);
        w.numOrNull("px", f.px);
        w.key("qty"); w.Uint(f.qty);
        w.numOrNull("t", f.t);
        if (f.tag) { w.key("tag"); w.Uint(f.tag); }
        if (f.hasCtx) {   // P1 batch-6.3
            w.key("ctx"); w.StartObject();
            w.numOrNull("theo", f.theo);
            w.numOrNull("mid", f.midAtIssue);
            w.numOrNull("iv", f.iv);
            w.numOrNull("delta", f.delta);
            w.numOrNull("gamma", f.gamma);
            w.numOrNull("vega", f.vega);
            w.numOrNull("theta", f.theta);
            w.EndObject();
        }
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    out.assign(sb.GetString(), sb.GetSize());
    out.push_back('\n');
}

void UiSnapshotBuilder::encodeEvent(const UiEventFrame& ev, std::string& out)
{
    rapidjson::StringBuffer sb;
    JsonWriter w(sb);
    w.StartObject();
    switch (ev.type) {
    case UiEventType::Trade:
        w.key("type"); w.String("trade");
        w.key("seq"); w.Uint64(ev.seq);
        w.key("ts"); w.Uint64(ev.tsWallMs);
        w.key("id"); w.Uint(ev.id);
        w.key("code"); w.String(ev.code.c_str());
        w.key("buy"); w.Bool(ev.buy);
        w.numOrNull("qty", ev.qty);
        w.numOrNull("px", ev.px);
        if (ev.tag) { w.key("tag"); w.Uint(ev.tag); }
        break;
    case UiEventType::Order:
        w.key("type"); w.String("order");
        w.key("seq"); w.Uint64(ev.seq);
        w.key("ts"); w.Uint64(ev.tsWallMs);
        w.key("id"); w.Uint(ev.id);
        w.key("code"); w.String(ev.code.c_str());
        w.key("buy"); w.Bool(ev.buy);
        w.numOrNull("qty", ev.qty);
        w.numOrNull("left", ev.left);
        w.numOrNull("px", ev.px);
        w.key("canceled"); w.Bool(ev.canceled);
        if (ev.tag) { w.key("tag"); w.Uint(ev.tag); }
        break;
    case UiEventType::Alert:
        w.key("type"); w.String("alert");
        w.key("seq"); w.Uint64(ev.seq);
        w.key("ts"); w.Uint64(ev.tsWallMs);
        w.key("level"); w.Uint(ev.level);
        w.key("msg"); w.String(ev.msg.c_str());
        break;
    case UiEventType::Reject:
        w.key("type"); w.String("reject");
        w.key("seq"); w.Uint64(ev.seq);
        w.key("ts"); w.Uint64(ev.tsWallMs);
        w.key("id"); w.Uint(ev.id);
        w.key("ok"); w.Bool(false);
        break;
    case UiEventType::Ack:
        w.key("type"); w.String("ack");
        w.key("cmdSeq"); w.Uint64(ev.cmdSeq);
        w.key("ok"); w.Bool(ev.ok);
        w.key("err"); w.String(ev.err.c_str());
        break;
    }
    w.EndObject();
    out.assign(sb.GetString(), sb.GetSize());
    out.push_back('\n');
}

#else // _WIN32 — stub encoders (no rapidjson in MyDeps141; Windows UI is
      // explicitly out of scope for P1, see design doc 10.4/6)

void UiSnapshotBuilder::encodeHello(const std::string& sid, bool readonly, std::string& out)
{
    out = "{\"type\":\"hello\",\"proto\":1,\"stub\":true}\n";
    (void)sid; (void)readonly;
}

void UiSnapshotBuilder::encodeSnapshot(const UiSnapshotView& v, const std::string& sid, std::string& out)
{
    out = "{\"type\":\"snapshot\",\"stub\":true}\n";
    (void)v; (void)sid;
}

void UiSnapshotBuilder::encodeFills(const std::vector<UiFillView>& fills,
                                    const std::string& sid, uint64_t seq, std::string& out)
{
    out = "{\"type\":\"fills\",\"stub\":true}\n";
    (void)fills; (void)sid; (void)seq;
}

void UiSnapshotBuilder::encodeEvent(const UiEventFrame& ev, std::string& out)
{
    out = "{\"type\":\"event\",\"stub\":true}\n";
    (void)ev;
}

#endif // _WIN32

} // namespace wt_option
