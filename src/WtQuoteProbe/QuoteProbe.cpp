// ============================================================================
// WtQuoteProbe — CTP 原生双边报价 (ReqQuoteInsert) 全链路抓包诊断工具
// ============================================================================
// 目的: 在动 WtUftCore L0 实现前，验证 broker 是否真支持双边报价 + 抓取
//       OnRspQuoteInsert / OnRtnQuote / OnRtnOrder / OnRtnTrade 完整字段
//
// 流程:
//   1. 连接 trader front -> Authenticate -> Login -> SettlementConfirm
//   2. 查询合约 InstrumentField (要 PriceTick / 涨跌停)
//   3. 查询深度行情 (取 LastPrice 作基准)
//   4. ReqQuoteInsert(bid=last-20*tick, ask=last+20*tick, qty=1)
//   5. 等 30s 抓所有回调
//   6. ReqQuoteAction(撤单)
//   7. 等 5s 抓回调
//   8. 退出，生成 report
//
// 编译: g++ -std=c++17 -O0 -g -fPIC QuoteProbe.cpp \
//         -I../API/CTP6.3.15 -L../API/CTP6.3.15/linux \
//         -Wl,-rpath,. -lthosttraderapi_se -lpthread \
//         -o QuoteProbe
// ============================================================================

#include "ThostFtdcTraderApi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------- 配置 ----------
struct ProbeConfig {
    std::string front     = "tcp://180.96.49.192:59205";
    std::string broker    = "4600";
    std::string user      = "010531";
    std::string pass      = "123456";
    std::string appid     = "hyzb_trader_1.0";
    std::string authcode  = "95XW2SCJG6JT4G6J";
    std::string instrument= "ag2606";   // 默认合约
    std::string exchange  = "SHFE";
    double      lastPrice = 0.0;        // 由 OnRspQryDepthMarketData 填充
    double      priceTick = 1.0;        // 由 OnRspQryInstrument 填充
    int         tickOffset= 20;         // bid/ask 偏移 tick 数
    int         qty       = 1;
};

static ProbeConfig g_cfg;
static std::mutex   g_logMtx;
static std::ofstream g_logFile;

static void plog(const char* tag, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count() % 1000;
    char ts[32];
    std::tm tmv{};
    localtime_r(&t, &tmv);
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)ms);
    std::fprintf(stdout, "[%s][%-22s] %s\n", ts, tag, msg.c_str());
    std::fflush(stdout);
    if (g_logFile.is_open()) {
        g_logFile << "[" << ts << "][" << tag << "] " << msg << "\n";
        g_logFile.flush();
    }
}

#define PLOG(tag, fmt, ...) do { \
    char buf[2048]; std::snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__); \
    plog(tag, buf); \
} while (0)

// ============================================================================
// SPI 实现
// ============================================================================
class QuoteProbeSpi : public CThostFtdcTraderSpi {
public:
    CThostFtdcTraderApi* m_api = nullptr;
    std::atomic<int> m_reqId{0};
    std::atomic<bool> m_logged{false};
    std::atomic<bool> m_settled{false};
    std::atomic<bool> m_instReady{false};
    std::atomic<bool> m_dmReady{false};
    std::atomic<bool> m_quoteSent{false};
    std::atomic<bool> m_done{false};

    int    m_frontId   = 0;
    int    m_sessionId = 0;
    char   m_orderRefBid[16] = {0};
    char   m_orderRefAsk[16] = {0};
    char   m_quoteSysID[32]  = {0};
    char   m_quoteExchgID[16]= {0};

    int nextReqId() { return ++m_reqId; }

    // ---------- 连接 / 认证 ----------
    void OnFrontConnected() override {
        PLOG("OnFrontConnected", "trader front connected, sending Authenticate");
        CThostFtdcReqAuthenticateField req{};
        std::strcpy(req.BrokerID, g_cfg.broker.c_str());
        std::strcpy(req.UserID,   g_cfg.user.c_str());
        std::strcpy(req.AppID,    g_cfg.appid.c_str());
        std::strcpy(req.AuthCode, g_cfg.authcode.c_str());
        int r = m_api->ReqAuthenticate(&req, nextReqId());
        PLOG("ReqAuthenticate", "sent ret=%d", r);
    }

    void OnFrontDisconnected(int reason) override {
        PLOG("OnFrontDisconnected", "reason=0x%x", reason);
    }

    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* rsp,
                           CThostFtdcRspInfoField* info,
                           int rid, bool isLast) override {
        if (info && info->ErrorID != 0) {
            PLOG("OnRspAuthenticate", "ERROR id=%d msg=%s", info->ErrorID, info->ErrorMsg);
            return;
        }
        PLOG("OnRspAuthenticate", "OK, sending Login");
        CThostFtdcReqUserLoginField req{};
        std::strcpy(req.BrokerID, g_cfg.broker.c_str());
        std::strcpy(req.UserID,   g_cfg.user.c_str());
        std::strcpy(req.Password, g_cfg.pass.c_str());
        int r = m_api->ReqUserLogin(&req, nextReqId());
        PLOG("ReqUserLogin", "sent ret=%d", r);
    }

    void OnRspUserLogin(CThostFtdcRspUserLoginField* rsp,
                        CThostFtdcRspInfoField* info,
                        int rid, bool isLast) override {
        if (info && info->ErrorID != 0) {
            PLOG("OnRspUserLogin", "ERROR id=%d msg=%s", info->ErrorID, info->ErrorMsg);
            return;
        }
        m_frontId   = rsp->FrontID;
        m_sessionId = rsp->SessionID;
        PLOG("OnRspUserLogin", "OK FrontID=%d SessionID=%d MaxOrderRef=%s TradingDay=%s",
             m_frontId, m_sessionId, rsp->MaxOrderRef, rsp->TradingDay);
        m_logged = true;

        // 结算单确认
        CThostFtdcSettlementInfoConfirmField req{};
        std::strcpy(req.BrokerID,   g_cfg.broker.c_str());
        std::strcpy(req.InvestorID, g_cfg.user.c_str());
        int r = m_api->ReqSettlementInfoConfirm(&req, nextReqId());
        PLOG("ReqSettlementConfirm", "sent ret=%d", r);
    }

    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* rsp,
                                    CThostFtdcRspInfoField* info,
                                    int rid, bool isLast) override {
        if (info && info->ErrorID != 0) {
            PLOG("OnRspSettlementConfirm", "ERROR id=%d msg=%s", info->ErrorID, info->ErrorMsg);
        } else {
            PLOG("OnRspSettlementConfirm", "OK");
        }
        m_settled = true;
        // 查询合约
        CThostFtdcQryInstrumentField req{};
        std::strcpy(req.InstrumentID, g_cfg.instrument.c_str());
        std::strcpy(req.ExchangeID,   g_cfg.exchange.c_str());
        int r = m_api->ReqQryInstrument(&req, nextReqId());
        PLOG("ReqQryInstrument", "sent ret=%d code=%s", r, g_cfg.instrument.c_str());
    }

    void OnRspQryInstrument(CThostFtdcInstrumentField* rsp,
                            CThostFtdcRspInfoField* info,
                            int rid, bool isLast) override {
        if (rsp) {
            g_cfg.priceTick = rsp->PriceTick;
            PLOG("OnRspQryInstrument",
                 "code=%s exch=%s tick=%.4f MaxLimitOrderVol=%d volMult=%d",
                 rsp->InstrumentID, rsp->ExchangeID, rsp->PriceTick,
                 rsp->MaxLimitOrderVolume, rsp->VolumeMultiple);
        }
        if (isLast) {
            m_instReady = true;
            // 查询行情快照
            CThostFtdcQryDepthMarketDataField req{};
            std::strcpy(req.InstrumentID, g_cfg.instrument.c_str());
            int r = m_api->ReqQryDepthMarketData(&req, nextReqId());
            PLOG("ReqQryDepthMarketData", "sent ret=%d", r);
        }
    }

    void OnRspQryDepthMarketData(CThostFtdcDepthMarketDataField* rsp,
                                 CThostFtdcRspInfoField* info,
                                 int rid, bool isLast) override {
        if (rsp) {
            g_cfg.lastPrice = rsp->LastPrice;
            PLOG("OnRspQryDepthMarketData",
                 "code=%s LastPrice=%.2f Bid1=%.2f Ask1=%.2f UpLimit=%.2f DnLimit=%.2f",
                 rsp->InstrumentID, rsp->LastPrice, rsp->BidPrice1, rsp->AskPrice1,
                 rsp->UpperLimitPrice, rsp->LowerLimitPrice);
        }
        if (isLast) {
            m_dmReady = true;
            // 触发报价/下单
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const char* mode = std::getenv("QP_MODE");
            if (mode && std::string(mode) == "order") {
                sendOrder();
            } else {
                sendQuote();
            }
        }
    }

    // ---------- 普通限价单 (ReqOrderInsert) ----------
    // 用来对比交易角色: Quote 路径需要 SHFE 做市商角色, Order 路径只需要普通投机交易角色
    void sendOrder() {
        if (m_quoteSent) return;
        if (g_cfg.lastPrice <= 0) {
            PLOG("sendOrder", "ERROR: lastPrice=0, abort");
            m_done = true;
            return;
        }
        // 单边 BID 远离市价, 不会成交
        double bidPrice = g_cfg.lastPrice - g_cfg.tickOffset * g_cfg.priceTick;

        CThostFtdcInputOrderField req{};
        std::strcpy(req.BrokerID,    g_cfg.broker.c_str());
        std::strcpy(req.InvestorID,  g_cfg.user.c_str());
        std::strcpy(req.UserID,      g_cfg.user.c_str());
        std::strcpy(req.InstrumentID,g_cfg.instrument.c_str());
        std::strcpy(req.ExchangeID,  g_cfg.exchange.c_str());

        int oref = nextReqId() + 2000;
        std::snprintf(req.OrderRef, sizeof(req.OrderRef), "%d", oref);
        std::snprintf(m_orderRefBid, sizeof(m_orderRefBid), "%d", oref);

        req.OrderPriceType    = THOST_FTDC_OPT_LimitPrice;
        req.Direction         = THOST_FTDC_D_Buy;
        req.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
        req.CombHedgeFlag[0]  = THOST_FTDC_HF_Speculation;
        req.LimitPrice        = bidPrice;
        req.VolumeTotalOriginal = g_cfg.qty;
        req.TimeCondition     = THOST_FTDC_TC_GFD;     // 当日有效
        req.VolumeCondition   = THOST_FTDC_VC_AV;      // 任意数量
        req.MinVolume         = 1;
        req.ContingentCondition = THOST_FTDC_CC_Immediately;
        req.ForceCloseReason  = THOST_FTDC_FCC_NotForceClose;
        req.IsAutoSuspend     = 0;

        PLOG("ReqOrderInsert",
             "code=%s OrderRef=%s BUY %d@%.2f (last=%.2f tick=%.4f off=%d)",
             g_cfg.instrument.c_str(), req.OrderRef,
             req.VolumeTotalOriginal, req.LimitPrice,
             g_cfg.lastPrice, g_cfg.priceTick, g_cfg.tickOffset);

        int r = m_api->ReqOrderInsert(&req, nextReqId());
        PLOG("ReqOrderInsert", "API ret=%d", r);
        m_quoteSent = true;

        // 5 秒后结束 (普通单不撤, 看回报状态就行)
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            m_done = true;
        }).detach();
    }

    // ---------- 双边报价插入 ----------
    void sendQuote() {
        if (m_quoteSent) return;
        if (g_cfg.lastPrice <= 0) {
            PLOG("sendQuote", "ERROR: lastPrice=0, abort");
            m_done = true;
            return;
        }

        double bidPrice = g_cfg.lastPrice - g_cfg.tickOffset * g_cfg.priceTick;
        double askPrice = g_cfg.lastPrice + g_cfg.tickOffset * g_cfg.priceTick;

        CThostFtdcInputQuoteField req{};
        std::strcpy(req.BrokerID,   g_cfg.broker.c_str());
        std::strcpy(req.InvestorID, g_cfg.user.c_str());
        std::strcpy(req.UserID,     g_cfg.user.c_str());
        std::strcpy(req.InstrumentID, g_cfg.instrument.c_str());
        std::strcpy(req.ExchangeID,   g_cfg.exchange.c_str());

        int qref = nextReqId() + 1000;  // 远离 reqid
        std::snprintf(req.QuoteRef, sizeof(req.QuoteRef), "%d", qref);
        std::snprintf(m_orderRefBid, sizeof(m_orderRefBid), "%d", qref);
        std::snprintf(m_orderRefAsk, sizeof(m_orderRefAsk), "%d", qref);

        req.BidPrice  = bidPrice;
        req.BidVolume = g_cfg.qty;
        req.BidOffsetFlag = THOST_FTDC_OF_Open;
        req.BidHedgeFlag  = THOST_FTDC_HF_Speculation;

        req.AskPrice  = askPrice;
        req.AskVolume = g_cfg.qty;
        req.AskOffsetFlag = THOST_FTDC_OF_Open;
        req.AskHedgeFlag  = THOST_FTDC_HF_Speculation;

        PLOG("ReqQuoteInsert",
             "code=%s QuoteRef=%s BID %d@%.2f ASK %d@%.2f (last=%.2f tick=%.4f off=%d)",
             g_cfg.instrument.c_str(), req.QuoteRef,
             req.BidVolume, req.BidPrice, req.AskVolume, req.AskPrice,
             g_cfg.lastPrice, g_cfg.priceTick, g_cfg.tickOffset);

        int r = m_api->ReqQuoteInsert(&req, nextReqId());
        PLOG("ReqQuoteInsert", "API ret=%d", r);
        m_quoteSent = true;

        // 30 秒后撤单
        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            sendQuoteAction();
        }).detach();
    }

    // ---------- 双边报价撤单 ----------
    void sendQuoteAction() {
        if (m_quoteSysID[0] == 0) {
            PLOG("sendQuoteAction", "WARN: QuoteSysID not yet captured, retry in 2s");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (m_quoteSysID[0] == 0) {
                PLOG("sendQuoteAction", "ERROR: still no QuoteSysID, skip cancel");
                std::this_thread::sleep_for(std::chrono::seconds(5));
                m_done = true;
                return;
            }
        }
        CThostFtdcInputQuoteActionField req{};
        std::strcpy(req.BrokerID,   g_cfg.broker.c_str());
        std::strcpy(req.InvestorID, g_cfg.user.c_str());
        std::strcpy(req.UserID,     g_cfg.user.c_str());
        std::strcpy(req.InstrumentID, g_cfg.instrument.c_str());
        std::strcpy(req.ExchangeID,   m_quoteExchgID[0] ? m_quoteExchgID : g_cfg.exchange.c_str());
        std::strcpy(req.QuoteSysID,   m_quoteSysID);
        req.ActionFlag = THOST_FTDC_AF_Delete;
        req.FrontID    = m_frontId;
        req.SessionID  = m_sessionId;
        std::strcpy(req.QuoteRef, m_orderRefBid);

        PLOG("ReqQuoteAction", "cancel QuoteSysID=%s exch=%s",
             req.QuoteSysID, req.ExchangeID);
        int r = m_api->ReqQuoteAction(&req, nextReqId());
        PLOG("ReqQuoteAction", "API ret=%d", r);

        std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            m_done = true;
        }).detach();
    }

    // ---------- 双边报价回报 ----------
    void OnRspQuoteInsert(CThostFtdcInputQuoteField* rsp,
                          CThostFtdcRspInfoField* info,
                          int rid, bool isLast) override {
        if (info && info->ErrorID != 0) {
            PLOG("OnRspQuoteInsert",
                 "REJECTED id=%d msg=%s QuoteRef=%s code=%s",
                 info->ErrorID, info->ErrorMsg,
                 rsp ? rsp->QuoteRef : "?", rsp ? rsp->InstrumentID : "?");
        } else {
            PLOG("OnRspQuoteInsert", "OK QuoteRef=%s rid=%d isLast=%d",
                 rsp ? rsp->QuoteRef : "?", rid, isLast);
        }
    }

    void OnErrRtnQuoteInsert(CThostFtdcInputQuoteField* rsp,
                             CThostFtdcRspInfoField* info) override {
        PLOG("OnErrRtnQuoteInsert",
             "id=%d msg=%s QuoteRef=%s code=%s",
             info ? info->ErrorID : 0,
             info ? info->ErrorMsg : "?",
             rsp ? rsp->QuoteRef : "?", rsp ? rsp->InstrumentID : "?");
    }

    void OnRtnQuote(CThostFtdcQuoteField* q) override {
        if (!q) return;
        // 捕获 QuoteSysID 用于撤单
        if (m_quoteSysID[0] == 0 && q->QuoteSysID[0] != 0) {
            std::strcpy(m_quoteSysID, q->QuoteSysID);
            std::strcpy(m_quoteExchgID, q->ExchangeID);
        }
        PLOG("OnRtnQuote",
             "code=%s QuoteRef=%s SysID=%s Status=%c(%s) StatusMsg='%s' "
             "BID %d@%.2f BidOrdSysID=%s Ask %d@%.2f AskOrdSysID=%s "
             "Front=%d Session=%d InsTime=%s CnclTime=%s",
             q->InstrumentID, q->QuoteRef, q->QuoteSysID,
             q->QuoteStatus, quoteStatusName(q->QuoteStatus),
             q->StatusMsg,
             q->BidVolume, q->BidPrice, q->BidOrderSysID,
             q->AskVolume, q->AskPrice, q->AskOrderSysID,
             q->FrontID, q->SessionID, q->InsertTime, q->CancelTime);
    }

    void OnRspQuoteAction(CThostFtdcInputQuoteActionField* rsp,
                          CThostFtdcRspInfoField* info,
                          int rid, bool isLast) override {
        if (info && info->ErrorID != 0) {
            PLOG("OnRspQuoteAction", "REJECTED id=%d msg=%s",
                 info->ErrorID, info->ErrorMsg);
        } else {
            PLOG("OnRspQuoteAction", "OK rid=%d isLast=%d", rid, isLast);
        }
    }

    void OnErrRtnQuoteAction(CThostFtdcQuoteActionField* rsp,
                             CThostFtdcRspInfoField* info) override {
        PLOG("OnErrRtnQuoteAction", "id=%d msg=%s",
             info ? info->ErrorID : 0, info ? info->ErrorMsg : "?");
    }

    // ---------- 关联订单回报 (quote 拆出来的 bid/ask order) ----------
    void OnRtnOrder(CThostFtdcOrderField* o) override {
        if (!o) return;
        // SubmitStatus 是 "前置/交易所是否接受订单" 的关键字段
        // OrderSubmitStatus: '0'已提交 '1'撤单已提交 '2'修改已提交 '3'已接受 '4'报单已拒绝 '5'撤单已拒绝
        // StatusMsg / GTDDate / SequenceNo: 交易所或前置带回的拒绝原因
        PLOG("OnRtnOrder",
             "code=%s OrdRef=%s SysID=%s ExchOrdID=%s Dir=%c "
             "Status=%c(%s) SubmitStatus=%c StatusMsg='%s' "
             "Price=%.2f Vol=%d Traded=%d Remain=%d "
             "Source=%c Type=%c Front=%d Sess=%d InsTime=%s CnclTime=%s",
             o->InstrumentID, o->OrderRef, o->OrderSysID,
             o->ExchangeID,
             o->Direction,
             o->OrderStatus, orderStatusName(o->OrderStatus),
             o->OrderSubmitStatus,
             o->StatusMsg,
             o->LimitPrice, o->VolumeTotalOriginal, o->VolumeTraded, o->VolumeTotal,
             o->OrderSource, o->OrderType,
             o->FrontID, o->SessionID,
             o->InsertTime, o->CancelTime);
    }

    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* o,
                             CThostFtdcRspInfoField* info) override {
        PLOG("OnErrRtnOrderInsert",
             "id=%d msg='%s' code=%s OrdRef=%s Price=%.2f Vol=%d",
             info ? info->ErrorID : 0,
             info ? info->ErrorMsg : "?",
             o ? o->InstrumentID : "?",
             o ? o->OrderRef : "?",
             o ? o->LimitPrice : 0.0,
             o ? o->VolumeTotalOriginal : 0);
    }

    void OnRtnTrade(CThostFtdcTradeField* t) override {
        if (!t) return;
        PLOG("OnRtnTrade",
             "code=%s OrdRef=%s OrdSysID=%s TradeID=%s Dir=%c Price=%.2f Vol=%d "
             "OffsetFlag=%c TradeTime=%s TradeSource=%c",
             t->InstrumentID, t->OrderRef, t->OrderSysID, t->TradeID,
             t->Direction, t->Price, t->Volume,
             t->OffsetFlag, t->TradeTime, t->TradeSource);
    }

    void OnRspError(CThostFtdcRspInfoField* info, int rid, bool isLast) override {
        PLOG("OnRspError", "id=%d msg=%s rid=%d",
             info ? info->ErrorID : 0, info ? info->ErrorMsg : "?", rid);
    }

    static const char* quoteStatusName(char s) {
        switch (s) {
            case '0': return "AllTraded";
            case '1': return "PartTradedQueueing";
            case '3': return "Unknown";
            case '4': return "NotTouched";
            case '5': return "Canceled";
            default:  return "?";
        }
    }
    static const char* orderStatusName(char s) {
        switch (s) {
            case '0': return "AllTraded";
            case '1': return "PartTradedQueueing";
            case '2': return "PartTradedNotQueueing";
            case '3': return "NoTradeQueueing";
            case '4': return "NoTradeNotQueueing";
            case '5': return "Canceled";
            case 'a': return "Unknown";
            default:  return "?";
        }
    }
};

// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
    if (argc >= 2) g_cfg.instrument = argv[1];
    if (argc >= 3) g_cfg.exchange   = argv[2];
    if (argc >= 4) g_cfg.tickOffset = std::atoi(argv[3]);
    // 实验: 命令行覆盖 appid/authcode/password 以定位认证失败根因
    if (argc >= 5) g_cfg.appid    = argv[4];
    if (argc >= 6) g_cfg.authcode = argv[5];
    if (argc >= 7) g_cfg.pass     = argv[6];
    if (const char* q = std::getenv("QP_QTY"))  g_cfg.qty = std::max(1, std::atoi(q));
    if (const char* e = std::getenv("QP_PRICE")) g_cfg.lastPrice = std::atof(e);
    if (const char* e = std::getenv("QP_TICK"))  g_cfg.priceTick = std::atof(e);
    // env 覆盖 front / broker / user，便于不修改源码切换前置
    if (const char* e = std::getenv("QP_FRONT"))  g_cfg.front  = e;
    if (const char* e = std::getenv("QP_BROKER")) g_cfg.broker = e;
    if (const char* e = std::getenv("QP_USER"))   g_cfg.user   = e;

    // 日志文件
    char fname[64];
    auto t = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    std::snprintf(fname, sizeof(fname), "quote_probe_%04d%02d%02d_%02d%02d%02d.log",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    g_logFile.open(fname);

    PLOG("main", "=== QuoteProbe start ===");
    PLOG("main", "front=%s broker=%s user=%s",
         g_cfg.front.c_str(), g_cfg.broker.c_str(), g_cfg.user.c_str());
    PLOG("main", "instrument=%s exchange=%s tickOffset=%d qty=%d",
         g_cfg.instrument.c_str(), g_cfg.exchange.c_str(),
         g_cfg.tickOffset, g_cfg.qty);
    {
        std::string ac_tail = g_cfg.authcode.size() >= 4
            ? g_cfg.authcode.substr(g_cfg.authcode.size()-4)
            : g_cfg.authcode;
        PLOG("main", "appid=%s authcode_tail=...%s pass_len=%zu",
             g_cfg.appid.c_str(), ac_tail.c_str(), g_cfg.pass.size());
    }
    PLOG("main", "log=%s", fname);

    // 创建 flow 目录
    std::system("mkdir -p ./QuoteProbeFlow");

    CThostFtdcTraderApi* api = CThostFtdcTraderApi::CreateFtdcTraderApi("./QuoteProbeFlow/");
    QuoteProbeSpi spi;
    spi.m_api = api;

    api->RegisterSpi(&spi);
    api->SubscribePrivateTopic(THOST_TERT_QUICK);
    api->SubscribePublicTopic(THOST_TERT_QUICK);
    api->RegisterFront(const_cast<char*>(g_cfg.front.c_str()));
    api->Init();

    // 等待完成 (最长 90s)
    int waited = 0;
    while (!spi.m_done && waited < 90) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++waited;
    }
    if (!spi.m_done) PLOG("main", "TIMEOUT after %ds, forced exit", waited);

    PLOG("main", "=== QuoteProbe end, log=%s ===", fname);
    api->RegisterSpi(nullptr);
    // api->Release();  // CTP 6.3.15 释放有时挂起, 直接 exit
    g_logFile.close();
    std::_Exit(0);
}
