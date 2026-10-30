#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wt_option {

class OptionGrid;

class OptionValueWriter {
public:
    OptionValueWriter() = default;
    ~OptionValueWriter();
    OptionValueWriter(const OptionValueWriter&) = delete;
    OptionValueWriter& operator=(const OptionValueWriter&) = delete;

    void setOutputDir(const std::string& dir) { m_outputDir = dir; }
    void setStartTime(double timeSec) { m_startTime = timeSec; }
    void setEndTime(double timeSec) { m_endTime = timeSec; }
    void setOutputPeriod(double sec) { m_outputPeriod = sec; }

    bool init(uint32_t tdate);
    int32_t writeValues(OptionGrid* grid, double now);
    void close();

    bool shouldWrite(double now) const {
        if (!m_ready.load(std::memory_order_acquire)) return false;
        if (now < m_startTime || now > m_endTime) return false;
        return (now - m_lastWriteTime) >= m_outputPeriod;
    }

private:
    void ioLoop();

    std::string m_outputDir;
    std::string m_filename;
    std::ofstream m_file;

    mutable std::mutex m_queueMtx;
    std::condition_variable m_queueCv;
    std::vector<std::string> m_queue;
    std::thread m_ioThread;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_stopping{false};

    double m_startTime = 0;
    double m_endTime = 86400;
    double m_outputPeriod = 1.0;
    double m_lastWriteTime = -1;
    uint32_t m_tdate = 0;
    uint32_t m_writeCount = 0;
};

using OptionValueWriterPtr = std::shared_ptr<OptionValueWriter>;

} // namespace wt_option
