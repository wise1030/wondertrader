/*!
 * \file FutuConfig.h
 * \brief 统一配置管理
 * 
 * 配置加载顺序（优先级从低到高）：
 *   1. 默认值（代码定义）
 *   2. coordinator.yaml（模块参数）
 *   3. config.yaml（策略参数，覆盖 coordinator）
 * 
 * 命名规范：统一使用 camelCase
 */
#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include "../Includes/WTSVariant.hpp"

namespace futu {

//==============================================================================
// 策略配置（从 config.yaml 加载，优先级最高）
//==============================================================================
struct StrategyConfig
{
    // 合约信息
    struct ContractInfo
    {
        std::string code;
        double multiplier = 1.0;
        double tickSize = 0.0;
        double maxPosition = 0.0;
        double maxDelta = 0.0;
        double targetPosition = 0.0;
    };
    
    std::string anchorCode;
    std::vector<ContractInfo> contracts;
    
    // 报价参数
    uint32_t numLevels = 1;         // 报价档位
    double baseSpread = 3.0;        // 基础价差 (tick)
    double baseQty = 3.0;           // 基础数量
    double qtyDecay = 0.7;          // 数量衰减
    double levelStep = 1.0;         // 档位步长
    
    // 库存参数
    double maxInventory = 100.0;    // 最大库存
    double skewFactor = 0.01;       // 库存倾斜因子
    double maxSkew = 5.0;           // 最大倾斜值
    double hedgeRatio = 1.0;        // 对冲比例
    double targetInventory = 0.0;   // 目标库存
    double maxDelta = 20000000.0;   // 最大组合Delta
    double maxExposure = 50000000.0;// 最大暴露
    double hedgeThreshold = 8000000.0; // 对冲阈值
    
    // Sticky 策略
    double stickyThreshold = 1.0;   // 价格粘性阈值 (tick)
    double improveRetreatRatio = 2.0; // 改善/撤退容忍比
    double maxPriceDeviation = 20.0; // 最大价格偏离
    double skewSensitivity = 2.0;   // Skew 灵敏度
    double aggressiveSkewThreshold = 0.5;
    double oneSidedThreshold = 0.8;
    
    // 风控参数
    double maxDailyLoss = -200000.0;
    double deltaLimit = 15000000.0;
    uint32_t orderErrorThreshold = 3;
    
    // 收盘平仓
    uint32_t closeoutMinutesBefore = 5;
    bool closeoutFlattenPosition = true;
    uint32_t closeTime = 150000;
    
    // 风控频率
    uint32_t riskMaxOrdersPerSec = 50;
    uint32_t riskMaxCancelsPerSec = 30;
    uint32_t riskMaxTradesPerSec = 20;
    uint32_t riskCooldownMs = 30000;
    uint32_t riskCheckIntervalMs = 5000;
    double riskRecoveryThreshold = 0.8;
    
    // 性能监控
    uint32_t perfAnalyzerWindowSize = 1000;
    double perfAnalyzerRiskFreeRate = 0.0;
    uint64_t perfMonitorLatencyThreshold = 100000;
    
    // 模块开关（覆盖 coordinator.yaml）
    bool useMarketMaking = true;
    bool useSpreadArbitrage = false;
    bool useSpreadOptimizer = true;
    bool useAutoCancel = true;
    bool useToxicityDetector = true;
    bool usePerformanceMonitor = false;
    bool usePerformanceAnalyzer = false;
};

//==============================================================================
// 模块配置（从 coordinator.yaml 加载）
//==============================================================================
struct ModuleConfig
{
    // SignalAggregator 信号聚合器
    struct SignalAggregatorConfig
    {
        bool useVolatility = true;
        bool useOfi = true;
        bool useTradeFlow = true;
        bool useAlpha = true;
        bool useMomentum = true;
        bool useLeadLag = false;
        
        uint32_t volatilityWindow = 100;
        uint32_t ofiWindow = 50;
        uint32_t tradeFlowWindow = 100;
        double largeTradeThreshold = 50.0;
        uint32_t momentumWindow = 50;
        double momentumEmaAlpha = 0.1;
        uint32_t leadLagWindow = 50;
        uint32_t leadLagLagMs = 50;
        
        double ofiWeight = 0.4;
        double tradeWeight = 0.3;
        double leadLagWeight = 0.3;
        double momentumWeight = 0.0;
        double strongThreshold = 0.7;
        
        double volThreshold = 0.003;
        double spreadThreshold = 5.0;
        double moveThreshold = 0.005;
    } signalAggregator;
    
    // ToxicityDetector 毒性检测器
    struct ToxicityConfig
    {
        bool enabled = true;
        double vpinThreshold = 0.7;
        uint32_t window = 50;
        uint32_t cooloffMs = 5000;
    } toxicity;
    
    // SpreadOptimizer 价差优化器
    struct SpreadOptimizerConfig
    {
        bool enabled = true;
        double volSensitivity = 1.0;
        double depthSensitivity = 0.5;
        uint32_t volWindow = 100;
        double minSpreadMult = 0.5;
        double phi = 0.01;
        double portfolioSkewWeight = 0.5;
        double minCorrelation = 0.5;
        
        // 市场状态检测
        double marketVolThreshold = 0.003;
        double marketMoveThreshold = 0.005;
        double marketSpreadThreshold = 5.0;
        double marketVolumeThreshold = 10.0;
        uint32_t marketCooldownTicks = 20;
    } spreadOptimizer;
    
    // AutoCancel 自动撤单
    struct AutoCancelConfig
    {
        bool enabled = true;
        uint32_t maxAgeMs = 10000;
        double priceDeviation = 3.0;
        bool cancelOnStateChange = true;
        bool cancelOnInventoryLimit = true;
        uint32_t inventoryCooldownMs = 2000;
    } autoCancel;
    
    // SelfTradePrevention 自成交防护
    struct SelfTradePreventionConfig
    {
        bool enabled = true;
        std::string strategy = "cancelMM";
        double minPriceGap = 1.0;
        bool allowSamePrice = false;
        double priceAdjustTicks = 1.0;
    } selfTradePrevention;
    
    // SelfTradeCalibrator 自身成交校准
    struct SelfTradeCalibratorConfig
    {
        uint32_t lookbackTrades = 50;
        uint32_t toxicityWindowMs = 5000;
        double adverseThreshold = 0.6;
    } selfTradeCalibrator;

    // CorrelationManager 相关性管理
    struct CorrelationManagerConfig
    {
        uint32_t windowSize = 100;
        double minCorrelation = 0.5;
        double spreadZThreshold = 2.0;
    } correlationManager;
    
    // SyntheticTransaction 综合信号融合
    struct SyntheticTransactionConfig
    {
        bool enabled = true;
        double tickWeight = 0.4;
        double bookWeight = 0.4;
        double selfTradeWeight = 0.2;
        uint32_t minSamples = 5;
    } syntheticTransaction;
    
    // TickTransactionInferer
    struct TickInfererConfig
    {
        uint32_t imbalanceWindowMs = 5000;
        double largeTradeThreshold = 50.0;
        double minConfidence = 0.3;
    } tickInferer;
    
    // AdaptiveParam 自适应参数
    struct AdaptiveParamConfig
    {
        bool enabled = false;
        uint32_t updateInterval = 100;
        double learningRate = 0.01;
        double minPhi = 0.001;
        double maxPhi = 0.1;
    } adaptiveParam;
};

//==============================================================================
// Pipeline 配置
//==============================================================================
struct PipelineConfig
{
    uint32_t paramUpdateInterval = 100;
    uint32_t globalToxicityCooldownMs = 3000;
};

//==============================================================================
// Performance 配置
//==============================================================================
struct PerformanceConfig
{
    bool enabled = true;
    uint32_t logInterval = 1000;
    uint64_t latencyThresholdNs = 100000;
    uint64_t warnThresholdNs = 10000;
    uint64_t criticalThresholdNs = 50000;
};

//==============================================================================
// 统一配置类
//==============================================================================
class FutuConfig
{
public:
    StrategyConfig strategy;
    ModuleConfig modules;
    PipelineConfig pipeline;
    PerformanceConfig performance;
    
    //==========================================================================
    // 加载方法
    //==========================================================================
    
    /// 加载默认值
    void loadDefaults();
    
    /// 加载 coordinator.yaml
    bool loadCoordinator(const std::string& path);
    
    /// 加载 config.yaml（覆盖 coordinator）
    bool loadStrategy(const std::string& path);
    
    /// 从 WTSVariant 加载 coordinator 配置
    void loadCoordinatorFromVariant(wtp::WTSVariant* cfg);
    
    /// 从 WTSVariant 加载策略配置
    void loadStrategyFromVariant(wtp::WTSVariant* cfg);
    
    //==========================================================================
    // 验证方法
    //==========================================================================
    
    /// 验证配置有效性
    bool validate() const;
    
    /// 获取验证错误信息
    std::string getValidationError() const;
    
    //==========================================================================
    // 工具方法
    //==========================================================================
    
    /// 打印配置摘要
    void printSummary() const;
    
private:
    mutable std::string _validation_error;
    
    // 辅助读取函数
    static double readDouble(wtp::WTSVariant* cfg, const char* key, double defVal);
    static uint32_t readUInt32(wtp::WTSVariant* cfg, const char* key, uint32_t defVal);
    static bool readBool(wtp::WTSVariant* cfg, const char* key, bool defVal);
    static std::string readString(wtp::WTSVariant* cfg, const char* key, const char* defVal);
};

} // namespace futu
