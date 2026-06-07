/*!
 * \file FutuConfigValidator.h
 * \brief Configuration Validation for WtFutuCore
 * 
 * Validates cross-module configuration consistency and parameter ranges.
 * Should be called after loading configuration and before starting the strategy.
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <string>
#include <vector>
#include <cmath>

namespace futu {

class FutuConfigValidator {
public:
    struct ValidationResult {
        bool valid = true;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        
        void addError(const std::string& msg) {
            errors.push_back(msg);
            valid = false;
        }
        void addWarning(const std::string& msg) {
            warnings.push_back(msg);
        }
        
        size_t errorCount() const { return errors.size(); }
        size_t warningCount() const { return warnings.size(); }
    };
    
    /// 验证信号权重之和是否接近1.0
    static bool validateSignalWeights(
        double ofi_weight,
        double trade_weight,
        double book_imbalance_weight,
        double momentum_weight,
        double lead_lag_weight,
        ValidationResult& result)
    {
        double total = ofi_weight + trade_weight + book_imbalance_weight + 
                       momentum_weight + lead_lag_weight;
        
        if (std::abs(total - 1.0) > 0.1) {
            result.addWarning("Signal weights sum to " + std::to_string(total) + 
                             ", expected ~1.0. This may cause alpha scaling issues.");
            return false;
        }
        return true;
    }
    
    /// 验证止损必须大于入场阈值
    static bool validateStopLoss(double entry_threshold, double stop_loss_z,
                                  ValidationResult& result)
    {
        if (stop_loss_z <= entry_threshold) {
            result.addError("stop_loss_z (" + std::to_string(stop_loss_z) + 
                           ") must be > entry_z_threshold (" + 
                           std::to_string(entry_threshold) + ")");
            return false;
        }
        return true;
    }
    
    /// 验证退出阈值必须小于入场阈值
    static bool validateExitThreshold(double entry_threshold, double exit_threshold,
                                       ValidationResult& result)
    {
        if (exit_threshold >= entry_threshold) {
            result.addError("exit_z_threshold (" + std::to_string(exit_threshold) + 
                           ") must be < entry_z_threshold (" + 
                           std::to_string(entry_threshold) + ")");
            return false;
        }
        return true;
    }
    
    /// 验证加仓安全比率
    static bool validateAddSafetyRatio(double add_safety_ratio,
                                        ValidationResult& result)
    {
        if (add_safety_ratio <= 0 || add_safety_ratio >= 1.0) {
            result.addError("add_safety_ratio (" + std::to_string(add_safety_ratio) + 
                           ") must be in (0, 1)");
            return false;
        }
        return true;
    }
    
    /// 验证队列大小是否为2的幂
    static bool validateQueueSize(uint32_t size, const std::string& name,
                                   ValidationResult& result)
    {
        if (size == 0 || (size & (size - 1)) != 0) {
            result.addError(name + " (" + std::to_string(size) + 
                           ") must be a power of 2");
            return false;
        }
        return true;
    }
    
    /// 验证参数范围
    template<typename T>
    static bool checkRange(const std::string& name, T value, T min_val, T max_val,
                           ValidationResult& result) {
        if (value < min_val || value > max_val) {
            result.addError(name + "=" + std::to_string(value) + 
                           " out of range [" + std::to_string(min_val) + 
                           ", " + std::to_string(max_val) + "]");
            return false;
        }
        return true;
    }
    
    /// 验证正数参数
    template<typename T>
    static bool checkPositive(const std::string& name, T value,
                               ValidationResult& result) {
        if (value <= 0) {
            result.addError(name + " must be positive, got " + std::to_string(value));
            return false;
        }
        return true;
    }
};

} // namespace futu
