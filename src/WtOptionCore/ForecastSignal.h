/*!
 * \file ForecastSignal.h
 * \brief Basic Trend Forecast Signal (MA Crossover)
 */
#pragma once

#include "IAlphaSignal.h"
#include <deque>
#include <numeric>

namespace wt_option {

class ForecastSignal : public IAlphaSignal
{
public:
    ForecastSignal() : m_fastWindow(10), m_slowWindow(30), m_value(0.0) {}
    
    virtual bool init(const std::string& config) override {
        // Parse config json string... (omitted for brevity)
        // Set windows
        return true;
    }
    
    virtual void onTick(OptionData* option, const wtp::WTSTickData* tick) override {
        double px = tick->price();
        m_history.push_back(px);
        if(m_history.size() > m_slowWindow) m_history.pop_front();
        
        if(m_history.size() >= m_slowWindow) {
            double fastAvg = std::accumulate(m_history.end() - m_fastWindow, m_history.end(), 0.0) / m_fastWindow;
            double slowAvg = std::accumulate(m_history.begin(), m_history.end(), 0.0) / m_slowWindow;
            
            // Signal: Normalized diff
            m_value = (fastAvg - slowAvg) / px * 100.0; // Pct diff
        }
    }
    
    virtual double getValue() const override { return m_value; }
    virtual const std::string& getName() const override { static std::string name="Forecast"; return name; }
    
private:
    std::deque<double> m_history;
    int m_fastWindow;
    int m_slowWindow;
    double m_value;
};

} // namespace wt_option
