/*!
 * \file CommandBus.h
 * \brief Structured hot-command dispatch — compact absorption of quantbox
 *        stratlib CommandRegistry/CommandServices.
 *
 * R9: replaces the raw "B,code,px,qty" string parsing of the manual_order
 * sync_param. Commands are registered by name with typed handlers; unknown
 * commands WARN instead of being silently ignored.
 *
 * Wire format (manual_order shared-memory string):
 *   STOP | RESUME | PANIC
 *   QMODE,<on|off|close>
 *   ORDER,B|S,<code>,<price>,<qty>
 *   CANCEL,<code>
 */
#pragma once

#include <functional>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

namespace wt_option {

class CommandBus {
public:
    using Handler = std::function<bool(const std::vector<std::string>& args, std::string& err)>;

    void registerCommand(const std::string& name, Handler h) {
        m_handlers[name] = std::move(h);
    }

    /// Dispatch one command line. Returns false when the line is malformed or
    /// the handler rejected it (err carries the reason either way).
    bool dispatch(const std::string& line, std::string& err) {
        if (line.empty()) { err = "empty"; return false; }
        std::vector<std::string> tokens;
        size_t start = 0;
        while (true) {
            size_t end = line.find(',', start);
            if (end == std::string::npos) { tokens.push_back(line.substr(start)); break; }
            tokens.push_back(line.substr(start, end - start));
            start = end + 1;
        }
        for (auto& t : tokens)
            while (!t.empty() && (t.back() == ' ' || t.back() == '\r' || t.back() == '\n')) t.pop_back();

        auto it = m_handlers.find(tokens[0]);
        if (it == m_handlers.end()) {
            err = "unknown command: " + tokens[0];
            return false;
        }
        try {
            return it->second(tokens, err);
        } catch (const std::exception& e) {
            err = std::string("command handler exception: ") + e.what();
            return false;
        } catch (...) {
            err = "command handler unknown exception";
            return false;
        }
    }

    bool has(const std::string& name) const { return m_handlers.count(name) != 0; }

private:
    std::unordered_map<std::string, Handler> m_handlers;
};

} // namespace wt_option
