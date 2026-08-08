/*!
 * \file MonitorBridge.cpp
 * \project    WtFutuCore
 *
 * \brief    监控数据桥实现 (契约见 MonitorBridge.h)
 */
#include "MonitorBridge.h"
#include "FutuPortfolio.h"

#include "../Includes/IUftStraCtx.h"
#include "../WtUftCore/WtHelper.h"
#include "../WTSTools/WTSLogger.h"
#include "../Share/StdUtils.hpp"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <boost/filesystem.hpp>

namespace rj = rapidjson;

namespace futu
{

namespace
{
inline uint64_t steady_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// 原子写文件: tmp + rename (DataMgr 60s 轮询读, 避免半文件)
bool write_file_atomic(const std::string& path, const char* content)
{
    std::string tmp = path + ".tmp";
    FILE* fp = fopen(tmp.c_str(), "w");
    if (fp == nullptr)
        return false;
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
    return rename(tmp.c_str(), path.c_str()) == 0;
}
} // namespace

void MonitorBridge::init(const char* straId, const FutuPortfolio* portfolio, const Config& cfg)
{
    _stra_id = straId;
    _portfolio = portfolio;
    _cfg = cfg;

    if (_cfg.enabled)
        WTSLogger::info("MonitorBridge[{}] enabled, flush interval {}ms", _stra_id.c_str(), _cfg.flush_interval_ms);
}

void MonitorBridge::maybeFlush(wtp::IUftStraCtx* ctx)
{
    if (!_cfg.enabled || _portfolio == nullptr || ctx == nullptr)
        return;

    uint64_t now = steady_ms();
    if (now - _last_flush_ms < _cfg.flush_interval_ms)
        return;

    std::lock_guard<std::mutex> lock(_mtx);
    // 双重检查: 等锁期间可能已被另一线程刷新
    if (now - _last_flush_ms < _cfg.flush_interval_ms)
        return;
    _last_flush_ms = now;

    doFlush(ctx->stra_get_date());
}

void MonitorBridge::onSessionEnd(wtp::IUftStraCtx* ctx, uint32_t uTDate)
{
    if (!_cfg.enabled || _portfolio == nullptr)
        return;

    std::lock_guard<std::mutex> lock(_mtx);
    doFlush(uTDate);

    double closeprofit = 0, dynprofit = 0;
    for (const auto& cs : _portfolio->getAllContractsSnapshot()) {
        closeprofit += cs.realized_pnl;
        dynprofit += cs.unrealized_pnl;
    }
    appendFundsCsv(uTDate, closeprofit, dynprofit, 0.0);
}

void MonitorBridge::doFlush(uint32_t tdate)
{
    rj::Document root(rj::kObjectType);
    rj::Document::AllocatorType& allocator = root.GetAllocator();

    double total_profit = 0;
    double total_dynprofit = 0;

    // === 持仓 (契约对齐 CtaStraBaseCtx::save_data) ===
    rj::Value jPos(rj::kArrayType);
    for (const auto& cs : _portfolio->getAllContractsSnapshot()) {
        total_profit += cs.realized_pnl;
        total_dynprofit += cs.unrealized_pnl;

        double volume = std::abs(cs.position);
        if (volume == 0.0)
            continue; // GUI 也过滤 volume=0, 直接不写

        rj::Value pItem(rj::kObjectType);
        pItem.AddMember("code", rj::Value(cs.code.c_str(), allocator), allocator);
        pItem.AddMember("volume", volume, allocator);
        pItem.AddMember("closeprofit", cs.realized_pnl, allocator);
        pItem.AddMember("dynprofit", cs.unrealized_pnl, allocator);
        pItem.AddMember("lastentertime", 0, allocator);
        pItem.AddMember("lastexittime", 0, allocator);
        pItem.AddMember("frozen", 0.0, allocator);
        pItem.AddMember("frozendate", 0, allocator);

        // 分向明细: 多空分别一条 (对冲持仓时可能双向)
        rj::Value details(rj::kArrayType);
        auto addDetail = [&](bool isLong, double qty, double avgPrice) {
            rj::Value dItem(rj::kObjectType);
            dItem.AddMember("long", isLong, allocator);
            dItem.AddMember("price", avgPrice, allocator);
            dItem.AddMember("maxprice", 0.0, allocator);
            dItem.AddMember("minprice", 0.0, allocator);
            dItem.AddMember("volume", qty, allocator);
            dItem.AddMember("opentime", (uint64_t)0, allocator);
            dItem.AddMember("opentdate", tdate, allocator);
            dItem.AddMember("profit", cs.unrealized_pnl, allocator);
            dItem.AddMember("maxprofit", 0.0, allocator);
            dItem.AddMember("maxloss", 0.0, allocator);
            dItem.AddMember("opentag", "", allocator);
            dItem.AddMember("openbarno", 0, allocator);
            details.PushBack(dItem, allocator);
        };
        if (cs.long_qty > 0)
            addDetail(true, cs.long_qty, cs.long_avg);
        if (cs.short_qty > 0)
            addDetail(false, cs.short_qty, cs.short_avg);
        if (cs.long_qty <= 0 && cs.short_qty <= 0)
            addDetail(cs.position > 0, volume, cs.avg_cost); // 兜底: 净头寸

        pItem.AddMember("details", details, allocator);
        jPos.PushBack(pItem, allocator);
    }
    root.AddMember("positions", jPos, allocator);

    // === 资金 ===
    // total_fees 暂无分策略手续费归集 (框架未提供), 记 0
    rj::Value jFund(rj::kObjectType);
    jFund.AddMember("total_profit", total_profit, allocator);
    jFund.AddMember("total_dynprofit", total_dynprofit, allocator);
    jFund.AddMember("total_fees", 0.0, allocator);
    jFund.AddMember("tdate", tdate, allocator);
    root.AddMember("fund", jFund, allocator);

    rj::StringBuffer sb;
    rj::PrettyWriter<rj::StringBuffer> writer(sb);
    root.Accept(writer);

    std::string path = WtHelper::getStraDataDir();
    path += _stra_id + ".json";
    write_file_atomic(path, sb.GetString());
}

void MonitorBridge::appendFundsCsv(uint32_t tdate, double closeprofit, double dynprofit, double fees)
{
    // 契约对齐 DataMgr.get_funds: date,closeprofit,dynprofit,dynbalance,fee
    std::string dir = WtHelper::getOutputDir();
    dir += _stra_id + "/";
    if (!StdFile::exists(dir.c_str()))
        boost::filesystem::create_directories(dir);
    std::string path = dir + "funds.csv";

    // 同日重复收盘不重复追加
    FILE* fp = fopen(path.c_str(), "r");
    if (fp != nullptr) {
        char line[256] = {0};
        char last[256] = {0};
        while (fgets(line, sizeof(line), fp)) {
            if (strlen(line) > 8)
                strcpy(last, line);
        }
        fclose(fp);
        uint32_t lastDate = (uint32_t)atoi(last);
        if (lastDate == tdate)
            return;
    }

    fp = fopen(path.c_str(), "a");
    if (fp == nullptr) {
        WTSLogger::warn("MonitorBridge[{}] open {} for append failed", _stra_id.c_str(), path.c_str());
        return;
    }
    double dynbalance = closeprofit + dynprofit - fees;
    fprintf(fp, "%u,%.2f,%.2f,%.2f,%.2f\n", tdate, closeprofit, dynprofit, dynbalance, fees);
    fclose(fp);
}

} // namespace futu
