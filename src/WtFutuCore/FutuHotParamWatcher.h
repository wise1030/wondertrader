/*!
 * \file FutuHotParamWatcher.h
 * \brief hotparams.yaml 文件监视器 (轮询 mtime, 同步到共享内存)
 *
 * 设计目的:
 *   桥接 YAML 配置文件与 ShareManager 共享内存热更新链路。
 *   修改 hotparams.yaml 后无需重启策略即可生效。
 *
 * 实现参考:
 *   - 文件变更检测: WtCore/WtFilterMgr.cpp (last_write_time 轮询)
 *   - 共享内存写入: WtUftCore/ShareManager (set_value + commit_param_watcher)
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
    /// @param strategy_id 策略ID (共享内存 section 名)
    /// @param filepath    hotparams.yaml 完整路径
    /// @param hot_mgr     热参数管理器 (用于 syncFromFile)
    /// @param targets     热参数应用目标模块集合
    /// @param interval_ms 轮询间隔, 默认 1000ms
    bool start(const char* strategy_id, const char* filepath,
               FutuHotParamManager* hot_mgr,
               const FutuHotParamManager::Targets& targets,
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
    FutuHotParamManager::Targets _targets{};

    std::atomic<bool> _running{false};
    std::atomic<bool> _stop_flag{false};
    std::unique_ptr<std::thread> _worker;

    uint64_t _last_mtime{0};
};

} // namespace futu
