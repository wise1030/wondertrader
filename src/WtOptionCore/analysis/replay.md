# 引擎级 tick 回放操作手册（P1 后续项 6.5）

完整闭环：**生产录流 → qstream CSV → dsb → WtBtRunner + libWtOptionCore 回放**。
已实测验证：34.6 万 tick 的铜期权日（20260602，56 个 cu_o 合约）回放，
策略侧 `Recovery complete` + `enabled=true options=36 strikes=27
underlying=106620`，18.7 万个批量全绿。

## 1. 生产录流（WtRunnerOption）

config.yaml 加（默认关，1s 节奏约 70MB/日，只开在有回放需求的日子）：

```yaml
    analysis:
        enable: true
        interval_sec: 60
        qstream_sec: 1
```

产出 `generated/qstream_<tdate>.csv`（ts,code,bid,ask,last,iv；期权+期货
腿全覆盖——期货腿是 forward/delta 重建的必要输入）。

## 2. 转换 dsb

```bash
python3 src/WtOptionCore/analysis/qstream_to_dsb.py \
    generated/qstream_20261120.csv /btdata SHFE
# 产出 /btdata/his/ticks/SHFE/20261120/<bare_code>.dsb
```

- 格式：12 字节 BlockHeader{type=BT_HIS_Ticks(24), version=RAW_V2(3)} +
  packed WTSTickStruct（512B，pack(8)）。RAW 无需 zstd——框架读取路径对
  非压缩 V2 直接透传（见 WtBtCore/HisDataReplayer.cpp proc_block_data）。
- **不要用 20 字节 BlockHeaderV2 写 RAW 块**：RAW 透传只剥 12 字节，多
  余的 8 字节会让结构体错位（实测段错误，教训已固化在转换器注释里）。
- stdCode 剥成裸 code 做文件名（his/ticks/<exchg>/<date>/<code>.dsb）。

## 3. 回测配置

以 `dist/WtBtRunner/configbt_option_replay.yaml` 为模板：

- `replayer.mode: bin`，`path:` 指向 /btdata（不要带 his/，读取器自己拼）
- `stime/etime` 覆盖交易日时段
- **underlyingCode 用点形分月 `SHFE.cu.2607`**——裸 code 会被
  extractStdCode 整段当 product 而查无会话（防御性 skip + WARN，不再段错误）
- basefiles 用 `dist/WtBtRunner/replay_common/`（含 cu/cu_o 品种与合约）；
  换品种时确认 commodities/contracts/sessions 三件套都有该品种

## 4. 跑回放

```bash
cd dist/WtBtRunner   # 或自带 hft/libWtOptionCore.so 的目录
LD_LIBRARY_PATH=./hft:/path/to/libs ./WtBtRunner -c configbt_replay.yaml -l logcfgbt.yaml
```

日志看 `BtLogs/Runner.log`：`Recovery complete` → `batch #N enabled=true
options=.. strikes=..` 即链路全通。

## 5. 校验工具

- `analysis/qstream_check.py`：录流完整性（单调 ts/坏价/间隙分布）+
  ATM mid 对 volseries fwd 的重放核对
- `analysis/dsb_roundtrip.py`：真实 dsb → qstream CSV → 转换器 → 新 dsb
  的逐 tick 往返一致性（已对 52,439 tick 实测 PASS）

## 已知边界

- qstream 只有 1s 采样 + 最优档价格（iv 列为引擎 impliedVol，可辅助
  smile 复盘）；更细粒度需 WtDataWriter 原生 dsb 录流（引擎级，未启用）。
- `action_time` 由策略时钟映射 HHMMSSmmm——同一交易日内顺序保真，跨
  日场景用 trading_date 分文件天然隔离。
- 撮合参数：`error_rate: 100` 时委托全失败，适合报价链路验证；要看成
  交流程把 error_rate 调 0。
