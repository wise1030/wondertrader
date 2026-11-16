#include "OptionCommandHandlers.h"

#include <exception>

namespace wt_option {

void OptionCommandHandlers::registerAll(CommandBus& bus, Hooks hooks)
{
    m_hooks = std::move(hooks);

    bus.registerCommand("STOP", [this](auto&, std::string&) {
        if (m_hooks.setState) m_hooks.setState(TradingStateType::Disabled, "hotcmd_stop");
        return true;
    });
    bus.registerCommand("RESUME", [this](auto&, std::string&) {
        if (!m_hooks.setState) return false;
        const bool ready = m_hooks.channelReady && m_hooks.channelReady();
        m_hooks.setState(ready ? TradingStateType::Normal : TradingStateType::Disabled,
                         "hotcmd_resume");
        return true;
    });
    bus.registerCommand("PANIC", [this](auto&, std::string&) {
        if (m_hooks.setState) m_hooks.setState(TradingStateType::Panic, "hotcmd_panic");
        return true;
    });
    bus.registerCommand("QMODE", [this](const auto& args, std::string& err) {
        if (args.size() < 2) { err = "QMODE needs <on|off|close>"; return false; }
        if (!m_hooks.setQModeAll) { err = "not initialized"; return false; }
        return m_hooks.setQModeAll(args[1], err);
    });
    bus.registerCommand("ORDER", [this](const auto& args, std::string& err) {
        if (args.size() < 5) { err = "ORDER needs B|S,code,price,qty"; return false; }
        if (!m_hooks.executeOrder) { err = "not initialized"; return false; }
        try {
            const bool isBuy = (args[1] == "B" || args[1] == "b");
            const double px = std::stod(args[3]);
            const uint32_t qty = static_cast<uint32_t>(std::stoul(args[4]));
            const int32_t ret = m_hooks.executeOrder(args[2], isBuy, px, qty);
            if (ret == 0) { err = "order rejected (limits/channel)"; return false; }
            return true;
        } catch (const std::exception& e) {
            err = std::string("invalid ORDER args: ") + e.what();
            return false;
        }
    });
    bus.registerCommand("CANCEL", [this](const auto& args, std::string& err) {
        if (args.size() < 2) { err = "CANCEL needs code"; return false; }
        if (!m_hooks.cancelAll) { err = "not initialized"; return false; }
        m_hooks.cancelAll(args[1]);
        return true;
    });
    // P2: cancel one order by framework localid (UI double-click cancel).
    bus.registerCommand("CANCELO", [this](const auto& args, std::string& err) {
        if (args.size() < 2) { err = "CANCELO needs localid"; return false; }
        if (!m_hooks.cancelOne) { err = "not initialized"; return false; }
        try {
            const uint32_t localid = static_cast<uint32_t>(std::stoul(args[1]));
            return m_hooks.cancelOne(localid, err);
        } catch (const std::exception& e) {
            err = std::string("invalid CANCELO localid: ") + e.what();
            return false;
        }
    });
    // P2: hot parameter write (whitelist lives in the strategy hook).
    bus.registerCommand("SETPARAM", [this](const auto& args, std::string& err) {
        if (args.size() < 3) { err = "SETPARAM needs key,value"; return false; }
        if (!m_hooks.setHotParam) { err = "not initialized"; return false; }
        try {
            const double value = std::stod(args[2]);
            return m_hooks.setHotParam(args[1], value, err);
        } catch (const std::exception& e) {
            err = std::string("invalid SETPARAM value: ") + e.what();
            return false;
        }
    });
}

} // namespace wt_option
