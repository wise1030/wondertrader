/*!
 * \file UiBridge.h
 * \brief P1 UI bridge: JSON Lines over loopback TCP between the strategy
 *        worker thread and browser-facing middleware.
 *
 * Design: analysis/ui-migration-design-2026-11-16.md (v2.1). The IO thread is
 * the only thread touching sockets; the worker thread only fills POD views /
 * event frames and drains queued commands at on_batch_complete top. All queues
 * are bounded and latest-wins where semantics allow — a slow or absent client
 * never back-pressures the trading worker.
 *
 * Threading contract (callers must respect):
 *   - start()/stop():        strategy init/shutdown thread only
 *   - publishSnapshot()/pushEvent()/drainCommands(): AEP worker thread only
 *   - everything else:       internal to the IO thread
 */
#pragma once

#include "UiSnapshotView.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wt_option {

class UiBridge
{
public:
    struct Config
    {
        bool enabled = false;                 // default off — backtest loads the same .so untouched
        std::string bind = "127.0.0.1";       // loopback only unless allowRemote
        uint16_t port = 8710;                 // 0 = ephemeral (unit tests)
        double snapshotIntervalSec = 1.0;     // full snapshot cadence (worker clock)
        double heartbeatSec = 1.0;            // enqueue_control() cadence while clients attached; 0 = off
        bool readonly = true;                 // P1 hard gate: everything but refresh rejected
        std::string token;                    // empty = no auth
        bool allowRemote = false;             // explicitly permit non-loopback binds
        std::string sid;                      // strategy id echoed in every frame
    };

    explicit UiBridge(Config cfg);
    ~UiBridge();

    UiBridge(const UiBridge&) = delete;
    UiBridge& operator=(const UiBridge&) = delete;

    /// Binds/listens and spawns the IO thread. false = bind failure (port in
    /// use, non-loopback bind without allow_remote, ...); the caller logs and
    /// keeps trading without the UI.
    bool start();
    void stop();                            // idempotent; joins the IO thread

    uint16_t boundPort() const;             // valid after successful start

    // ---- worker-thread API -------------------------------------------------
    /// on_batch_complete top (BEFORE the !grid/!pricer early return): pops
    /// queued commands, invokes handler(line, err) for each, and queues an
    /// ack back to the originating client. Never blocks on IO.
    void drainCommands(const std::function<bool(const std::string& line, std::string& err)>& handler);

    /// True when a snapshot is due (interval elapsed or a refresh forced it).
    bool snapshotDue(double nowSec) const;
    /// Worker calls this right after publishing a snapshot (records cadence).
    void noteSnapshotSent(double nowSec);
    void clearForceSnapshot();

    /// Latest-wins handoff to the IO thread (~40KB vector move under a short mutex).
    void publishSnapshot(UiSnapshotView view);

    /// Bounded FIFO (default 256); overflow drops oldest and counts.
    void pushEvent(const UiEventFrame& ev);

    /// One-shot message (fills history on refresh/reconnect), sent to all
    /// current clients right after the next snapshot.
    void publishAuxJson(std::string json);

    // ---- observability ------------------------------------------------------
    uint64_t eventDrops() const { return _eventDrops.load(std::memory_order_relaxed); }
    uint64_t snapshotCoalesced() const { return _snapshotCoalesced.load(std::memory_order_relaxed); }
    uint64_t clientDrops() const { return _clientDrops.load(std::memory_order_relaxed); }
    uint64_t cmdsAccepted() const { return _cmdsAccepted.load(std::memory_order_relaxed); }
    uint64_t cmdsRejected() const { return _cmdsRejected.load(std::memory_order_relaxed); }
    int clientCount() const { return _clientCount.load(std::memory_order_relaxed); }

    const Config& config() const { return _cfg; }

    static uint64_t wallMsNow();

private:
    struct UiCommand
    {
        int clientId = -1;
        uint64_t cmdSeq = 0;
        std::string line;
    };

    void ioThreadMain();
    void handleLine(int clientId, const std::string& line);
    bool sendTo(int clientId, const std::string& frame);
    void closeClient(int clientId);
    void queueAck(int clientId, uint64_t cmdSeq, bool ok, const std::string& err);

    Config _cfg;
    std::function<void()> _wakeWorker;      // -> AEP enqueue_control()

    // lifecycle
    std::atomic<bool> _stop{false};
    std::atomic<bool> _started{false};
    std::thread _io;
    int _listenFd = -1;
    int _pipeR = -1, _pipeW = -1;           // stop wake
    uint16_t _boundPort = 0;

    // worker -> IO: latest-wins snapshot slot
    mutable std::mutex _slotMtx;
    UiSnapshotView _slot;
    uint64_t _slotGen = 0;
    bool _slotPending = false;              // unconsumed -> next publish coalesces

    // worker -> IO: bounded event FIFO
    std::mutex _evtMtx;
    std::deque<UiEventFrame> _events;
    static constexpr size_t EVENT_CAP = 256;

    // IO -> worker: command queue (worker drains at batch top)
    std::mutex _cmdMtx;
    std::deque<UiCommand> _commands;
    static constexpr size_t COMMAND_CAP = 1024;

    // worker -> IO: acks (result of command handling)
    std::mutex _ackMtx;
    std::deque<std::pair<int, UiEventFrame>> _acks;
    static constexpr size_t ACK_CAP = 256;

    // worker -> IO: one-shot side message (fills history)
    std::mutex _auxMtx;
    std::string _auxJson;
    bool _auxPending = false;

    // worker-side snapshot timing
    double _lastSnapshotSec = -1e9;
    uint64_t _lastSnapshotWallMs = 0;
    bool _forceSnapshot = false;            // worker thread only

    // IO-thread state
    struct Client
    {
        int fd = -1;
        int id = -1;
        bool authed = false;
        uint64_t lastSentGen = 0;
        std::string inbuf;
        int consecutiveDrops = 0;
    };
    std::vector<Client> _clients;           // IO thread only
    int _nextClientId = 1;                  // IO thread only
    std::vector<int> _auxDue;               // client ids owed the aux message

    // stats
    std::atomic<uint64_t> _eventDrops{0};
    std::atomic<uint64_t> _snapshotCoalesced{0};
    std::atomic<uint64_t> _clientDrops{0};
    std::atomic<uint64_t> _cmdsAccepted{0};
    std::atomic<uint64_t> _cmdsRejected{0};
    std::atomic<int> _clientCount{0};

public:
    /// Strategy registers the worker wakeup (AEP enqueue_control).
    void setWakeCallback(std::function<void()> cb) { _wakeWorker = std::move(cb); }
};

} // namespace wt_option
