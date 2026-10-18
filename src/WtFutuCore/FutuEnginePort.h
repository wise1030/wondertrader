/*!
 * \file FutuEnginePort.h
 * \brief WtFutuCore 与 UFT 引擎之间唯一的实现依赖入口
 *
 * 设计目标：
 *   - 策略公共头文件只依赖 Includes 的抽象接口，不暴露 UftStraContext/EventNotifier；
 *   - 引擎实现细节收敛到 FutuEnginePort.cpp 一个编译单元；
 *   - 引擎侧接口变化时，只需修改 FutuEnginePort.cpp，不波及策略主体。
 */
#pragma once

namespace wtp
{
class IUftStraCtx;
class EventNotifier;
} // namespace wtp

namespace futu
{

/// 从 UFT 策略上下文解析引擎注入的 EventNotifier。
/// 返回 nullptr 表示当前上下文未提供告警通道（策略应降级为无告警运行）。
wtp::EventNotifier* resolveEventNotifier(wtp::IUftStraCtx* ctx);

/// 通过引擎告警通道发布一条消息；notifier 为空时静默降级。
/// 该函数让策略侧不必包含 EventNotifier.h 即可发送告警。
void notifyAlert(wtp::EventNotifier* notifier, const char* topic, const char* message);

} // namespace futu
