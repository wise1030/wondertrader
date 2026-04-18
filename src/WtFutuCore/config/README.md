# WtFutuCore 配置说明

> 📖 **配置优化方案**: 详细的分模块配置和热更新设计请参阅: [CONFIG_OPTIMIZATION.md](CONFIG_OPTIMIZATION.md)
>
> 包含：分模块配置方案、热更新实现方案、配置验证方案、迁移指南

## 目录结构

```
config/
├── main.yaml                 # 主配置文件（基于 dist/WtRunnerFutu/config.yaml 优化）
├── hot_update.yaml           # 热更新参数配置
├── modules/                  # 分模块配置目录
│   ├── quoting.yaml          # 报价模块配置
│   ├── risk.yaml             # 风控模块配置
│   ├── alpha.yaml            # Alpha模块配置
│   ├── spread_optimizer.yaml # 价差优化配置
│   ├── toxicity.yaml         # 毒性检测配置
│   ├── market_state.yaml     # 市场状态配置
│   ├── auto_cancel.yaml      # 自动撤单配置
│   ├── correlation.yaml      # 相关性管理配置
│   └── synthetic_signal.yaml # 综合信号配置
├── profiles/
│   ├── conservative.yaml     # 保守模式配置模板
│   ├── standard.yaml         # 标准模式配置模板
│   └── aggressive.yaml       # 激进模式配置模板
└── strategy/
    ├── quoting.yaml          # 报价参数参考配置
    ├── risk.yaml             # 风控参数参考配置
    └── modules.yaml          # 模块参数参考配置
```

## 快速开始

### 1. 使用主配置文件

```bash
# 使用优化后的主配置文件
./WtUftRunner ./config/main.yaml
```

### 2. 必须修改的配置项

#### 2.1 行情源配置

```yaml
parsers:
  - active: true
    id: parser_ctp
    module: ParserCTP
    front: tcp://your_md_front      # 修改为实际行情前置地址
    userid: "your_userid"           # 修改为实际用户ID
    password: "your_password"       # 修改为实际密码
    brokerid: "your_brokerid"       # 修改为实际经纪商ID
```

#### 2.2 交易通道配置

```yaml
traders:
  - active: true
    id: trader_ctp
    module: TraderCTP
    front: tcp://your_td_front      # 修改为实际交易前置地址
    broker: "your_broker"           # 修改为实际经纪商
    appid: your_appid               # 修改为实际APPID
    authcode: "your_authcode"       # 修改为实际授权码
    user: "your_user"               # 修改为实际用户
    pass: "your_pass"               # 修改为实际密码
```

#### 2.3 合约配置

```yaml
strategies:
  uft:
    - params:
        anchorCode: SHFE.ag2606    # 修改为实际主力合约
        contracts:
          - code: SHFE.ag2606      # 修改为实际做市合约
            maxPosition: 50
            maxDelta: 5000000
```

## 分模块配置说明

### 模块配置列表

| 模块 | 配置文件 | 说明 |
|------|----------|------|
| 报价模块 | `modules/quoting.yaml` | 核心报价参数、库存管理、Sticky/Skew |
| 风控模块 | `modules/risk.yaml` | 流控参数、风险限制、恢复机制 |
| Alpha模块 | `modules/alpha.yaml` | OFI/Trade/LeadLag因子参数 |
| 价差优化 | `modules/spread_optimizer.yaml` | GLFT价差优化参数 |
| 毒性检测 | `modules/toxicity.yaml` | VPIN阈值、检测窗口、冷却时间 |
| 市场状态 | `modules/market_state.yaml` | 波动率、价格变动、成交量阈值 |
| 自动撤单 | `modules/auto_cancel.yaml` | 订单超时、价格偏离撤单 |
| 相关性管理 | `modules/correlation.yaml` | 统计窗口、最小相关系数 |
| 综合信号 | `modules/synthetic_signal.yaml` | 信号融合权重、Tick推断、自身成交校准 |

### 使用分模块配置

分模块配置文件作为参考模板，可以在主配置文件中引用对应的参数值：

```yaml
# 示例：从 modules/quoting.yaml 复制参数到主配置
strategies:
  uft:
    - params:
        # 核心报价参数 (来自 modules/quoting.yaml)
        numLevels: 1
        baseSpread: 3.0
        baseQty: 3.0
        qtyDecay: 0.7
        levelStep: 1.0
```

## 热更新配置

### 热更新参数列表

| 参数 | 类型 | 默认值 | 范围 | 说明 |
|------|------|--------|------|------|
| baseSpread | double | 3.0 | 0.5-20.0 | 基础价差 (tick) |
| baseQty | double | 3.0 | 1.0-100.0 | 基础量 (手) |
| skewFactor | double | 0.01 | 0.001-0.1 | 库存惩罚系数 |
| maxInventory | double | 100.0 | 10.0-1000.0 | 最大库存 |
| deltaLimit | double | 5000000.0 | 100000.0-100000000.0 | Delta阈值 |
| maxDailyLoss | double | -200000.0 | -1000000.0-0 | 最大日亏损 |
| alphaSensitivity | double | 0.5 | 0.0-2.0 | Alpha敏感度 |
| toxicityThreshold | double | 0.7 | 0.3-1.0 | 毒性阈值 |
| toxicityCooloffMs | int | 5000 | 1000-60000 | 毒性冷却时间 |

### 热更新使用方式

```bash
# 修改热更新参数
# 方式1: 直接修改 hot_update.yaml 文件
# 方式2: 使用共享内存工具更新参数

# 热更新检查间隔: 5秒
# 参数修改后自动生效，无需重启策略
```

## 配置参数说明

### 核心参数（必须配置）

| 参数 | 说明 | 默认值 | 建议范围 |
|------|------|--------|----------|
| anchorCode | 锚定合约（对冲用） | - | 主力合约 |
| contracts | 做市合约列表 | - | 实际交易合约 |
| deltaLimit | Delta阈值 | 5000000 | 根据资金规模调整 |
| maxDailyLoss | 最大日亏损 | -200000 | 根据风险承受能力调整 |
| numLevels | 报价档位数 | 1 | 1-5 |
| baseSpread | 基础价差(tick) | 3.0 | 2-10 |
| baseQty | 基础量(手) | 3.0 | 1-10 |

### 推荐参数（建议调整）

| 参数 | 说明 | 默认值 | 建议范围 |
|------|------|--------|----------|
| maxInventory | 最大库存 | 100 | 20-200 |
| skewFactor | 库存惩罚系数 | 0.01 | 0.005-0.02 |
| hedgeRatio | 对冲比例 | 1.0 | 0.5-1.0 |
| alphaEngine.sensitivity | Alpha敏感度 | 0.5 | 0.3-0.8 |

## 配置模式对比

### 保守模式 vs 标准模式 vs 激进模式

| 参数 | 保守 | 标准 | 激进 |
|------|------|------|------|
| maxPosition | 20 | 50 | 100 |
| baseSpread | 5.0 | 3.0 | 2.0 |
| baseQty | 1.0 | 3.0 | 5.0 |
| maxInventory | 20.0 | 100.0 | 200.0 |
| deltaLimit | 1000000 | 5000000 | 10000000 |
| maxDailyLoss | -50000 | -200000 | -500000 |
| numLevels | 1 | 1 | 3 |
| maxOrdersPerSec | 20 | 50 | 100 |
| closeoutMinutesBefore | 10 | 5 | 3 |

## 启动方式

```bash
# 使用优化后的主配置
./WtUftRunner ./config/main.yaml

# 使用保守模式（复制模板后修改）
cp config/profiles/conservative.yaml config.yaml
./WtUftRunner ./config.yaml

# 使用标准模式
cp config/profiles/standard.yaml config.yaml
./WtUftRunner ./config.yaml

# 使用激进模式
cp config/profiles/aggressive.yaml config.yaml
./WtUftRunner ./config.yaml
```

## 注意事项

1. **连接信息**: 行情和交易通道的连接信息必须修改为实际值
2. **合约代码**: 合约代码格式需与基础数据文件一致
3. **风险控制**: 建议从小仓位开始，逐步增加风险敞口
4. **参数调优**: 建议先在模拟环境测试，再切换到实盘
5. **日志监控**: 运行过程中注意观察日志输出，及时调整参数
6. **热更新**: 热更新参数修改后自动生效，注意观察策略响应

## 常见问题

### Q: 如何添加新的做市合约？

在 contracts 列表中添加新的合约配置：

```yaml
contracts:
  - code: SHFE.ag2606
    maxPosition: 50
    maxDelta: 5000000
  - code: SHFE.ag2612      # 新添加的合约
    maxPosition: 50
    maxDelta: 5000000
```

### Q: 如何调整报价频率？

报价频率主要由行情tick驱动，无法直接配置。但可以通过调整以下参数间接影响：
- `riskMonitor.maxOrdersPerSec`: 限制每秒最大下单数
- `autoCancel.maxAgeMs`: 订单最大存活时间

### Q: 如何启用跨期价差套利？

修改以下配置：

```yaml
spreadArbitrage:
  enabled: true
  enhanceMarketMaking: true
  maxPosition: 20.0
  entryZScore: 2.0
  exitZScore: 0.5
  windowSize: 200
```

### Q: 如何调整毒性检测灵敏度？

修改以下配置：

```yaml
toxicityDetector:
  vpinThreshold: 0.7    # 降低此值提高灵敏度
  window: 50            # 计算窗口大小
  cooloffMs: 5000       # 熔断冷却时间
```

### Q: 如何使用热更新功能？

1. 在 `hot_update.yaml` 中配置可热更新参数
2. 修改参数值（直接编辑文件或使用共享内存工具）
3. 策略自动检测并应用新参数（检查间隔: 5秒）
4. 查看日志确认参数更新成功