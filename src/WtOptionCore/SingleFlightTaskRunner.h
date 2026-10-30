#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <deque>
#include <string>
#include <thread>
#include <utility>

namespace wt_option {

/// Serial background runner with per-task single-flight semantics.
/// Intended for slow, idempotent tasks (fit jobs, IO dumps, pure stress
/// calculations) after the caller has captured an immutable snapshot.
class SingleFlightTaskRunner {
public:
    struct TaskResult {
        std::string name;
        bool success = false;
        std::string error;
    };

    using Task = std::function<TaskResult()>;

    SingleFlightTaskRunner() : m_thread([this]() { runLoop(); }) {}
    ~SingleFlightTaskRunner() { stop(); }

    SingleFlightTaskRunner(const SingleFlightTaskRunner&) = delete;
    SingleFlightTaskRunner& operator=(const SingleFlightTaskRunner&) = delete;

    /// Returns false if the same named task is already queued/running.
    bool submit(std::string name, Task task) {
        if (!task) return false;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            for (const auto& pending : m_queue)
                if (pending.first == name) return false;
            if (m_runningName == name) return false;
            m_queue.emplace_back(std::move(name), std::move(task));
        }
        m_cv.notify_one();
        return true;
    }

    bool isRunningOrQueued(const std::string& name) const {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_runningName == name) return true;
        for (const auto& pending : m_queue)
            if (pending.first == name) return true;
        return false;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            if (m_stopping) return;
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void runLoop() {
        while (true) {
            std::pair<std::string, Task> item;
            {
                std::unique_lock<std::mutex> lock(m_mtx);
                m_cv.wait(lock, [&]() { return m_stopping || !m_queue.empty(); });
                if (m_stopping && m_queue.empty()) return;
                item = std::move(m_queue.front());
                m_queue.pop_front();
                m_runningName = item.first;
            }

            try {
                TaskResult result = item.second();
                result.name = item.first;
                (void)result; // Caller can capture result inside task if needed.
            } catch (const std::exception& e) {
                (void)e;
            } catch (...) {
            }

            {
                std::lock_guard<std::mutex> lock(m_mtx);
                m_runningName.clear();
            }
        }
    }

    mutable std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::pair<std::string, Task>> m_queue;
    std::string m_runningName;
    bool m_stopping = false;
    std::thread m_thread;
};

} // namespace wt_option
