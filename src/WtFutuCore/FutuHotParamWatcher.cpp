/*!
 * \file FutuHotParamWatcher.cpp
 * \brief hotparams.yaml 文件监视器实现
 *
 * V8-P0-1: 每轮 parse+值比对 (替代 mtime 秒粒度门控), 只写共享内存+置脏,
 * applyAll 由策略 on_tick 在 _cb_mtx 内 drain (见 UftFutuMmStrategy::on_tick)。
 */
#include "FutuHotParamWatcher.h"
#include "FutuHotParamManager.h"

#include "../WTSTools/WTSLogger.h"

#include <boost/filesystem.hpp>
#include <chrono>

namespace futu
{

FutuHotParamWatcher::FutuHotParamWatcher()
{
}

FutuHotParamWatcher::~FutuHotParamWatcher()
{
    stop();
}

bool FutuHotParamWatcher::start(const char* strategy_id, const char* filepath,
                                FutuHotParamManager* hot_mgr,
                                uint32_t interval_ms)
{
    if (_running.load())
        return false;

    if (!strategy_id || !filepath || !hot_mgr || strlen(strategy_id) == 0 || strlen(filepath) == 0)
    {
        WTSLogger::error("FutuHotParamWatcher: invalid arguments");
        return false;
    }

    if (!boost::filesystem::exists(filepath))
    {
        WTSLogger::warn("FutuHotParamWatcher: {} not exists, watcher disabled", filepath);
        return false;
    }

    _strategy_id = strategy_id;
    _filepath = filepath;
    _interval_ms = interval_ms;
    _hot_mgr = hot_mgr;
    _stop_flag.store(false);

    // 首次同步 (把文件当前值写入共享内存, 确保文件为权威源)
    // 语义: 解析失败才视为失败; 空文件/无有效键 = 0 变更 = 成功
    // (旧逻辑 updated>0 才成功, 空 hotparams.yaml 会导致 watcher 不启动)
    if (!syncFileToSharedMemory())
    {
        WTSLogger::error("FutuHotParamWatcher: initial sync failed, watcher not started");
        return false;
    }

    _worker.reset(new std::thread([this]() { watchLoop(); }));
    _running.store(true);

    WTSLogger::info("FutuHotParamWatcher: started for {} (interval={}ms, apply on tick thread)",
                    _filepath,
                    _interval_ms);
    return true;
}

void FutuHotParamWatcher::stop()
{
    if (!_running.load())
        return;

    _stop_flag.store(true);
    if (_worker && _worker->joinable())
        _worker->join();

    _running.store(false);
    WTSLogger::info("FutuHotParamWatcher: stopped");
}

void FutuHotParamWatcher::watchLoop()
{
    while (!_stop_flag.load())
    {
        // V8-P0-1: 每轮全量 parse+diff -- 值比对在 syncFromFile 内完成,
        // 无变化零写入; 规避 mtime 秒级粒度丢同秒二次修改的问题
        try
        {
            if (!syncFileToSharedMemory())
            {
                // 文件正在被写入等暂态解析失败: 下轮重试, 不终止 watcher
                WTSLogger::error("FutuHotParamWatcher: parse {} failed, retry next poll", _filepath);
            }
        }
        catch (const std::exception& e)
        {
            WTSLogger::error("FutuHotParamWatcher: watch error - {}", e.what());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(_interval_ms));
    }
}

bool FutuHotParamWatcher::syncFileToSharedMemory()
{
    if (!_hot_mgr)
        return false;

    // -1 = 解析失败; >=0 = 值变更数 (可为 0)
    return _hot_mgr->syncFromFile(_filepath.c_str()) >= 0;
}

} // namespace futu
