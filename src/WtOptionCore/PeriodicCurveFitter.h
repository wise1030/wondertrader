/*!
 * \file PeriodicCurveFitter.h
 * \brief Periodic vol-curve fitter (migrated from quantbox optioncore/PeriodicCurveFitter)
 *
 * Original: longbeach::optioncore::PeriodicCurveFitter depended on longbeach
 * ClientContext/ClockMonitor (scheduled wakeup calls), ListenerList, PerfCounter,
 * Subscription, IVolCurve, BSplineVolCurve, OptionGrid.
 *
 * Migration (dependency-replacement only, fitting logic preserved):
 *  - namespace longbeach::optioncore -> wt_option
 *  - ClockMonitor scheduled wakeup removed. The host now calls onTimer(time)
 *    periodically; doFit() can also be called directly. The original
 *    onClockWakeup(ctv, swtv) self-rescheduling loop is replaced by onTimer.
 *  - getTime() reads an externally-set m_time (host calls setTime each tick).
 *  - ListenerList<fitCompletedEventSink> -> std::vector<std::function>
 *  - boost::function -> std::function
 *  - boost::shared_ptr -> std::shared_ptr
 *  - boost::posix_time / duration_t -> double seconds
 *  - Subscription -> opaque wt_option::Subscription token
 *  - PerfCounter -> local PerfCounter (see OptionPricer2.h)
 */
#pragma once

#include "IVolCurve.h"
#include "optioncoretypes.h"

#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <cstdint>

namespace wt_option {

class OptionGrid;
class OptionPricer2;
class ExpiryData;
class StrikeData;
struct Subscription;  // from IOptionGrid.h

class PeriodicCurveFitter
{
public:
    // fit-completed event (replaces ListenerList<fitCompletedEventSink>)
    typedef std::function<void(const PeriodicCurveFitter&)> fitCompletedEventSink;
    typedef std::vector<fitCompletedEventSink> fitCompletedEventSource;

    fitCompletedEventSource& fitCompletedEvent() { return m_fitCompletedEvent; }

public:
    // ClientContextPtr removed; ClockMonitor removed.
    PeriodicCurveFitter(OptionGrid* grid
                       , const std::shared_ptr<OptionPricer2>& black_pxr
                       , double vol_fitting_start_time       // seconds from midnight
                       , double vol_fitting_end_time
                       , const std::map<int32_t, double>& vol_fitting_good_points_thresh
                       , double vol_fitting_decay_window
                       , double period                       // seconds
                       , double vol_fitting_threshold = 0
                       , bool   vol_fitting_to_all_expiries = false);

    void setTraceLevel(int32_t lvl) { m_traceLevel = lvl; }

    bool   doFit();
    double getLastFitTime() const { return m_tvLastFit; }

    // ADAPTATION: replaces onClockWakeup. Host calls this on each timer tick;
    // PeriodicCurveFitter decides whether to fit based on m_period elapsed.
    // Returns true if doFit was called (fit attempted), false if period not elapsed.
    bool   onTimer(double time);

    // Host sets current time (replaces ClockMonitor::getTime())
    double getTime() const { return m_time; }
    void   setTime(double t) { m_time = t; }

private:
    class FitData
    {
    public:
        void print();
        bool isValid() { return stkvol_points.size() > 0; }

        double                 tv = 0.0;
        IVolCurve::dataset_t   stkvol_points;
        uint32_t               exp = 0;
        double                 atm_forward = 0.0;
        double                 atm_vol = 0.0;
    };

    bool doFit_imp();
    bool fitToExpiry(uint32_t exp);
    void updateFitData(const FitData& fd);
    void __fitExpiry_imp(const std::pair<const uint32_t, std::shared_ptr<ExpiryData>>& v);

    int32_t getTraceLevel() const { return m_traceLevel; }

private:
    OptionGrid*                     m_spGrid = nullptr;
    std::shared_ptr<OptionPricer2>  m_spBlackPricer;
    double                          m_tvLastFit = 0.0;
    std::map<uint32_t, FitData>     m_fit_data_prev;

    double                          m_volFittingStartTime;
    double                          m_volFittingEndTime;
    double                          m_volFittingDecayWindow;
    double                          m_period;       // seconds
    double                          m_volFittingThreshold;

    IVolCurvePtr                    m_volFittingGoodPointsThreshCurve;

    int32_t                         m_traceLevel = 1;
    std::map<uint32_t, bool>        m_expiryFitStatus;
    fitCompletedEventSource         m_fitCompletedEvent;

    // local perf counters (accumulate seconds; used only for logging)
    double m_perfCounter_total = 0.0;
    double m_imp_perfCounter_total = 0.0;

    // time state
    double m_time = 0.0;          // set by host via setTime()
    double m_tvNextFit = 0.0;     // next scheduled fit time
};
using PeriodicCurveFitterPtr = std::shared_ptr<PeriodicCurveFitter>;

} // namespace wt_option
