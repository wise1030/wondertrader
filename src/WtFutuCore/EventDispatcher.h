#pragma once

#include <functional>
#include <vector>

namespace futu {

//==============================================================================
// C10: Internal Event Dispatcher (synchronous listener, NOT queue bus)
//
// Rationale: TradingState is currently written by 6+ classes across 2 threads
// (MdSpi/TdSpi), serialized by _cb_mtx. This dispatcher provides the
// infrastructure to converge TradingState to a single-writer model in a
// future phase. For now, _cb_mtx is retained and events are informational.
//
// Hot path stays synchronous (direct call, no queue) - this system's latency
// advantage depends on synchronous tick-to-quote. A disruptor/queue would be
// a negative optimization.
//
//==============================================================================
// TradingState Conversion Matrix Audit (C10 prerequisite)
//==============================================================================
//
// Writer                   | Thread | Transition         | Trigger
// -------------------------|--------|--------------------|---------------------------
// RiskCoordinator          | MdSpi  | ->RISK_HALTED      | position/loss/rate breach
// RiskCoordinator          | MdSpi  | ->TOXICITY         | toxicity cooloff
// StrategyCoordinator      | MdSpi  | ->exitToQuoting    | night session end
// StrategyCoordinator      | MdSpi  | ->MARKET           | market state paused
// FutuRuntimeOps           | TdSpi  | ->RISK_HALTED      | order error threshold
// FutuRuntimeOps           | TdSpi  | ->ERROR            | order error
// FutuRuntimeOps           | TdSpi  | ->RISK_HALTED      | strong flat (closeout)
// UftFutuMmStrategy        | MdSpi  | ->RISK_HALTED      | coordinator null fail-safe
// CloseoutOrchestrator     | TdSpi  | ->enterCloseout    | closeout phase start
// CloseoutTrigger          | TdSpi  | ->enterCloseout    | closeout trigger
//
// Total: 15 write points, 6 classes, 2 threads
//
// Key timing assumptions:
// 1. RiskCoordinator writes (MdSpi) must precede processQuoting (Stage 4 < 6)
// 2. FutuRuntimeOps writes (TdSpi) happen during fill, serialized by _cb_mtx
// 3. Closeout writes assume CLOSEOUT phase blocks quoting (no re-entry)
// 4. Cross-thread writes currently serialized by _cb_mtx recursive mutex
//
// Convergence plan (future phase, post C12):
// - All TradingState transitions fire via dispatcher as events
// - Single TradingStateWriter listener processes transitions
// - Eliminates 6 scattered writers -> 1 centralized writer
// (V8-R4: FUTU_CALLBACK_LOCK=0 已废弃删除 -- 本注释为历史说明)
//==============================================================================

enum class CoordinatorEvent
{
    TickReceived,             // processTick entry (MdSpi)
    FillReceived,             // processTradeFill entry (TdSpi)
    RiskAction,               // risk check result (halting/cooloff/resume)
    TradingStateTransition,   // any TradingState phase change
    CloseoutPhase,            // closeout start/end
};

using EventListener = std::function<void(CoordinatorEvent)>;

class EventDispatcher
{
public:
    void subscribe(EventListener listener) { _listeners.push_back(std::move(listener)); }

    /// Synchronous dispatch (hot path direct call, no queue)
    void dispatch(CoordinatorEvent event)
    {
        // V8 §5#6: 零订阅时避免每帧两枪空转遍历 (hasListeners 此前存在但未使用)
        if (_listeners.empty())
            return;
        for (auto& l : _listeners)
            l(event);
    }

    bool hasListeners() const { return !_listeners.empty(); }

private:
    std::vector<EventListener> _listeners;
};

} // namespace futu
