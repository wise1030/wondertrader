/*!
 * \file FutuHotParamWatcher.h
 * \brief hotparams.yaml 文件监视器 (轮询值比对, 同步到共享内存)
 *
 * 设计目的:
 *   桥接 YAML 配置文件与 ShareManager 共享内存热更新链路。
 *   修改 hotparams.yaml 后无需重启策略即可生效。
 *
 * V8-P0-1 线程模型:
 *   watcher 线程只做 parse+校验+值比对 -> 写共享内存 + 置 pending 标志;
 *   参数应用 (applyAll) 由策略 on_tick 在 _cb_mtx 内 drain -- 此前
 *   watcher 线程直接 applyAll 裸写主链路状态 (SignalAggregator 权重/
 *   FutuPortfolio 参数/策略配置结构体), 是唯一绕过回调锁的写者。
 *   值比对取代 mtime 门控 (boost last_write_time 秒级粒度丢同秒二次修改)。
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "FutuHotParamManager.h"

namespace futu
{

class FutuHotParamWatcher
{
public:
    FutuHotParamWatcher();
    ~FutuHotParamWatcher();

    /// 启动监视线程
    /// @param strategy_id 策略ID (日志)
    /// @param filepath    hotparams.yaml 完整路径
    /// @param hot_mgr     热参数管理器 (仅写共享内存+置脏, 不再 applyAll)
    /// @param interval_ms 轮询间隔, 默认 1000ms
    bool start(const char* strategy_id, const char* filepath,
               FutuHotParamManager* hot_mgr,
               uint32_t interval_ms = 1000);

    /// 停止监视线程
    void stop();

    bool isRunning() const { return _running.load(); }

private:
    void watchLoop();
    bool syncFileToSharedMemory();

private:
    std::string _strategy_id;
    std::string _filepath;
    uint32_t    _interval_ms;

    FutuHotParamManager* _hot_mgr{nullptr};

    std::atomic<bool> _running{false};
    std::atomic<bool> _stop_flag{false};
    std::unique_ptr<std::thread> _worker;
};

} // namespace futu
