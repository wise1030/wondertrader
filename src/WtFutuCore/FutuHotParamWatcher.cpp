/*!
 * \file FutuHotParamWatcher.cpp
 * \brief hotparams.yaml 文件监视器实现
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
                                const FutuHotParamManager::Targets& targets,
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
    _targets = targets;
    _last_mtime = 0;
    _stop_flag.store(false);

    // 首次同步 (把文件当前值写入共享内存, 确保文件为权威源)
    if (!syncFileToSharedMemory())
    {
        WTSLogger::error("FutuHotParamWatcher: initial sync failed, watcher not started");
        return false;
    }

    _worker.reset(new std::thread([this]() { watchLoop(); }));
    _running.store(true);

    WTSLogger::info("FutuHotParamWatcher: started for {} (interval={}ms)", _filepath, _interval_ms);
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
        try
        {
            uint64_t mtime = boost::filesystem::last_write_time(boost::filesystem::path(_filepath));
            if (mtime > _last_mtime)
            {
                _last_mtime = mtime;
                WTSLogger::info("FutuHotParamWatcher: {} modified, syncing to shared memory", _filepath);
                syncFileToSharedMemory();
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

    uint32_t updated = _hot_mgr->syncFromFile(_filepath.c_str(), _targets, _strategy_id.c_str());
    return updated > 0;
}

} // namespace futu
