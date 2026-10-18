/*!
 * \file FutuEnginePort.cpp
 * \brief WtFutuCore 与 UFT 引擎之间唯一的实现依赖入口
 *
 * 这是 WtFutuCore 内唯一 include UftStraContext.h / EventNotifier.h 的编译单元。
 * 策略主体（UftFutuMmStrategy.cpp）只调用 futu::resolveEventNotifier()。
 */
#include "FutuEnginePort.h"

#include "../WtUftCore/EventNotifier.h"
#include "../WtUftCore/UftStraContext.h"

namespace futu
{

wtp::EventNotifier* resolveEventNotifier(wtp::IUftStraCtx* ctx)
{
    if (ctx == nullptr)
        return nullptr;

    // WtUftRunner 始终以 UftStraContext 承载策略；保留 dynamic_cast，使未来
    // 其它 IUftStraCtx 实现可以安全降级为“无告警通道”，而不是未定义行为。
    auto* uft_ctx = dynamic_cast<wtp::UftStraContext*>(ctx);
    if (uft_ctx == nullptr)
        return nullptr;

    return uft_ctx->getEventNotifier();
}

void notifyAlert(wtp::EventNotifier* notifier, const char* topic, const char* message)
{
    if (notifier == nullptr || topic == nullptr || message == nullptr)
        return;

    notifier->notify(topic, message);
}

} // namespace futu
