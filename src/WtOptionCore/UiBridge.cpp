/*!
 * \file UiBridge.cpp
 * \brief UiBridge implementation (see header for the threading contract).
 */
#include "UiBridge.h"

#include "UiSnapshotBuilder.h"
#include "WTSLogger.h"
#include "fmtlib.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <utility>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <rapidjson/document.h>
#else
#include <winsock2.h>
#endif

namespace wt_option
{

namespace
{

std::string trimLine(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

#ifndef _WIN32
/// Extract {"auth":"..."} token; empty string when absent/unparseable.
std::string extractAuthToken(const std::string& line)
{
    if (line.empty() || line[0] != '{') return "";
    rapidjson::Document doc;
    if (doc.Parse(line.c_str()).HasParseError() || !doc.IsObject()) return "";
    auto it = doc.FindMember("auth");
    if (it == doc.MemberEnd() || !it->value.IsString()) return "";
    return it->value.GetString();
}

/// P2 write path: translate a UI JSON command into the CommandBus wire
/// format (CSV). Bare CSV lines pass through untouched (nc-friendly). The
/// worker only ever sees wire-format lines for writes.
bool translateCommand(const std::string& line, const std::string& verb,
                      std::string& wire, std::string& err)
{
    if (line.empty() || line[0] != '{')
    {
        wire = line;
        return true;
    }

    rapidjson::Document doc;
    if (doc.Parse(line.c_str()).HasParseError() || !doc.IsObject())
    {
        err = "invalid JSON";
        return false;
    }
    auto strField = [&doc](const char* name, std::string& out) -> bool {
        auto it = doc.FindMember(name);
        if (it == doc.MemberEnd() || !it->value.IsString()) return false;
        out = it->value.GetString();
        return !out.empty();
    };
    auto numField = [&doc](const char* name, double& out) -> bool {
        auto it = doc.FindMember(name);
        if (it == doc.MemberEnd() || !it->value.IsNumber()) return false;
        out = it->value.GetDouble();
        return std::isfinite(out);
    };

    auto numStr = [](double v) { return fmt::format("{}", v); };   // shortest round-trip

    if (verb == "stop" || verb == "resume" || verb == "panic")
    {
        std::string v = verb;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        wire = v;
        return true;
    }
    if (verb == "qmode")
    {
        std::string mode;
        if (!strField("mode", mode) ||
            (mode != "on" && mode != "off" && mode != "close"))
        {
            err = "qmode needs mode=on|off|close";
            return false;
        }
        wire = "QMODE," + mode;
        return true;
    }
    if (verb == "qmode_code")
    {
        std::string code, mode;
        if (!strField("code", code)) { err = "qmode_code needs code"; return false; }
        if (!strField("mode", mode) ||
            (mode != "on" && mode != "off" && mode != "close"))
        {
            err = "qmode_code needs mode=on|off|close";
            return false;
        }
        wire = "QMODES," + code + "," + mode;
        return true;
    }
    if (verb == "cancel_all")
    {
        wire = "CANCELALL";
        return true;
    }
    if (verb == "order")
    {
        std::string code, side;
        double px = 0, qtyD = 0;
        if (!strField("code", code)) { err = "order needs code"; return false; }
        if (!strField("side", side) || (side != "B" && side != "S" && side != "b" && side != "s"))
        {
            err = "order needs side=B|S";
            return false;
        }
        if (!numField("price", px) || px <= 0) { err = "order needs price>0"; return false; }
        if (!numField("qty", qtyD) || qtyD < 1) { err = "order needs qty>=1"; return false; }
        std::ostringstream os2;
        os2 << "ORDER," << static_cast<char>(std::toupper(static_cast<unsigned char>(side[0])))
            << "," << code << "," << numStr(px) << "," << static_cast<uint32_t>(qtyD);
        wire = os2.str();
        return true;
    }
    if (verb == "cancel")
    {
        std::string code;
        if (!strField("code", code)) { err = "cancel needs code"; return false; }
        wire = "CANCEL," + code;
        return true;
    }
    if (verb == "cancel_order")
    {
        auto it = doc.FindMember("id");
        if (it == doc.MemberEnd() || !it->value.IsUint()) { err = "cancel_order needs id"; return false; }
        wire = fmt::format("CANCELO,{}", it->value.GetUint());
        return true;
    }
    if (verb == "set_param")
    {
        std::string key;
        double value = 0;
        if (!strField("key", key)) { err = "set_param needs key"; return false; }
        if (!numField("value", value)) { err = "set_param needs numeric value"; return false; }
        wire = fmt::format("SETPARAM,{},{}", key, numStr(value));
        return true;
    }
    err = "unsupported command: " + verb;
    return false;
}
#endif

} // namespace

UiBridge::UiBridge(Config cfg)
    : _cfg(std::move(cfg))
{
}

UiBridge::~UiBridge()
{
    stop();
}

uint64_t UiBridge::wallMsNow()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

#ifndef _WIN32

uint16_t UiBridge::boundPort() const
{
    return _boundPort;
}

bool UiBridge::start()
{
    if (_started.load(std::memory_order_acquire)) return true;

    // Hard safety gate: non-loopback binds need explicit opt-in.
    if (!_cfg.allowRemote && _cfg.bind != "127.0.0.1" && _cfg.bind != "localhost"
        && _cfg.bind != "::1")
    {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "UiBridge: refusing to bind non-loopback address '{}' without allow_remote — UI disabled",
            _cfg.bind.c_str());
        return false;
    }

    if (pipe(&_pipeR) != 0)
    {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "UiBridge: pipe() failed: {}", strerror(errno));
        return false;
    }

    _listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (_listenFd < 0)
    {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "UiBridge: socket() failed: {}", strerror(errno));
        close(_pipeR); close(_pipeW); _pipeR = _pipeW = -1;
        return false;
    }

    int one = 1;
    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // Non-blocking: the accept-drain loop below must hit EAGAIN instead of
    // parking the IO thread forever when no further connection is pending.
    fcntl(_listenFd, F_SETFL, fcntl(_listenFd, F_GETFL, 0) | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(_cfg.port);
    const char* host = _cfg.bind == "localhost" ? "127.0.0.1" : _cfg.bind.c_str();
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "UiBridge: bad bind address '{}' (IPv4 only)", _cfg.bind.c_str());
        close(_listenFd); close(_pipeR); close(_pipeW);
        _listenFd = _pipeR = _pipeW = -1;
        return false;
    }

    if (bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "UiBridge: bind {}:{} failed: {} — trading continues without UI",
            _cfg.bind.c_str(), (int)_cfg.port, strerror(errno));
        close(_listenFd); close(_pipeR); close(_pipeW);
        _listenFd = _pipeR = _pipeW = -1;
        return false;
    }

    if (listen(_listenFd, 8) != 0)
    {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "UiBridge: listen failed: {}", strerror(errno));
        close(_listenFd); close(_pipeR); close(_pipeW);
        _listenFd = _pipeR = _pipeW = -1;
        return false;
    }

    if (_cfg.port == 0)
    {
        sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        if (getsockname(_listenFd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
            _boundPort = ntohs(bound.sin_port);
    }
    else
    {
        _boundPort = _cfg.port;
    }

    _stop.store(false, std::memory_order_release);
    _io = std::thread([this]() { ioThreadMain(); });
    _started.store(true, std::memory_order_release);

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "UiBridge: listening on {}:{} (readonly={}, heartbeat={}s, interval={}s)",
        _cfg.bind.c_str(), (int)_boundPort, _cfg.readonly ? 1 : 0,
        _cfg.heartbeatSec, _cfg.snapshotIntervalSec);
    return true;
}

void UiBridge::stop()
{
    if (!_started.load(std::memory_order_acquire)) return;
    _stop.store(true, std::memory_order_release);
    if (_pipeW >= 0)
    {
        const char b = 1;
        ssize_t r = write(_pipeW, &b, 1);
        (void)r;
    }
    if (_io.joinable()) _io.join();
    _started.store(false, std::memory_order_release);

    for (auto& c : _clients) if (c.fd >= 0) close(c.fd);
    _clients.clear();
    if (_listenFd >= 0) { close(_listenFd); _listenFd = -1; }
    if (_pipeR >= 0) { close(_pipeR); _pipeR = -1; }
    if (_pipeW >= 0) { close(_pipeW); _pipeW = -1; }
    _clientCount.store(0, std::memory_order_relaxed);
}

bool UiBridge::snapshotDue(double nowSec) const
{
    if (_forceSnapshot) return true;
    // Sim-time interval (normal path: ticks advance the strategy clock)...
    if ((nowSec - _lastSnapshotSec) >= _cfg.snapshotIntervalSec) return true;
    // ...plus a wall-clock fallback: on a live tick-less market the strategy
    // clock freezes (heartbeats carry no time), which would otherwise stall
    // snapshots forever. In replay the sim clock runs ahead of wall time, so
    // this fallback stays silent there.
    return (wallMsNow() - _lastSnapshotWallMs) >=
           static_cast<uint64_t>(_cfg.snapshotIntervalSec * 1000.0);
}

void UiBridge::noteSnapshotSent(double nowSec)
{
    _lastSnapshotSec = nowSec;
    _lastSnapshotWallMs = wallMsNow();
    _forceSnapshot = false;
}

void UiBridge::clearForceSnapshot()
{
    _forceSnapshot = false;
}

void UiBridge::publishSnapshot(UiSnapshotView view)
{
    std::lock_guard<std::mutex> lk(_slotMtx);
    if (_slotPending)
        _snapshotCoalesced.fetch_add(1, std::memory_order_relaxed);
    _slot = std::move(view);
    ++_slotGen;
    _slotPending = true;
}

void UiBridge::pushEvent(const UiEventFrame& ev)
{
    std::lock_guard<std::mutex> lk(_evtMtx);
    if (_events.size() >= EVENT_CAP)
    {
        _events.pop_front();
        _eventDrops.fetch_add(1, std::memory_order_relaxed);
    }
    _events.push_back(ev);
}

void UiBridge::publishAuxJson(std::string json)
{
    std::lock_guard<std::mutex> lk(_auxMtx);
    _auxJson = std::move(json);
    _auxPending = true;
}

void UiBridge::drainCommands(const std::function<bool(const std::string&, std::string&)>& handler)
{
    std::deque<UiCommand> batch;
    {
        std::lock_guard<std::mutex> lk(_cmdMtx);
        batch.swap(_commands);
    }
    for (const auto& cmd : batch)
    {
        std::string err;
        bool ok = false;
        try
        {
            ok = handler ? handler(cmd.line, err) : false;
            if (!ok && err.empty()) err = "rejected";
        }
        catch (const std::exception& e)
        {
            ok = false;
            err = std::string("exception: ") + e.what();
        }
        catch (...)
        {
            ok = false;
            err = "unknown exception";
        }
        std::lock_guard<std::mutex> lk(_ackMtx);
        if (_acks.size() >= ACK_CAP) continue;   // ack storm: drop oldest-pressure valve
        UiEventFrame ack;
        ack.type = UiEventType::Ack;
        ack.cmdSeq = cmd.cmdSeq;
        ack.ok = ok;
        ack.err = err;
        _acks.emplace_back(cmd.clientId, std::move(ack));
    }
}

void UiBridge::queueAck(int clientId, uint64_t cmdSeq, bool ok, const std::string& err)
{
    std::lock_guard<std::mutex> lk(_ackMtx);
    if (_acks.size() >= ACK_CAP) return;
    UiEventFrame ack;
    ack.type = UiEventType::Ack;
    ack.cmdSeq = cmdSeq;
    ack.ok = ok;
    ack.err = err;
    _acks.emplace_back(clientId, std::move(ack));
}

bool UiBridge::sendTo(int clientId, const std::string& frame)
{
    auto it = std::find_if(_clients.begin(), _clients.end(),
                           [clientId](const Client& c) { return c.id == clientId; });
    if (it == _clients.end()) return false;
    Client& c = *it;

    size_t off = 0;
    while (off < frame.size())
    {
        ssize_t n = send(c.fd, frame.data() + off, frame.size() - off, MSG_NOSIGNAL);
        if (n <= 0)
        {
            if (errno == EINTR) continue;
            c.consecutiveDrops++;
            _clientDrops.fetch_add(1, std::memory_order_relaxed);
            if (c.consecutiveDrops >= 25) closeClient(clientId);
            return false;
        }
        off += static_cast<size_t>(n);
    }
    c.consecutiveDrops = 0;
    return true;
}

void UiBridge::closeClient(int clientId)
{
    auto it = std::find_if(_clients.begin(), _clients.end(),
                           [clientId](const Client& c) { return c.id == clientId; });
    if (it == _clients.end()) return;
    WTSLogger::log_by_cat("strategy", LL_DEBUG, "UiBridge: client {} disconnected", clientId);
    if (it->fd >= 0) close(it->fd);
    _clients.erase(it);
    _clientCount.store(static_cast<int>(_clients.size()), std::memory_order_relaxed);
}

void UiBridge::handleLine(int clientId, const std::string& raw)
{
    auto it = std::find_if(_clients.begin(), _clients.end(),
                           [clientId](const Client& c) { return c.id == clientId; });
    if (it == _clients.end()) return;
    Client& c = *it;

    const std::string line = trimLine(raw);
    if (line.empty()) return;

    // Auth phase: only {"auth":"..."} is accepted until authed.
    if (!_cfg.token.empty() && !c.authed)
    {
        if (extractAuthToken(line) == _cfg.token)
        {
            c.authed = true;
            std::string hello;
            UiSnapshotBuilder::encodeHello(_cfg.sid, _cfg.readonly, hello);
            sendTo(clientId, hello);
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "UiBridge: client {} authenticated", clientId);
        }
        else
        {
            WTSLogger::log_by_cat("strategy", LL_WARN,
                "UiBridge: client {} auth failed — closing", clientId);
            closeClient(clientId);
        }
        return;
    }

    UiParsedCommand pc;
    if (!UiSnapshotBuilder::parseCommand(line, pc))
    {
        queueAck(clientId, 0, false, pc.err.empty() ? "bad command" : pc.err);
        _cmdsRejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (pc.cmd == "refresh")
    {
        {
            std::lock_guard<std::mutex> lk(_cmdMtx);
            if (_commands.size() >= COMMAND_CAP)
            {
                queueAck(clientId, pc.cmdSeq, false, "busy");
                _cmdsRejected.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            _commands.push_back(UiCommand{clientId, pc.cmdSeq, line});
        }
        _cmdsAccepted.fetch_add(1, std::memory_order_relaxed);
        if (_wakeWorker) _wakeWorker();
    }
    else if (_cfg.readonly)
    {
        queueAck(clientId, pc.cmdSeq, false, "readonly");
        _cmdsRejected.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        // P2 write path: translate JSON to the CommandBus wire format here
        // (IO thread); malformed commands are rejected without waking the
        // worker. The worker drains the wire line at the next batch top and
        // acks the execution result.
        std::string wire, terr;
        if (!translateCommand(line, pc.cmd, wire, terr))
        {
            queueAck(clientId, pc.cmdSeq, false, terr);
            _cmdsRejected.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(_cmdMtx);
            if (_commands.size() >= COMMAND_CAP)
            {
                queueAck(clientId, pc.cmdSeq, false, "busy");
                _cmdsRejected.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            _commands.push_back(UiCommand{clientId, pc.cmdSeq, std::move(wire)});
        }
        _cmdsAccepted.fetch_add(1, std::memory_order_relaxed);
        if (_wakeWorker) _wakeWorker();
    }
}

void UiBridge::ioThreadMain()
{
    WTSLogger::log_by_cat("strategy", LL_DEBUG, "UiBridge: IO thread started");

    auto lastHeartbeat = std::chrono::steady_clock::now();

    while (!_stop.load(std::memory_order_acquire))
    {
        // ---- build poll set ------------------------------------------------
        std::vector<pollfd> fds;
        std::vector<int> fdOwners;   // client id, or 0 = pipe, -1 = listen
        fds.push_back({_pipeR, POLLIN, 0});
        fdOwners.push_back(0);
        fds.push_back({_listenFd, POLLIN, 0});
        fdOwners.push_back(-1);
        for (const auto& c : _clients)
        {
            fds.push_back({c.fd, POLLIN, 0});
            fdOwners.push_back(c.id);
        }

        const int pr = poll(fds.data(), fds.size(), 50);
        const bool stopRequested = (fds[0].revents & POLLIN) != 0;
        if (stopRequested) break;
        if (pr < 0)
        {
            if (errno == EINTR) continue;
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "UiBridge: poll failed: {}", strerror(errno));
            break;
        }

        // ---- accept ---------------------------------------------------------
        if (pr > 0 && (fds[1].revents & POLLIN))
        {
            while (true)
            {
                int cfd = accept(_listenFd, nullptr, nullptr);
                if (cfd < 0) break;
                timeval tv{0, 100 * 1000};   // 100ms send timeout
                setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                // Inbound drain uses blocking recv until EAGAIN; without a
                // recv timeout the drain loop would park the IO thread.
                setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                int one = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                Client cl;
                cl.fd = cfd;
                cl.id = _nextClientId++;
                cl.authed = _cfg.token.empty();
                const bool authedNow = cl.authed;
                const int newId = cl.id;
                _clients.push_back(std::move(cl));
                _clientCount.store(static_cast<int>(_clients.size()), std::memory_order_relaxed);
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "UiBridge: client {} connected (total {})", cfd, (int)_clients.size());

                if (authedNow)
                {
                    std::string hello;
                    UiSnapshotBuilder::encodeHello(_cfg.sid, _cfg.readonly, hello);
                    sendTo(newId, hello);
                }
            }
        }

        // ---- inbound --------------------------------------------------------
        if (pr > 0)
        {
            for (size_t i = 2; i < fds.size(); ++i)
            {
                if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
                const int cid = fdOwners[i];
                char buf[4096];
                while (true)
                {
                    ssize_t n = recv(fds[i].fd, buf, sizeof(buf), 0);
                    if (n > 0)
                    {
                        auto it = std::find_if(_clients.begin(), _clients.end(),
                                               [cid](const Client& c) { return c.id == cid; });
                        if (it == _clients.end()) break;
                        it->inbuf.append(buf, static_cast<size_t>(n));
                        size_t pos;
                        while ((pos = it->inbuf.find('\n')) != std::string::npos)
                        {
                            std::string line = it->inbuf.substr(0, pos);
                            it->inbuf.erase(0, pos + 1);
                            try { handleLine(cid, line); }
                            catch (const std::exception& e)
                            {
                                WTSLogger::log_by_cat("strategy", LL_ERROR,
                                    "UiBridge: handleLine exception: {}", e.what());
                            }
                            catch (...)
                            {
                                WTSLogger::log_by_cat("strategy", LL_ERROR,
                                    "UiBridge: handleLine unknown exception");
                            }
                            // handleLine may have closed this client
                            if (std::find_if(_clients.begin(), _clients.end(),
                                             [cid](const Client& c) { return c.id == cid; })
                                == _clients.end())
                                break;
                        }
                        if (std::find_if(_clients.begin(), _clients.end(),
                                         [cid](const Client& c) { return c.id == cid; })
                            == _clients.end())
                            break;
                        continue;
                    }
                    if (n == 0) { closeClient(cid); break; }        // peer closed
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    closeClient(cid);
                    break;
                }
            }
        }

        // ---- heartbeat (only while someone is watching) ---------------------
        if (_cfg.heartbeatSec > 0 && !_clients.empty())
        {
            const auto nowSteady = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(nowSteady - lastHeartbeat).count();
            if (elapsed >= _cfg.heartbeatSec)
            {
                lastHeartbeat = nowSteady;
                if (_wakeWorker) _wakeWorker();   // -> enqueue_control(): silent-market keep-alive
            }
        }

        // ---- outbound -------------------------------------------------------
        if (_clients.empty()) continue;

        // 1) acks (command results from the worker)
        {
            std::deque<std::pair<int, UiEventFrame>> batch;
            {
                std::lock_guard<std::mutex> lk(_ackMtx);
                batch.swap(_acks);
            }
            for (const auto& p : batch)
            {
                std::string frame;
                UiSnapshotBuilder::encodeEvent(p.second, frame);
                sendTo(p.first, frame);
            }
        }

        // 2) events (broadcast; slow clients drop individually)
        std::vector<UiEventFrame> evBatch;
        {
            std::lock_guard<std::mutex> lk(_evtMtx);
            evBatch.assign(std::make_move_iterator(_events.begin()),
                           std::make_move_iterator(_events.end()));
            _events.clear();
        }
        if (!evBatch.empty())
        {
            std::string frame;
            for (const auto& ev : evBatch)
            {
                frame.clear();
                UiSnapshotBuilder::encodeEvent(ev, frame);
                for (const auto& c : _clients)
                    if (c.authed) sendTo(c.id, frame);
            }
        }

        // 3) snapshot (latest-wins; new clients get the current slot at once)
        UiSnapshotView snap;
        uint64_t snapGen = 0;
        {
            std::lock_guard<std::mutex> lk(_slotMtx);
            if (_slotGen > 0 && _slotGen != ~0ull)
            {
                snap = _slot;
                snapGen = _slotGen;
            }
            if (snapGen > 0) _slotPending = false;
        }
        if (snapGen > 0)
        {
            std::string frame;
            UiSnapshotBuilder::encodeSnapshot(snap, _cfg.sid, frame);

            // capture aux once per cycle; deliver after the snapshot
            std::string aux;
            bool auxPending = false;
            std::vector<int> auxDue;
            {
                std::lock_guard<std::mutex> lk(_auxMtx);
                if (_auxPending)
                {
                    aux = std::move(_auxJson);
                    _auxJson.clear();
                    _auxPending = false;
                    auxPending = true;
                }
            }
            if (auxPending)
            {
                for (const auto& c : _clients)
                    if (c.authed) auxDue.push_back(c.id);
            }

            for (auto& c : _clients)
            {
                if (!c.authed || c.lastSentGen == snapGen) continue;
                if (sendTo(c.id, frame)) c.lastSentGen = snapGen;
                if (auxPending &&
                    std::find(auxDue.begin(), auxDue.end(), c.id) != auxDue.end())
                {
                    sendTo(c.id, aux);
                }
            }
        }
    }

    WTSLogger::log_by_cat("strategy", LL_DEBUG, "UiBridge: IO thread stopped");
}

#else // _WIN32 — stubs (Windows UI not in P1 scope, see design doc)

uint16_t UiBridge::boundPort() const { return 0; }
bool UiBridge::start()
{
    WTSLogger::log_by_cat("strategy", LL_WARN,
        "UiBridge: not implemented on Windows — UI disabled");
    return false;
}
void UiBridge::stop() {}
bool UiBridge::snapshotDue(double) const { return false; }
void UiBridge::noteSnapshotSent(double) {}
void UiBridge::clearForceSnapshot() {}
void UiBridge::publishSnapshot(UiSnapshotView) {}
void UiBridge::pushEvent(const UiEventFrame&) {}
void UiBridge::publishAuxJson(std::string) {}
void UiBridge::drainCommands(const std::function<bool(const std::string&, std::string&)>&) {}
void UiBridge::queueAck(int, uint64_t, bool, const std::string&) {}
bool UiBridge::sendTo(int, const std::string&) { return false; }
void UiBridge::closeClient(int) {}
void UiBridge::handleLine(int, const std::string&) {}
void UiBridge::ioThreadMain() {}

#endif // _WIN32

} // namespace wt_option
