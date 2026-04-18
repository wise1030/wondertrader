# WtFutuCore 配置优化方案

## 一、现有配置问题分析

### 1.1 当前配置结构

```
config/
├── config.yaml              # 主配置文件（包含所有参数，约200行）
├── profiles/
│   ├── conservative.yaml    # 保守模式
│   ├── standard.yaml        # 标准模式
│   └── aggressive.yaml      # 激进模式
└── strategy/
    └── quoting.yaml         # 报价参数配置
```

### 1.2 存在的问题

| 问题 | 说明 |
|------|------|
| 配置臃肿 | 单个配置文件包含所有模块参数，难以维护 |
| 参数重复 | 相似参数在多处配置（如 skewSensitivity 在顶层和子模块） |
| 热更新困难 | 所有参数混在一起，无法区分哪些可热更新 |
| 配置验证缺失 | 启动时不验证配置完整性 |
| 环境切换困难 | 不同品种/市场需要手动修改配置 |

---

## 二、分模块配置方案

### 2.1 新的目录结构

```
config/
├── main.yaml                # 主入口配置（仅包含引用）
├── base.yaml                # 基础配置（合约、连接信息）
├── profiles/
│   ├── conservative/        # 保守模式配置目录
│   │   ├── quoting.yaml     # 报价参数
│   │   ├── risk.yaml        # 风控参数
│   │   └── alpha.yaml       # Alpha参数
│   ├── standard/            # 标准模式
│   └── aggressive/          # 激进模式
├── modules/
│   ├── quoting.yaml         # 报价模块配置
│   ├── risk.yaml            # 风控模块配置
│   ├── alpha.yaml           # Alpha模块配置
│   ├── spread_optimizer.yaml # 价差优化配置
│   ├── toxicity.yaml        # 毒性检测配置
│   ├── market_state.yaml    # 市场状态配置
│   ├── auto_cancel.yaml     # 自动撤单配置
│   ├── correlation.yaml     # 相关性管理配置
│   ├── synthetic_signal.yaml # 综合信号配置
│   ├── spread_arbitrage.yaml # 跨期套利配置
│   └── self_trade_prev.yaml  # 自成交防护配置
├── contracts/
│   ├── ag.yaml              # 白银合约配置
│   ├── if.yaml              # 沪深300合约配置
│   └── ic.yaml              # 中证500合约配置
└── hot_update.yaml          # 热更新参数定义
```

### 2.2 主入口配置 (main.yaml)

```yaml
# ============================================================
# WtFutuCore 主入口配置
# 仅包含模块引用，不包含具体参数
# ============================================================

# 基础配置
base:
  file: base.yaml

# 模式配置（选择一种）
profile: standard  # conservative | standard | aggressive

# 模块配置
modules:
  quoting: modules/quoting.yaml
  risk: modules/risk.yaml
  alpha: modules/alpha.yaml
  spread_optimizer: modules/spread_optimizer.yaml
  toxicity: modules/toxicity.yaml
  market_state: modules/market_state.yaml
  auto_cancel: modules/auto_cancel.yaml
  correlation: modules/correlation.yaml
  synthetic_signal: modules/synthetic_signal.yaml
  spread_arbitrage: modules/spread_arbitrage.yaml
  self_trade_prev: modules/self_trade_prev.yaml

# 合约配置
contracts:
  - file: contracts/ag.yaml
  - file: contracts/if.yaml

# 热更新配置
hot_update:
  file: hot_update.yaml
  enabled: true
  check_interval_ms: 5000  # 检查间隔
```

### 2.3 模块配置示例

#### quoting.yaml (报价模块)

```yaml
# ============================================================
# 报价模块配置
# ============================================================

# 核心报价参数
numLevels: 1                    # 每边档位数
baseSpread: 3.0                 # 基础价差 (tick)
baseQty: 3.0                    # 基础量 (手)
qtyDecay: 0.7                   # 外层衰减
levelStep: 1.0                  # 档位间距 (tick)

# 库存管理
maxInventory: 100.0             # 最大库存
skewFactor: 0.01                # GLFT φ系数
maxSkew: 5.0                    # 最大倾斜 (tick)
hedgeRatio: 1.0                 # 对冲比例
targetInventory: 0              # 目标库存

# 统一参数
stickyThreshold: 1.0            # 价格粘性阈值
maxPriceDeviation: 20.0         # 最大价格偏离
skewSensitivity: 2.0            # Skew 非线性增强系数
aggressiveSkewThreshold: 0.5    # 激进 Skew 触发阈值
oneSidedThreshold: 0.8          # 单向报价阈值

# 模块开关
enabled: true
```

#### risk.yaml (风控模块)

```yaml
# ============================================================
# 风控模块配置
# ============================================================

# 流控参数
rateLimits:
  maxOrdersPerSec: 50
  maxCancelsPerSec: 30
  maxTradesPerSec: 20

# 风险限制
limits:
  deltaLimit: 5000000.0
  maxDelta: 100000000.0
  maxExposure: 300000000.0
  hedgeThreshold: 3000000.0
  maxSpreadMult: 3.0
  maxDailyLoss: -200000.0

# 恢复机制
recovery:
  cooldownMs: 30000
  checkIntervalMs: 5000
  recoveryThreshold: 0.8

# 收盘前平仓
closeout:
  minutesBefore: 5
  flattenPosition: true

# 下单错误处理
orderError:
  threshold: 3

# 模块开关
enabled: true
```

#### alpha.yaml (Alpha模块)

```yaml
# ============================================================
# Alpha预测模块配置
# ============================================================

# 核心参数
sensitivity: 0.5              # η: Alpha对价格影响
ofiWeight: 0.4                # OFI权重
tradeWeight: 0.3              # 交易流权重
leadlagWeight: 0.3            # 跨品种权重
emaFactor: 0.3                # EMA平滑因子
strongThreshold: 0.7          # 强信号阈值

# 窗口参数
ofiWindow: 50                 # OFI计算窗口
tradeWindow: 100              # 交易流计算窗口

# 模块开关
enabled: true
```

### 2.4 合约配置示例

#### contracts/ag.yaml (白银合约)

```yaml
# ============================================================
# 白银合约配置
# ============================================================

# 锚定合约
anchorCode: SHFE.ag2606

# 做市合约列表
contracts:
  - code: SHFE.ag2606
    maxPosition: 50
    maxDelta: 500000
    targetPosition: 30
  - code: SHFE.ag2612
    maxPosition: 50
    maxDelta: 500000
    targetPosition: 30

# 价差对配置（可选）
spreadPairs:
  - pairId: ag_2606_2612
    leg1: SHFE.ag2606
    leg2: SHFE.ag2612
    entryZScore: 2.0
    exitZScore: 0.5
```

---

## 三、热更新配置方案

### 3.1 热更新参数分类

| 类别 | 参数 | 更新方式 | 影响 |
|------|------|----------|------|
| 报价参数 | baseSpread, baseQty, skewFactor | 立即生效 | 影响下次报价 |
| 风控参数 | deltaLimit, maxDailyLoss | 立即生效 | 影响风控检查 |
| Alpha参数 | sensitivity, weights | 立即生效 | 影响下次Alpha计算 |
| 毒性参数 | vpinThreshold, cooloffMs | 立即生效 | 影响毒性判断 |
| 撤单参数 | maxAgeMs, priceDeviation | 立即生效 | 影响撤单决策 |

### 3.2 热更新配置文件 (hot_update.yaml)

```yaml
# ============================================================
# 热更新参数配置
# ============================================================

# 热更新开关
enabled: true

# 检查间隔（毫秒）
check_interval_ms: 5000

# 热更新参数列表
params:
  # 报价参数
  baseSpread:
    type: double
    default: 3.0
    min: 0.5
    max: 20.0
    hot_update: true
    
  baseQty:
    type: double
    default: 3.0
    min: 1.0
    max: 100.0
    hot_update: true
    
  skewFactor:
    type: double
    default: 0.01
    min: 0.001
    max: 0.1
    hot_update: true
    
  maxInventory:
    type: double
    default: 100.0
    min: 10.0
    max: 1000.0
    hot_update: true
    
  # 风控参数
  deltaLimit:
    type: double
    default: 5000000.0
    min: 100000.0
    max: 100000000.0
    hot_update: true
    
  maxDailyLoss:
    type: double
    default: -200000.0
    min: -1000000.0
    max: 0
    hot_update: true
    
  # Alpha参数
  alphaSensitivity:
    type: double
    default: 0.5
    min: 0.0
    max: 2.0
    hot_update: true
    
  # 毒性参数
  toxicityThreshold:
    type: double
    default: 0.7
    min: 0.3
    max: 1.0
    hot_update: true
    
  toxicityCooloffMs:
    type: int
    default: 5000
    min: 1000
    max: 60000
    hot_update: true
```

### 3.3 热更新实现方案

#### 3.3.1 共享内存热更新

```cpp
// 热更新管理器
class HotUpdateManager {
public:
    // 初始化
    bool init(const std::string& config_file);
    
    // 注册热更新参数
    template<typename T>
    void registerParam(const std::string& name, T* target, 
                       T default_val, T min_val, T max_val);
    
    // 检查更新（定期调用）
    void checkUpdates();
    
    // 手动触发更新
    void triggerUpdate(const std::string& param_name, double new_value);
    
private:
    struct ParamInfo {
        void* target;
        ParamType type;
        double min_val, max_val;
        std::atomic<double> shared_value;  // 共享内存
    };
    
    std::unordered_map<std::string, ParamInfo> _params;
    std::string _config_file;
    std::atomic<uint64_t> _last_check_time{0};
};
```

#### 3.3.2 热更新流程

```
┌─────────────────────────────────────────────────────────────┐
│                      热更新流程                              │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  外部工具/脚本                                               │
│       │                                                     │
│       │ 修改共享内存中的参数值                               │
│       ▼                                                     │
│  ┌─────────────────┐                                        │
│  │ 共享内存区域     │                                        │
│  │ (参数值 + 版本号) │                                        │
│  └─────────────────┘                                        │
│       │                                                     │
│       │ 定期检查 (每5秒)                                     │
│       ▼                                                     │
│  ┌─────────────────┐                                        │
│  │ HotUpdateManager │                                        │
│  │ 1. 读取共享内存  │                                        │
│  │ 2. 比较版本号    │                                        │
│  │ 3. 更新目标变量  │                                        │
│  │ 4. 触发回调      │                                        │
│  └─────────────────┘                                        │
│       │                                                     │
│       │ on_params_updated() 回调                            │
│       ▼                                                     │
│  ┌─────────────────┐                                        │
│  │ UftFutuMmStrategy│                                        │
│  │ 1. 同步到模块    │                                        │
│  │ 2. 记录日志      │                                        │
│  └─────────────────┘                                        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 四、配置验证方案

### 4.1 启动时验证

```cpp
// 配置验证器
class ConfigValidator {
public:
    // 验证配置完整性
    static ValidationResult validate(const Config& config);
    
    // 验证参数范围
    static bool validateRange(const std::string& param, double value);
    
    // 验证参数一致性
    static bool validateConsistency(const Config& config);
    
    // 输出验证报告
    static void printReport(const ValidationResult& result);
};

// 验证结果
struct ValidationResult {
    bool is_valid;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};
```

### 4.2 验证规则

| 规则 | 说明 |
|------|------|
| 范围检查 | 参数值必须在 min/max 范围内 |
| 一致性检查 | 相关参数必须一致（如 entryZ > exitZ） |
| 完整性检查 | 必需参数不能缺失 |
| 类型检查 | 参数类型必须匹配 |

---

## 五、迁移指南

### 5.1 从旧配置迁移到新配置

```bash
# 1. 备份旧配置
cp config.yaml config.yaml.bak

# 2. 使用迁移工具
python tools/migrate_config.py config.yaml --output-dir config/

# 3. 验证新配置
./WtUftRunner --validate-config config/main.yaml

# 4. 启动新配置
./WtUftRunner config/main.yaml
```

### 5.2 迁移工具功能

- 自动拆分配置文件
- 参数去重和归一化
- 生成验证报告
- 备份原始配置

---

## 六、配置管理最佳实践

### 6.1 环境隔离

```
config/
├── dev/           # 开发环境
├── test/          # 测试环境
├── staging/       # 预发布环境
└── prod/          # 生产环境
```

### 6.2 版本控制

- 配置文件纳入 Git 管理
- 敏感信息（密码、密钥）使用环境变量
- 配置变更通过 PR 审核

### 6.3 配置模板

为不同品种提供配置模板：
- `templates/ag.yaml` - 白银
- `templates/if.yaml` - 沪深300
- `templates/ic.yaml` - 中证500

### 6.4 配置检查清单

上线前检查：
- [ ] 所有必需参数已配置
- [ ] 参数值在有效范围内
- [ ] 相关参数一致
- [ ] 合约代码格式正确
- [ ] 连接信息已更新
- [ ] 风控参数合理