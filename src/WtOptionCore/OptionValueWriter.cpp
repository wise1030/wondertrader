#include "OptionValueWriter.h"
#include "OptionGrid.h"
#include "OptionData.h"
#include "../WTSTools/WTSLogger.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace wt_option {

OptionValueWriter::~OptionValueWriter()
{
    close();
}

bool OptionValueWriter::init(uint32_t tdate)
{
    m_tdate = tdate;
    m_filename = m_outputDir + "/option_values_" + std::to_string(tdate) + ".csv";

    m_file.open(m_filename);
    if (!m_file.is_open()) {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "OptionValueWriter: cannot open {}", m_filename);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMtx);
        m_queue.emplace_back(
            "time,code,expiry,strike,right,bid,ask,mid,theo,impliedVol,"
            "theoVol,delta,gamma,vega,theta,position,active\n");
    }
    m_ready.store(true, std::memory_order_release);
    m_ioThread = std::thread(&OptionValueWriter::ioLoop, this);
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OptionValueWriter initialized (async IO): {}", m_filename);
    return true;
}

int32_t OptionValueWriter::writeValues(OptionGrid* grid, double now)
{
    if (!grid || !m_ready.load(std::memory_order_acquire)) return 0;

    std::ostringstream batch;
    batch << std::fixed << std::setprecision(4);
    int32_t count = 0;

    for (const auto& od : grid->getAllOptions()) {
        if (!od) continue;
        const OptionValues& v = od->values(0);
        batch << now << ','
              << od->getCode() << ','
              << od->getExpiry() << ','
              << od->getStrike() << ','
              << (od->getRight() == OR_Call ? "C" : "P") << ','
              << od->getBid() << ','
              << od->getAsk() << ','
              << od->getMid() << ','
              << v.theo() << ','
              << v.impliedVol() << ','
              << v.theoVol() << ','
              << v.greeks().delta() << ','
              << v.greeks().gamma() << ','
              << v.greeks().vega() << ','
              << v.greeks().theta() << ','
              << od->getPosition() << ','
              << (od->isActive() ? 1 : 0) << '\n';
        ++count;
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMtx);
        m_queue.push_back(batch.str());
    }
    m_queueCv.notify_one();
    m_lastWriteTime = now;
    ++m_writeCount;
    return count;
}

void OptionValueWriter::ioLoop()
{
    while (true) {
        std::vector<std::string> pending;
        {
            std::unique_lock<std::mutex> lock(m_queueMtx);
            m_queueCv.wait(lock, [&]() {
                return m_stopping.load(std::memory_order_acquire) || !m_queue.empty();
            });
            pending.swap(m_queue);
        }
        for (const auto& chunk : pending)
            m_file << chunk;
        if (!pending.empty())
            m_file.flush();
        if (m_stopping.load(std::memory_order_acquire) && pending.empty())
            break;
    }
}

void OptionValueWriter::close()
{
    if (!m_ready.exchange(false, std::memory_order_acq_rel) && !m_file.is_open()) return;
    m_stopping.store(true, std::memory_order_release);
    m_queueCv.notify_all();
    if (m_ioThread.joinable()) m_ioThread.join();
    if (m_file.is_open()) {
        m_file.close();
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "OptionValueWriter closed: {} writes to {}",
            m_writeCount, m_filename);
    }
}

} // namespace wt_option
