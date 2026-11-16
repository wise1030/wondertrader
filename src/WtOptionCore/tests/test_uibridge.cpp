// P1 UI bridge tests: builder field mapping, JSON null semantics, command
// parsing, and a loopback integration test of UiBridge (hello/snapshot/
// events/refresh-ack/readonly gate/auth/FIFO backpressure).
#include "../UiBridge.h"
#include "../UiSnapshotBuilder.h"
#include "../UiSnapshotView.h"
#include "../OptionData.h"
#include "../ExpiryData.h"
#include "../StrikeData.h"

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

    std::cout << "ALL UI BRIDGE TESTS PASSED" << std::endl;
    return 0;
}
