// P1 UI bridge tests: builder field mapping, JSON null semantics, command
// parsing, and a loopback integration test of UiBridge (hello/snapshot/
// events/refresh-ack/readonly gate/auth/FIFO backpressure).
// P2 additions: CANCELO/SETPARAM command handlers and the write path
// (JSON -> CommandBus wire translation -> worker dispatch -> ack).
#include "../UiBridge.h"
#include "../UiSnapshotBuilder.h"
#include "../UiSnapshotView.h"
#include "../OptionData.h"
#include "../ExpiryData.h"
#include "../StrikeData.h"
#include "../CommandBus.h"
#include "../OptionCommandHandlers.h"
#include "../TradingState.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace wt_option;

#define ASSERT_TRUE(x) do { if (!(x)) { std::cerr << "FAIL: " #x " (line " << __LINE__ << ")" << std::endl; std::exit(1); } } while (0)
#define ASSERT_EQ(a,b) do { auto _a=(a); auto _b=(b); if (!(_a==_b)) { \
    std::cerr << "FAIL: " #a " != " #b " (line " << __LINE__ << ")" << std::endl; std::exit(1); } } while (0)

namespace
{

std::shared_ptr<OptionData> makeLeg(const std::string& code, OptionRight right,
                                    bool withBook, bool withTheo = true)
{
    OptionInfo ci{};
    ci.code = code;
    ci.product = "cu";
    ci.expiry = 202612;
    ci.strike = 100000;
    ci.right = right;
    ci.multiplier = 5;
    ci.tickSize = 2;
    auto od = std::make_shared<OptionData>(ci);
    if (withBook) {
        OptionMarket m{};
        m.bid = 1233; m.bidSize = 2; m.ask = 1237; m.askSize = 1; m.last = 1235;
        od->updateMarket(m);
    }
    od->setPosition(2);
    if (withTheo) {
        auto& vals = od->beginUpdateValues();
        vals.setTheo(1235.2).setTheoVol(0.1285).setImpliedVol(0.128);
        vals.greeks().delta() = 0.52;
        od->commitUpdateValues();
    }
    return od;
}

int connectLocal(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_TRUE(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

/// Read one '\n'-terminated line (2s timeout from SO_RCVTIMEO).
std::string recvLine(int fd)
{
    std::string line;
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return line;      // timeout or closed: return what we have
        if (c == '\n') return line;
        line.push_back(c);
        if (line.size() > 1 << 20) return line;
    }
}

bool sendAll(int fd, const std::string& s)
{
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

int main()
{
    std::cout << "WtOptionCore UI Bridge Tests" << std::endl;

    // ------------------------------------------------------------------
    // 1) buildLeg: market/theo/iv/delta/vshift/position mapping
    // ------------------------------------------------------------------
    auto edp = std::make_shared<ExpiryData>(202612, "cu_o");
    edp->setForward(101255.5);
    edp->setUnderlyingPrice(101250.0);
    auto sd = StrikeData::createWT(edp, 100000);
    auto call = makeLeg("SHFE.cu2612C100000", OR_Call, true);
    auto put = makeLeg("SHFE.cu2612P100000", OR_Put, false, /*withTheo=*/false);
    sd->setCall(call);
    sd->setPut(put);

    UiLegView leg;
    UiSnapshotBuilder::buildLeg(*call, 0.131, leg);
    ASSERT_EQ(leg.bid, 1233.0);
    ASSERT_EQ(leg.bidSz, 2.0);
    ASSERT_EQ(leg.ask, 1237.0);
    ASSERT_TRUE(leg.hasTheo);
    ASSERT_TRUE(std::fabs(leg.theo - 1235.2) < 1e-9);
    ASSERT_TRUE(leg.hasIv);
    ASSERT_TRUE(leg.hasDelta);
    ASSERT_TRUE(std::fabs(leg.delta - 0.52) < 1e-9);
    ASSERT_TRUE(leg.hasVshift);
    ASSERT_TRUE(std::fabs(leg.vshift - (0.1285 - 0.131)) < 1e-12);
    ASSERT_EQ(leg.pos, 2.0);
    ASSERT_TRUE(!leg.hasQmode);   // no trading data attached
    ASSERT_TRUE(!leg.hasPnl);

    // ------------------------------------------------------------------
    // 2) buildExpiry: booksWithBid (FITGATE metric), readiness flags, nulls
    // ------------------------------------------------------------------
    UiExpiryView ev;
    UiSnapshotBuilder::buildExpiry({sd}, *edp, 0.131, 0.02, nullptr, ev);
    ASSERT_EQ(ev.expiry, 202612u);
    ASSERT_EQ(ev.booksTotal, 2);
    ASSERT_EQ(ev.booksWithBid, 1);        // only the call has a book
    ASSERT_TRUE(!ev.hasUnd);              // no hedge UTD wired
    ASSERT_TRUE(!ev.hasGreeks);           // nullptr peek
    ASSERT_TRUE(ev.hasAtmVol);
    ASSERT_TRUE(std::fabs(ev.atmVol - 0.131) < 1e-12);
    ASSERT_EQ(ev.strikes.size(), 1u);
    ASSERT_EQ(ev.strikes[0].k, 100000.0);

    // ------------------------------------------------------------------
    // 3) parseCommand: JSON + CSV verb
    // ------------------------------------------------------------------
    UiParsedCommand pc;
    ASSERT_TRUE(UiSnapshotBuilder::parseCommand("{\"cmd\":\"refresh\",\"cmdSeq\":7}", pc));
    ASSERT_EQ(pc.cmd, std::string("refresh"));
    ASSERT_TRUE(pc.hasCmdSeq);
    ASSERT_EQ(pc.cmdSeq, 7u);
    ASSERT_TRUE(UiSnapshotBuilder::parseCommand("REFRESH", pc));
    ASSERT_EQ(pc.cmd, std::string("refresh"));
    ASSERT_TRUE(UiSnapshotBuilder::parseCommand("ORDER,B,SHFE.cu2612C100000,1233,1", pc));
    ASSERT_EQ(pc.cmd, std::string("order"));
    ASSERT_TRUE(!UiSnapshotBuilder::parseCommand("{bad json", pc));
    ASSERT_TRUE(!UiSnapshotBuilder::parseCommand("", pc));

    // ------------------------------------------------------------------
    // 4) encodeSnapshot: null semantics + field presence
    // ------------------------------------------------------------------
    UiSnapshotView v;
    v.seq = 42;
    v.tsWallMs = 1763300000123ull;
    v.stateType = "Normal";
    v.stateReason = "";
    v.channelReady = true;
    v.enabled = true;
    v.tdate = 20261116;
    v.hasPnl = true;
    v.pnlCur = 1234.5;
    v.ordersSent = 10; v.fills = 2; v.cancels = 3; v.rejects = 1;
    v.expiries.push_back(ev);
    UiOrderView o;
    o.id = 10001; o.code = "SHFE.cu2612C100000"; o.buy = true;
    o.px = 1233; o.qty = 2; o.filled = 1; o.t = 1763300000.0;
    v.orders.push_back(o);
    v.contracts.emplace_back("SHFE.cu2612C100000", std::make_pair(5.0, 2.0));

    std::string json;
    UiSnapshotBuilder::encodeSnapshot(v, "option_mm_ag", json);
    ASSERT_TRUE(json.find("\"type\":\"snapshot\"") != std::string::npos);
    ASSERT_TRUE(json.find("\"trading\":\"Normal\"") != std::string::npos);
    ASSERT_TRUE(json.find("\"booksWithBid\":1") != std::string::npos);
    ASSERT_TRUE(json.find("\"theo\":null") != std::string::npos);   // put leg has no theo
    ASSERT_TRUE(json.find("\"id\":10001") != std::string::npos);
    ASSERT_TRUE(json.find("\"priceTick\":2") != std::string::npos);

    // event + fills encoders produce terminated lines
    std::string frame;
    UiEventFrame t;
    t.type = UiEventType::Trade; t.seq = 1; t.id = 5; t.code = "X"; t.qty = 1; t.px = 2;
    UiSnapshotBuilder::encodeEvent(t, frame);
    ASSERT_TRUE(frame.back() == '\n');
    ASSERT_TRUE(frame.find("\"type\":\"trade\"") != std::string::npos);
    std::string fj;
    UiSnapshotBuilder::encodeFills({{"X", true, 2, 1, 3.5}}, "sid", 9, fj);
    ASSERT_TRUE(fj.find("\"type\":\"fills\"") != std::string::npos);

    // ------------------------------------------------------------------
    // 5) UiBridge loopback integration
    // ------------------------------------------------------------------
    UiBridge::Config cfg;
    cfg.enabled = true;
    cfg.port = 0;                        // ephemeral
    cfg.readonly = true;
    cfg.heartbeatSec = 0;                // no worker in this test
    cfg.snapshotIntervalSec = 60.0;
    cfg.sid = "option_mm_ag";

    UiBridge bridge(cfg);
    ASSERT_TRUE(bridge.start());
    ASSERT_TRUE(bridge.boundPort() != 0);

    int fd = connectLocal(bridge.boundPort());
    std::string hello = recvLine(fd);
    ASSERT_TRUE(hello.find("\"type\":\"hello\"") != std::string::npos);
    ASSERT_TRUE(hello.find("\"sid\":\"option_mm_ag\"") != std::string::npos);

    // snapshot publish -> delivered (new client gets current slot immediately)
    bridge.publishSnapshot(UiSnapshotView{});
    std::string snap = recvLine(fd);
    ASSERT_TRUE(snap.find("\"type\":\"snapshot\"") != std::string::npos);

    // event broadcast
    bridge.pushEvent(t);
    std::string evLine = recvLine(fd);
    ASSERT_TRUE(evLine.find("\"type\":\"trade\"") != std::string::npos);

    // refresh: IO queues -> (simulated worker) drain -> ack to the client
    ASSERT_TRUE(sendAll(fd, "{\"cmd\":\"refresh\",\"cmdSeq\":7}\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let IO queue it
    bool sawRefresh = false;
    bridge.drainCommands([&](const std::string& line, std::string& err) {
        UiParsedCommand p2;
        if (!UiSnapshotBuilder::parseCommand(line, p2)) { err = p2.err; return false; }
        if (p2.cmd == "refresh") { sawRefresh = true; return true; }
        err = "unsupported";
        return false;
    });
    ASSERT_TRUE(sawRefresh);
    std::string ack = recvLine(fd);
    ASSERT_TRUE(ack.find("\"type\":\"ack\"") != std::string::npos);
    ASSERT_TRUE(ack.find("\"cmdSeq\":7") != std::string::npos);
    ASSERT_TRUE(ack.find("\"ok\":true") != std::string::npos);

    // readonly gate: rejected at IO, no worker involvement
    ASSERT_TRUE(sendAll(fd, "{\"cmd\":\"stop\",\"cmdSeq\":8}\n"));
    std::string nack = recvLine(fd);
    ASSERT_TRUE(nack.find("\"ok\":false") != std::string::npos);
    ASSERT_TRUE(nack.find("readonly") != std::string::npos);
    ASSERT_EQ(bridge.cmdsRejected(), 1u);

    // snapshot cadence helpers (worker-side clock; interval is 60s here)
    ASSERT_TRUE(bridge.snapshotDue(0.0));            // first snapshot always due
    bridge.noteSnapshotSent(100.0);
    ASSERT_TRUE(!bridge.snapshotDue(100.5));
    ASSERT_TRUE(bridge.snapshotDue(161.0));

    close(fd);
    bridge.stop();

    // ------------------------------------------------------------------
    // 6) FIFO overflow with no clients -> drops counted, worker never blocks
    // ------------------------------------------------------------------
    UiBridge::Config cfg2;
    cfg2.enabled = true;
    cfg2.port = 0;
    cfg2.heartbeatSec = 0;
    UiBridge sink(cfg2);
    ASSERT_TRUE(sink.start());
    for (int i = 0; i < 300; ++i) sink.pushEvent(t);
    ASSERT_TRUE(sink.eventDrops() >= 44u);           // 300 - 256 cap
    sink.stop();

    // ------------------------------------------------------------------
    // 7) token auth: hello only after valid auth
    // ------------------------------------------------------------------
    UiBridge::Config cfg3;
    cfg3.enabled = true;
    cfg3.port = 0;
    cfg3.heartbeatSec = 0;
    cfg3.token = "s3cret";
    UiBridge gated(cfg3);
    ASSERT_TRUE(gated.start());
    int fd3 = connectLocal(gated.boundPort());
    ASSERT_TRUE(sendAll(fd3, "{\"auth\":\"s3cret\"}\n"));
    std::string hello3 = recvLine(fd3);
    ASSERT_TRUE(hello3.find("\"type\":\"hello\"") != std::string::npos);
    close(fd3);
    gated.stop();

    // non-loopback bind refused without allow_remote
    UiBridge::Config cfg4;
    cfg4.enabled = true;
    cfg4.bind = "0.0.0.0";
    cfg4.port = 0;
    UiBridge remote(cfg4);
    ASSERT_TRUE(!remote.start());

    // ------------------------------------------------------------------
    // 8) P2 command handlers: CANCELO + SETPARAM (whitelist in the strategy)
    // ------------------------------------------------------------------
    CommandBus bus;
    OptionCommandHandlers cmdHandlers;
    bool cancelOneOk = true;
    std::string lastParamKey;
    double lastParamValue = -1;
    OptionCommandHandlers::Hooks hooks;
    hooks.setState = [](TradingStateType, const std::string&) {};
    hooks.channelReady = []() { return true; };
    hooks.setQModeAll = [](const std::string&, std::string&) { return true; };
    hooks.executeOrder = [](const std::string&, bool, double, uint32_t) { return 1; };
    hooks.cancelAll = [](const std::string&) {};
    hooks.cancelOne = [&](uint32_t, std::string& err) {
        if (!cancelOneOk) { err = "stra_cancel failed"; return false; }
        return true;
    };
    hooks.setHotParam = [&](const std::string& key, double value, std::string& err) {
        // strategy-side whitelist replica (fixture): only hot-effective keys
        if (key != "wgt_vegaflow" && key != "max_tps") {
            err = "key not hot-adjustable: " + key;
            return false;
        }
        lastParamKey = key;
        lastParamValue = value;
        return true;
    };
    cmdHandlers.registerAll(bus, std::move(hooks));

    std::string be;
    ASSERT_TRUE(bus.dispatch("CANCELO,42", be));
    cancelOneOk = false;
    ASSERT_TRUE(!bus.dispatch("CANCELO,42", be));
    ASSERT_TRUE(!bus.dispatch("CANCELO,abc", be));      // malformed localid
    cancelOneOk = true;
    ASSERT_TRUE(bus.dispatch("SETPARAM,wgt_vegaflow,0.4", be));
    ASSERT_EQ(lastParamKey, std::string("wgt_vegaflow"));
    ASSERT_EQ(lastParamValue, 0.4);
    ASSERT_TRUE(bus.dispatch("SETPARAM,max_tps,60", be));
    ASSERT_EQ(lastParamValue, 60.0);
    ASSERT_TRUE(!bus.dispatch("SETPARAM,sticky_base,0.9", be));  // not hot-adjustable
    ASSERT_TRUE(!bus.dispatch("SETPARAM,wgt_vegaflow,abc", be)); // malformed value

    // ------------------------------------------------------------------
    // 9) P2 bridge write path: JSON -> wire translation -> dispatch -> ack
    // ------------------------------------------------------------------
    UiBridge::Config cfgW;
    cfgW.enabled = true;
    cfgW.port = 0;
    cfgW.readonly = false;               // write path open
    cfgW.heartbeatSec = 0;
    cfgW.sid = "opt";
    UiBridge wb(cfgW);
    ASSERT_TRUE(wb.start());
    int fdW = connectLocal(wb.boundPort());
    std::string helloW = recvLine(fdW);
    ASSERT_TRUE(helloW.find("\"readonly\":false") != std::string::npos);

    std::vector<std::string> dispatched;
    auto drainToBus = [&]() {
        wb.drainCommands([&](const std::string& line, std::string& err) -> bool {
            if (line.find("refresh") != std::string::npos ||
                line.find("REFRESH") != std::string::npos) return true;
            dispatched.push_back(line);
            return bus.dispatch(line, err);
        });
    };

    // JSON stop -> wire "STOP" -> bus ok -> ack ok:true
    ASSERT_TRUE(sendAll(fdW, "{\"cmd\":\"stop\",\"cmdSeq\":20}\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    drainToBus();
    std::string ackW = recvLine(fdW);
    ASSERT_TRUE(ackW.find("\"cmdSeq\":20") != std::string::npos);
    ASSERT_TRUE(ackW.find("\"ok\":true") != std::string::npos);
    ASSERT_EQ(dispatched.back(), std::string("STOP"));

    // JSON order -> translated wire line
    ASSERT_TRUE(sendAll(fdW, "{\"cmd\":\"order\",\"code\":\"SHFE.cu2612C100000\",\"side\":\"B\",\"price\":12.5,\"qty\":3,\"cmdSeq\":21}\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    drainToBus();
    std::string ackO = recvLine(fdW);
    ASSERT_TRUE(ackO.find("\"ok\":true") != std::string::npos);
    ASSERT_EQ(dispatched.back(),
              std::string("ORDER,B,SHFE.cu2612C100000,12.5,3"));

    // malformed order (missing qty) -> immediate ack err, nothing dispatched
    const size_t nBefore = dispatched.size();
    ASSERT_TRUE(sendAll(fdW, "{\"cmd\":\"order\",\"code\":\"X\",\"side\":\"B\",\"price\":12.5,\"cmdSeq\":22}\n"));
    std::string ackBad = recvLine(fdW);
    ASSERT_TRUE(ackBad.find("\"ok\":false") != std::string::npos);
    ASSERT_TRUE(ackBad.find("qty") != std::string::npos);
    ASSERT_EQ(dispatched.size(), nBefore);

    // set_param translation + whitelist rejection propagation
    ASSERT_TRUE(sendAll(fdW, "{\"cmd\":\"set_param\",\"key\":\"wgt_vegaflow\",\"value\":0.4,\"cmdSeq\":23}\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    drainToBus();
    std::string ackP = recvLine(fdW);
    ASSERT_TRUE(ackP.find("\"ok\":true") != std::string::npos);
    ASSERT_EQ(dispatched.back(), std::string("SETPARAM,wgt_vegaflow,0.4"));
    ASSERT_EQ(lastParamValue, 0.4);

    ASSERT_TRUE(sendAll(fdW, "{\"cmd\":\"set_param\",\"key\":\"sticky_base\",\"value\":0.9,\"cmdSeq\":24}\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    drainToBus();
    std::string ackP2 = recvLine(fdW);
    ASSERT_TRUE(ackP2.find("\"ok\":false") != std::string::npos);
    ASSERT_TRUE(ackP2.find("not hot-adjustable") != std::string::npos);

    close(fdW);
    wb.stop();

    // ------------------------------------------------------------------
    // 10) snapshot params echo (P2 SETPARAM current values)
    // ------------------------------------------------------------------
    UiSnapshotView pv;
    pv.pWgtVegaflow = 0.35;
    pv.pWgtFrontfutSkew = 0.25;
    pv.pWgtDeltaflow = 0.2;
    pv.pWgtAtmsig = 0.15;
    pv.pWgtRollema = 0.05;
    pv.pMaxTps = 30;
    std::string pj;
    UiSnapshotBuilder::encodeSnapshot(pv, "sid", pj);
    ASSERT_TRUE(pj.find("\"params\"") != std::string::npos);
    ASSERT_TRUE(pj.find("\"wgt_vegaflow\":0.35") != std::string::npos);
    ASSERT_TRUE(pj.find("\"max_tps\":30") != std::string::npos);

    std::cout << "ALL UI BRIDGE TESTS PASSED" << std::endl;
    return 0;
}
