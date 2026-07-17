#pragma once

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <cstdint>
#include <memory>
#include <functional>

namespace wt_option {

class OptionGrid;
class OptionData;

class OptionValueWriter {
public:
    OptionValueWriter() = default;
    ~OptionValueWriter();

    // Configure output
    void setOutputDir(const std::string& dir) { m_outputDir = dir; }
    void setStartTime(double timeSec) { m_startTime = timeSec; }
    void setEndTime(double timeSec) { m_endTime = timeSec; }
    void setOutputPeriod(double sec) { m_outputPeriod = sec; }

    // Initialize: open output file for the given trading date
    bool init(uint32_t tdate);

    // Write one snapshot of all option values
    // Returns number of options written
    int32_t writeValues(OptionGrid* grid, double now);

    // Close file
    void close();

    // Check if it's time to write (based on outputPeriod)
    bool shouldWrite(double now) const {
        if (!m_file.is_open()) return false;
        if (now < m_startTime || now > m_endTime) return false;
        return (now - m_lastWriteTime) >= m_outputPeriod;
    }

    // Keep history of written values (for debugging/replay)
    void setKeepHistory(bool keep) { m_keepHistory = keep; }

private:
    std::string m_outputDir;
    std::string m_filename;
    std::ofstream m_file;

    double m_startTime = 0;
    double m_endTime = 86400;   // 24h in seconds
    double m_outputPeriod = 1.0; // seconds
    double m_lastWriteTime = -1;
    bool m_keepHistory = false;
    uint32_t m_tdate = 0;
    uint32_t m_writeCount = 0;
};

using OptionValueWriterPtr = std::shared_ptr<OptionValueWriter>;

} // namespace wt_option
