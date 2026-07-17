#include "OptionValueWriter.h"
#include "OptionGrid.h"
#include "OptionData.h"
#include "../WTSTools/WTSLogger.h"

#include <cmath>
#include <iomanip>

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

    // Write CSV header
    m_file << "time,code,expiry,strike,right,bid,ask,mid,theo,impliedVol,"
           << "theoVol,delta,gamma,vega,theta,position,active\n";
    m_file.flush();

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OptionValueWriter initialized: {}", m_filename);
    return true;
}

int32_t OptionValueWriter::writeValues(OptionGrid* grid, double now)
{
    if (!grid || !m_file.is_open()) return 0;

    m_file << std::fixed << std::setprecision(4);
    int32_t count = 0;

    for (const auto& od : grid->getAllOptions()) {
        if (!od) continue;
        const OptionValues& v = od->values(0);

        m_file << now << ","
               << od->getCode() << ","
               << od->getExpiry() << ","
               << od->getStrike() << ","
               << (od->getRight() == OR_Call ? "C" : "P") << ","
               << od->getBid() << ","
               << od->getAsk() << ","
               << od->getMid() << ","
               << v.theo() << ","
               << v.impliedVol() << ","
               << v.theoVol() << ","
               << v.greeks().delta() << ","
               << v.greeks().gamma() << ","
               << v.greeks().vega() << ","
               << v.greeks().theta() << ","
               << od->getPosition() << ","
               << (od->isActive() ? 1 : 0) << "\n";
        count++;
    }

    m_file.flush();
    m_lastWriteTime = now;
    m_writeCount++;
    return count;
}

void OptionValueWriter::close()
{
    if (m_file.is_open()) {
        m_file.close();
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "OptionValueWriter closed: {} writes to {}",
            m_writeCount, m_filename);
    }
}

} // namespace wt_option
