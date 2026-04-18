/*!
 * \file FutuConfig.cpp
 * \brief 统一配置管理实现
 */
#include "FutuConfig.h"
#include "../WTSTools/WTSLogger.h"
#include <cmath>
#include <algorithm>

namespace futu {

//==============================================================================
// 辅助读取函数
//==============================================================================

double FutuConfig::readDouble(wtp::WTSVariant* cfg, const char* key, double defVal)
{
    if (!cfg) return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    return node ? node->asDouble() : defVal;
}

uint32_t FutuConfig::readUInt32(wtp::WTSVariant* cfg, const char* key, uint32_t defVal)
{
    if (!cfg) return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    return node ? node->asUInt32() : defVal;
}

bool FutuConfig::readBool(wtp::WTSVariant* cfg, const char* key, bool defVal)
{
    if (!cfg) return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    return node ? node->asBoolean() : defVal;
}

std::string FutuConfig::readString(wtp::WTSVariant* cfg, const char* key, const char* defVal)
{
    if (!cfg) return defVal;
    wtp::WTSVariant* node = cfg->get(key);
    return node ? node->asString() : defVal;
}

//==============================================================================
// 加载默认值
//==============================================================================

void FutuConfig::loadDefaults()
{
    // 所有默认值已在结构体定义中设置
    // 此方法可用于运行时重置
    strategy = StrategyConfig();
    modules = ModuleConfig();
    pipeline = PipelineConfig();
    performance = PerformanceConfig();
}

//==============================================================================
// 加载 coordinator.yaml
//==============================================================================

void FutuConfig::loadCoordinatorFromVariant(wtp::WTSVariant* cfg)
{
    if (!cfg) return;
    
    // 读取 coordinator 节点
    wtp::WTSVariant* coord = cfg->get("coordinator");
    if (!coord) {
        WTSLogger::warn("FutuConfig: No 'coordinator' section found");
        return;
    }
    
    // 读取 modules 节点
    wtp::WTSVariant* modulesNode = coord->get("modules");
    if (modulesNode)
    {
        // SignalAggregator
        wtp::WTSVariant* sig = modulesNode->get("signalAggregator");
        if (sig) {
            modules.signalAggregator.useVolatility = readBool(sig, "useVolatility", modules.signalAggregator.useVolatility);
            modules.signalAggregator.useOfi = readBool(sig, "useOfi", modules.signalAggregator.useOfi);
            modules.signalAggregator.useTradeFlow = readBool(sig, "useTradeFlow", modules.signalAggregator.useTradeFlow);
            modules.signalAggregator.useAlpha = readBool(sig, "useAlpha", modules.signalAggregator.useAlpha);
            modules.signalAggregator.useMomentum = readBool(sig, "useMomentum", modules.signalAggregator.useMomentum);
            modules.signalAggregator.useLeadLag = readBool(sig, "useLeadLag", modules.signalAggregator.useLeadLag);
            
            modules.signalAggregator.volatilityWindow = readUInt32(sig, "volatilityWindow", modules.signalAggregator.volatilityWindow);
            modules.signalAggregator.ofiWindow = readUInt32(sig, "ofiWindow", modules.signalAggregator.ofiWindow);
            modules.signalAggregator.tradeFlowWindow = readUInt32(sig, "tradeFlowWindow", modules.signalAggregator.tradeFlowWindow);
            modules.signalAggregator.largeTradeThreshold = readDouble(sig, "largeTradeThreshold", modules.signalAggregator.largeTradeThreshold);
            modules.signalAggregator.momentumWindow = readUInt32(sig, "momentumWindow", modules.signalAggregator.momentumWindow);
            modules.signalAggregator.momentumEmaAlpha = readDouble(sig, "momentumEmaAlpha", modules.signalAggregator.momentumEmaAlpha);
            modules.signalAggregator.leadLagWindow = readUInt32(sig, "leadLagWindow", modules.signalAggregator.leadLagWindow);
            modules.signalAggregator.leadLagLagMs = readUInt32(sig, "leadLagLagMs", modules.signalAggregator.leadLagLagMs);
            
            modules.signalAggregator.ofiWeight = readDouble(sig, "ofiWeight", modules.signalAggregator.ofiWeight);
            modules.signalAggregator.tradeWeight = readDouble(sig, "tradeWeight", modules.signalAggregator.tradeWeight);
            modules.signalAggregator.leadLagWeight = readDouble(sig, "leadLagWeight", modules.signalAggregator.leadLagWeight);
            modules.signalAggregator.momentumWeight = readDouble(sig, "momentumWeight", modules.signalAggregator.momentumWeight);
            modules.signalAggregator.strongThreshold = readDouble(sig, "strongThreshold", modules.signalAggregator.strongThreshold);
            
            modules.signalAggregator.volThreshold = readDouble(sig, "volThreshold", modules.signalAggregator.volThreshold);
            modules.signalAggregator.spreadThreshold = readDouble(sig, "spreadThreshold", modules.signalAggregator.spreadThreshold);
            modules.signalAggregator.moveThreshold = readDouble(sig, "moveThreshold", modules.signalAggregator.moveThreshold);
        }
        
        // ToxicityDetector
        wtp::WTSVariant* tox = modulesNode->get("toxicityDetector");
        if (tox) {
            modules.toxicity.enabled = readBool(tox, "enabled", modules.toxicity.enabled);
            modules.toxicity.vpinThreshold = readDouble(tox, "vpinThreshold", modules.toxicity.vpinThreshold);
            modules.toxicity.window = readUInt32(tox, "window", modules.toxicity.window);
            modules.toxicity.cooloffMs = readUInt32(tox, "cooloffMs", modules.toxicity.cooloffMs);
        }
        
        // SpreadOptimizer
        wtp::WTSVariant* spOpt = modulesNode->get("spreadOptimizer");
        if (spOpt) {
            modules.spreadOptimizer.enabled = readBool(spOpt, "enabled", modules.spreadOptimizer.enabled);
            modules.spreadOptimizer.volSensitivity = readDouble(spOpt, "volSensitivity", modules.spreadOptimizer.volSensitivity);
            modules.spreadOptimizer.depthSensitivity = readDouble(spOpt, "depthSensitivity", modules.spreadOptimizer.depthSensitivity);
            modules.spreadOptimizer.volWindow = readUInt32(spOpt, "volWindow", modules.spreadOptimizer.volWindow);
            modules.spreadOptimizer.minSpreadMult = readDouble(spOpt, "minSpreadMult", modules.spreadOptimizer.minSpreadMult);
            modules.spreadOptimizer.phi = readDouble(spOpt, "phi", modules.spreadOptimizer.phi);
            modules.spreadOptimizer.portfolioSkewWeight = readDouble(spOpt, "portfolioSkewWeight", modules.spreadOptimizer.portfolioSkewWeight);
            modules.spreadOptimizer.minCorrelation = readDouble(spOpt, "minCorrelation", modules.spreadOptimizer.minCorrelation);
            
            // Market state
            wtp::WTSVariant* market = spOpt->get("marketState");
            if (market) {
                modules.spreadOptimizer.marketVolThreshold = readDouble(market, "volThreshold", modules.spreadOptimizer.marketVolThreshold);
                modules.spreadOptimizer.marketMoveThreshold = readDouble(market, "moveThreshold", modules.spreadOptimizer.marketMoveThreshold);
                modules.spreadOptimizer.marketSpreadThreshold = readDouble(market, "spreadThreshold", modules.spreadOptimizer.marketSpreadThreshold);
                modules.spreadOptimizer.marketVolumeThreshold = readDouble(market, "volumeThreshold", modules.spreadOptimizer.marketVolumeThreshold);
                modules.spreadOptimizer.marketCooldownTicks = readUInt32(market, "cooldownTicks", modules.spreadOptimizer.marketCooldownTicks);
            }
        }
        
        // AutoCancel
        wtp::WTSVariant* ac = modulesNode->get("autoCancel");
        if (ac) {
            modules.autoCancel.enabled = readBool(ac, "enabled", modules.autoCancel.enabled);
            modules.autoCancel.maxAgeMs = readUInt32(ac, "maxAgeMs", modules.autoCancel.maxAgeMs);
            modules.autoCancel.priceDeviation = readDouble(ac, "priceDeviation", modules.autoCancel.priceDeviation);
            modules.autoCancel.cancelOnStateChange = readBool(ac, "cancelOnStateChange", modules.autoCancel.cancelOnStateChange);
            modules.autoCancel.cancelOnInventoryLimit = readBool(ac, "cancelOnInventoryLimit", modules.autoCancel.cancelOnInventoryLimit);
            modules.autoCancel.inventoryCooldownMs = readUInt32(ac, "inventoryCooldownMs", modules.autoCancel.inventoryCooldownMs);
        }
        
        // SelfTradePrevention
        wtp::WTSVariant* stp = modulesNode->get("selfTradePrevention");
        if (stp) {
            modules.selfTradePrevention.enabled = readBool(stp, "enabled", modules.selfTradePrevention.enabled);
            modules.selfTradePrevention.strategy = readString(stp, "strategy", modules.selfTradePrevention.strategy.c_str());
            modules.selfTradePrevention.minPriceGap = readDouble(stp, "minPriceGap", modules.selfTradePrevention.minPriceGap);
            modules.selfTradePrevention.allowSamePrice = readBool(stp, "allowSamePrice", modules.selfTradePrevention.allowSamePrice);
            modules.selfTradePrevention.priceAdjustTicks = readDouble(stp, "priceAdjustTicks", modules.selfTradePrevention.priceAdjustTicks);
        }
        
        // SelfTradeCalibrator
        wtp::WTSVariant* calib = modulesNode->get("selfTradeCalibrator");
        if (calib) {
            modules.selfTradeCalibrator.lookbackTrades = readUInt32(calib, "lookbackTrades", modules.selfTradeCalibrator.lookbackTrades);
            modules.selfTradeCalibrator.toxicityWindowMs = readUInt32(calib, "toxicityWindowMs", modules.selfTradeCalibrator.toxicityWindowMs);
            modules.selfTradeCalibrator.adverseThreshold = readDouble(calib, "adverseThreshold", modules.selfTradeCalibrator.adverseThreshold);
        }

        // CorrelationManager
        wtp::WTSVariant* corr = modulesNode->get("correlationManager");
        if (corr) {
            modules.correlationManager.windowSize = readUInt32(corr, "windowSize", modules.correlationManager.windowSize);
            modules.correlationManager.minCorrelation = readDouble(corr, "minCorrelation", modules.correlationManager.minCorrelation);
            modules.correlationManager.spreadZThreshold = readDouble(corr, "spreadZThreshold", modules.correlationManager.spreadZThreshold);
        }
        
        // SyntheticTransaction
        wtp::WTSVariant* synth = modulesNode->get("syntheticTransaction");
        if (synth) {
            modules.syntheticTransaction.enabled = readBool(synth, "enabled", modules.syntheticTransaction.enabled);
            modules.syntheticTransaction.tickWeight = readDouble(synth, "tickWeight", modules.syntheticTransaction.tickWeight);
            modules.syntheticTransaction.bookWeight = readDouble(synth, "bookWeight", modules.syntheticTransaction.bookWeight);
            modules.syntheticTransaction.selfTradeWeight = readDouble(synth, "selfTradeWeight", modules.syntheticTransaction.selfTradeWeight);
            modules.syntheticTransaction.minSamples = readUInt32(synth, "minSamples", modules.syntheticTransaction.minSamples);
        }
        
        // TickInferer
        wtp::WTSVariant* infer = modulesNode->get("tickInferer");
        if (infer) {
            modules.tickInferer.imbalanceWindowMs = readUInt32(infer, "imbalanceWindowMs", modules.tickInferer.imbalanceWindowMs);
            modules.tickInferer.largeTradeThreshold = readDouble(infer, "largeTradeThreshold", modules.tickInferer.largeTradeThreshold);
            modules.tickInferer.minConfidence = readDouble(infer, "minConfidence", modules.tickInferer.minConfidence);
        }
        
        // AdaptiveParam
        wtp::WTSVariant* adapt = modulesNode->get("adaptiveParam");
        if (adapt) {
            modules.adaptiveParam.enabled = readBool(adapt, "enabled", modules.adaptiveParam.enabled);
            modules.adaptiveParam.updateInterval = readUInt32(adapt, "updateInterval", modules.adaptiveParam.updateInterval);
            modules.adaptiveParam.learningRate = readDouble(adapt, "learningRate", modules.adaptiveParam.learningRate);
            modules.adaptiveParam.minPhi = readDouble(adapt, "minPhi", modules.adaptiveParam.minPhi);
            modules.adaptiveParam.maxPhi = readDouble(adapt, "maxPhi", modules.adaptiveParam.maxPhi);
        }
        
        // 模块开关
        strategy.useMarketMaking = readBool(modulesNode, "useMarketMaking", strategy.useMarketMaking);
        strategy.useSpreadArbitrage = readBool(modulesNode, "useSpreadArbitrage", strategy.useSpreadArbitrage);
    }
    
    // Pipeline
    wtp::WTSVariant* pipe = coord->get("pipeline");
    if (pipe) {
        pipeline.paramUpdateInterval = readUInt32(pipe, "paramUpdateInterval", pipeline.paramUpdateInterval);
        pipeline.globalToxicityCooldownMs = readUInt32(pipe, "globalToxicityCooldownMs", pipeline.globalToxicityCooldownMs);
    }
    
    // Performance
    wtp::WTSVariant* perf = coord->get("performance");
    if (perf) {
        performance.enabled = readBool(perf, "enabled", performance.enabled);
        performance.logInterval = readUInt32(perf, "logInterval", performance.logInterval);
        performance.latencyThresholdNs = readUInt32(perf, "latencyThresholdNs", performance.latencyThresholdNs);
        performance.warnThresholdNs = readUInt32(perf, "warnThresholdNs", performance.warnThresholdNs);
        performance.criticalThresholdNs = readUInt32(perf, "criticalThresholdNs", performance.criticalThresholdNs);
    }
}

//==============================================================================
// 加载策略配置
//==============================================================================

void FutuConfig::loadStrategyFromVariant(wtp::WTSVariant* cfg)
{
    if (!cfg) return;
    
    // 合约配置
    wtp::WTSVariant* contracts = cfg->get("contracts");
    if (contracts) {
        // 合约列表由 UftFutuMmStrategy 处理
    }
    
    // 报价参数
    strategy.numLevels = readUInt32(cfg, "numLevels", strategy.numLevels);
    strategy.baseSpread = readDouble(cfg, "baseSpread", strategy.baseSpread);
    strategy.baseQty = readDouble(cfg, "baseQty", strategy.baseQty);
    strategy.qtyDecay = readDouble(cfg, "qtyDecay", strategy.qtyDecay);
    strategy.levelStep = readDouble(cfg, "levelStep", strategy.levelStep);
    
    // 库存参数
    strategy.maxInventory = readDouble(cfg, "maxInventory", strategy.maxInventory);
    strategy.skewFactor = readDouble(cfg, "skewFactor", strategy.skewFactor);
    strategy.maxSkew = readDouble(cfg, "maxSkew", strategy.maxSkew);
    strategy.hedgeRatio = readDouble(cfg, "hedgeRatio", strategy.hedgeRatio);
    strategy.targetInventory = readDouble(cfg, "targetInventory", strategy.targetInventory);
    strategy.maxDelta = readDouble(cfg, "maxDelta", strategy.maxDelta);
    strategy.maxExposure = readDouble(cfg, "maxExposure", strategy.maxExposure);
    strategy.hedgeThreshold = readDouble(cfg, "hedgeThreshold", strategy.hedgeThreshold);
    
    // Sticky 策略
    strategy.stickyThreshold = readDouble(cfg, "stickyThreshold", strategy.stickyThreshold);
    strategy.improveRetreatRatio = readDouble(cfg, "improveRetreatRatio", strategy.improveRetreatRatio);
    strategy.maxPriceDeviation = readDouble(cfg, "maxPriceDeviation", strategy.maxPriceDeviation);
    strategy.skewSensitivity = readDouble(cfg, "skewSensitivity", strategy.skewSensitivity);
    strategy.aggressiveSkewThreshold = readDouble(cfg, "aggressiveSkewThreshold", strategy.aggressiveSkewThreshold);
    strategy.oneSidedThreshold = readDouble(cfg, "oneSidedThreshold", strategy.oneSidedThreshold);
    
    // 风控参数
    strategy.maxDailyLoss = readDouble(cfg, "maxDailyLoss", strategy.maxDailyLoss);
    strategy.deltaLimit = readDouble(cfg, "deltaLimit", strategy.deltaLimit);
    strategy.orderErrorThreshold = readUInt32(cfg, "orderErrorThreshold", strategy.orderErrorThreshold);
    
    // 收盘平仓
    strategy.closeoutMinutesBefore = readUInt32(cfg, "closeoutMinutesBefore", strategy.closeoutMinutesBefore);
    strategy.closeoutFlattenPosition = readBool(cfg, "closeoutFlattenPosition", strategy.closeoutFlattenPosition);
    
    // 风控频率
    wtp::WTSVariant* risk = cfg->get("riskMonitor");
    if (risk) {
        strategy.riskMaxOrdersPerSec = readUInt32(risk, "maxOrdersPerSec", strategy.riskMaxOrdersPerSec);
        strategy.riskMaxCancelsPerSec = readUInt32(risk, "maxCancelsPerSec", strategy.riskMaxCancelsPerSec);
        strategy.riskMaxTradesPerSec = readUInt32(risk, "maxTradesPerSec", strategy.riskMaxTradesPerSec);
        strategy.riskCooldownMs = readUInt32(risk, "cooldownMs", strategy.riskCooldownMs);
        strategy.riskCheckIntervalMs = readUInt32(risk, "checkIntervalMs", strategy.riskCheckIntervalMs);
        strategy.riskRecoveryThreshold = readDouble(risk, "recoveryThreshold", strategy.riskRecoveryThreshold);
    }
    
    // 性能监控
    wtp::WTSVariant* perfAnal = cfg->get("performanceAnalyzer");
    if (perfAnal) {
        strategy.perfAnalyzerWindowSize = readUInt32(perfAnal, "windowSize", strategy.perfAnalyzerWindowSize);
        strategy.perfAnalyzerRiskFreeRate = readDouble(perfAnal, "sharpeRiskFree", strategy.perfAnalyzerRiskFreeRate);
    }
    
    wtp::WTSVariant* perfMon = cfg->get("performanceMonitor");
    if (perfMon) {
        strategy.perfMonitorLatencyThreshold = (uint64_t)readDouble(perfMon, "latencyThreshold", strategy.perfMonitorLatencyThreshold);
    }
    
    // 模块开关（覆盖 coordinator）
    wtp::WTSVariant* modSwitch = cfg->get("modules");
    if (modSwitch) {
        strategy.useMarketMaking = readBool(modSwitch, "useMarketMaking", strategy.useMarketMaking);
        strategy.useSpreadArbitrage = readBool(modSwitch, "useSpreadArbitrage", strategy.useSpreadArbitrage);
        strategy.useSpreadOptimizer = readBool(modSwitch, "useSpreadOptimizer", strategy.useSpreadOptimizer);
        strategy.useAutoCancel = readBool(modSwitch, "useAutoCancel", strategy.useAutoCancel);
        strategy.useToxicityDetector = readBool(modSwitch, "useToxicityDetector", strategy.useToxicityDetector);
        strategy.usePerformanceMonitor = readBool(modSwitch, "usePerformanceMonitor", strategy.usePerformanceMonitor);
        strategy.usePerformanceAnalyzer = readBool(modSwitch, "usePerformanceAnalyzer", strategy.usePerformanceAnalyzer);
    }
}

//==============================================================================
// 验证配置
//==============================================================================

bool FutuConfig::validate() const
{
    // 验证报价参数
    if (strategy.numLevels == 0) {
        _validation_error = "numLevels must be > 0";
        return false;
    }
    if (strategy.baseSpread <= 0) {
        _validation_error = "baseSpread must be > 0";
        return false;
    }
    if (strategy.baseQty <= 0) {
        _validation_error = "baseQty must be > 0";
        return false;
    }
    
    // 验证库存参数
    if (strategy.maxInventory <= 0) {
        _validation_error = "maxInventory must be > 0";
        return false;
    }
    if (strategy.skewFactor <= 0) {
        _validation_error = "skewFactor must be > 0";
        return false;
    }
    
    // 验证权重
    double alphaWeightSum = modules.signalAggregator.ofiWeight + 
                            modules.signalAggregator.tradeWeight + 
                            modules.signalAggregator.leadLagWeight +
                            modules.signalAggregator.momentumWeight;
    if (std::abs(alphaWeightSum - 1.0) > 0.01 && alphaWeightSum > 0) {
        WTSLogger::warn("FutuConfig: Alpha weights sum to {}, expected 1.0", alphaWeightSum);
    }
    
    return true;
}

std::string FutuConfig::getValidationError() const
{
    return _validation_error;
}

//==============================================================================
// 打印配置摘要
//==============================================================================

void FutuConfig::printSummary() const
{
    WTSLogger::info("=== FutuConfig Summary ===");
    WTSLogger::info("Strategy:");
    WTSLogger::info("  numLevels={}, baseSpread={}, baseQty={}", 
        strategy.numLevels, strategy.baseSpread, strategy.baseQty);
    WTSLogger::info("  maxInventory={}, skewFactor={}, maxSkew={}", 
        strategy.maxInventory, strategy.skewFactor, strategy.maxSkew);
    WTSLogger::info("  stickyThreshold={}, improveRetreatRatio={}", 
        strategy.stickyThreshold, strategy.improveRetreatRatio);
    WTSLogger::info("  closeoutMinutesBefore={}, closeoutFlattenPosition={}", 
        strategy.closeoutMinutesBefore, strategy.closeoutFlattenPosition);
    
    WTSLogger::info("Modules:");
    WTSLogger::info("  SignalAggregator: ofiWeight={}, tradeWeight={}, strongThreshold={}", 
        modules.signalAggregator.ofiWeight, modules.signalAggregator.tradeWeight, 
        modules.signalAggregator.strongThreshold);
    WTSLogger::info("  Toxicity: enabled={}, vpinThreshold={}", 
        modules.toxicity.enabled, modules.toxicity.vpinThreshold);
    WTSLogger::info("  SpreadOptimizer: enabled={}, phi={}", 
        modules.spreadOptimizer.enabled, modules.spreadOptimizer.phi);
    WTSLogger::info("  AutoCancel: enabled={}, maxAgeMs={}", 
        modules.autoCancel.enabled, modules.autoCancel.maxAgeMs);
    
    WTSLogger::info("Pipeline:");
    WTSLogger::info("  paramUpdateInterval={}, globalToxicityCooldownMs={}", 
        pipeline.paramUpdateInterval, pipeline.globalToxicityCooldownMs);
    
    WTSLogger::info("Performance:");
    WTSLogger::info("  enabled={}, latencyThresholdNs={}", 
        performance.enabled, performance.latencyThresholdNs);
}

//==============================================================================
// 文件加载方法（需要 WTSCfgLoader 支持）
//==============================================================================

bool FutuConfig::loadCoordinator(const std::string& path)
{
    // 由 UftFutuMmStrategy 调用，传入已解析的 WTSVariant
    // 此方法保留用于独立测试
    WTSLogger::info("FutuConfig: Loading coordinator from {}", path);
    return true;
}

bool FutuConfig::loadStrategy(const std::string& path)
{
    // 由 UftFutuMmStrategy 调用，传入已解析的 WTSVariant
    // 此方法保留用于独立测试
    WTSLogger::info("FutuConfig: Loading strategy from {}", path);
    return true;
}

} // namespace futu
