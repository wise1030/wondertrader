---
name: KS Data Fetch
description: 从 KS/MinIO 远程数据服务器 (ks2wtpy) 拉取 K线/tick 数据并转换为 WonderTrader 存储格式，含期权 tick 拉取、数据可用性探测、端到端 replay 数据准备
metadata:
  opencode/autoinvoke: true
---

# KS 数据服务器取数（ks2wtpy）

从 KS 服务商数据服务器（MinIO/S3）拉取期货/期权历史数据并转换为 WonderTrader 可直接消费的存储格式（CSV / DSB / LMDB / basefiles）。

## 工具位置与环境

- 工具目录：`/mnt/d/gf_pc/WonderTrader/wtpy/tools/ks2wtpy`（本仓库外，属 wtpy 项目）
- 凭据：`credentials.json`（与工具同目录，含 `url`/`accessKey`/`secretKey`）
- S3 endpoint：`172.16.36.3:6900`，bucket：`ks-data`，`secure=false`
- Python 依赖：`minio`、`zstandard`（已装于 anaconda python3）
- 输出约定：传 `--output /mnt/d/gf_pc/WonderTrader/wondertrader/dist/WtBtFutu/storage` 时，tick DSB 直接落在 `his/ticks/{EXCHG}/{YYYYMMDD}/{code}.dsb`，即 WtBtRunner replayer 的读取路径

## 数据布局（KS S3）

```text
ks-data/{EXCHG}/{PRODUCT}/L1/{kline_1d|kline_1m|kline_5m|tick}/{YYYYMMDD}.tar.gz
```

- CFFEX 股指期权（IO/HO/MO）是纯期权独立目录：合约形如 `IO2509-C-4250`
- 商品期权与期货共享目录（SHFE cu/ag/au、DCE、CZCE、INE sc）：期权按合约代码模式识别（如 `cu2506C50000`）
- tar.gz 内含每合约一个 CSV；tick CSV 字段为 KS 标准格式

## 快速用法

### 1. 探测数据可用性（优先 stat，不要 list）

**关键陷阱**：该服务器 list API 经常超时（>60s 无响应），但 stat/GET 正常。
不要用 `client.list_objects(...)` 探测日期；用 `stat_object` 逐个探测候选日期：

```python
from minio import Minio
from urllib3 import PoolManager, Retry
import json
creds = json.load(open('/mnt/d/gf_pc/WonderTrader/wtpy/tools/ks2wtpy/credentials.json'))
http_client = PoolManager(timeout=30, retries=Retry(total=1))
client = Minio('172.16.36.3:6900', access_key=creds['accessKey'],
               secret_key=creds['secretKey'], secure=False, http_client=http_client)
client.stat_object('ks-data', 'SHFE/cu/L1/tick/20260605.tar.gz')   # 存在即返回，HEAD 请求很快
```

### 2. 拉取 tick（K线同理，去掉 --tick/--tick-dates）

```bash
cd /mnt/d/gf_pc/WonderTrader/wtpy/tools/ks2wtpy
python3 main.py --tick --s3 \
  --s3-endpoint 172.16.36.3:6900 \
  --s3-access-key "$(python3 -c "import json;print(json.load(open('credentials.json'))['accessKey'])")" \
  --s3-secret-key "$(python3 -c "import json;print(json.load(open('credentials.json'))['secretKey'])")" \
  --s3-bucket ks-data \
  --s3-cache-dir /tmp/opencode/ks_s3cache \
  --products SHFE.cu \
  --include-option \              # 共享目录保留期权；CFFEX IO/HO/MO 用 --include-cffex-options
  --tick-dates 20260602,20260603 \
  --output /mnt/d/gf_pc/WonderTrader/wondertrader/dist/WtBtFutu/storage \
  --log-level INFO
```

要点：
- `--include-option`：共享品种目录保留期权合约（默认过滤掉）
- `--include-cffex-options`：CFFEX IO/HO/MO 纯期权流水线（排除 *_main/*_index）
- `--tick-code CODE`：只转单个合约（如 `cu2606` / `IO2606-C-4300`）
- `--s3-cache-dir` 务必指定到本地持久目录：tar.gz 缓存在 `{cache}/{EXCHG}/{PROD}/tick/{date}.tar.gz`，重跑零下载
- 输出文件：`{output}/his/ticks/{EXCHG}/{trading_day}/{code}.dsb`（zstd 压缩，WTSTickStruct 512B/条）
- 验证：`python3 -c "from dsb_writer import read_tick_dsb_file; print(read_tick_dsb_file('<path>'))"`

### 3. 服务器故障模式与处理

**2026-10-31 实测重大发现**：MinIO 外网数据面挂起时，可用 **root SSH + 服务器环回** 完整绕过：

| 现象 | 含义 | 处理 |
|---|---|---|
| `stat_object` 秒回，`get_object` 长时间 0 字节 | **数据面挂起**（元数据面正常） | 用带重试的后台循环等服务恢复；已缓存 tar 的日期不受影响 |
| `list_objects` 超时 | list API 不可用或极慢 | 改用 stat 探测（见上） |
| 匿名/未签名 GET 返回 403 | 正常 | 必须用签名请求（minio 客户端） |
| `Read timed out` 偶发 | 网络抖动 | 已下载部分不落盘，重试整文件；缓存命中后无需重试 |

**SSH 绕行管道**（本机 `~/.ssh/id_rsa` 可登录 `root@172.16.36.3`，MinIO 宿主机 hyzb-3）：
1. MinIO 对象盘在 `/nvme/minio/data/{bucket}/...`，xl 格式：`{obj}/xl.meta` + `{obj}/{uuid}/part.N`（仅 part.1 带 32 字节头，**不要手工拼接**，CRC 会错）
2. **环回拉取**：presign 指定 `region='us-east-1'`（跳过 region 查询）且 host=127.0.0.1:6900，scp 脚本到服务器后 `curl` 环回 GET —— 服务器自读 NVMe 满速且数据面正常
3. **服务器侧转换**：`convert_local.py`（纯 stdlib，无 minio/zstd 依赖）解析 tar CSV → `write_tick_dsb(use_compression=False)` 写 **raw V2(0x03)** DSB（注意：zstd 缺失时的 0x01 fallback 是"老结构体"标记，C++ 读不了）
4. **分块拉回**：外网 SSH/S3 大数据流间歇性 0 字节，用 `pull_chunks.py`（1MB `dd skip=N` 分块 + 逐块重试 + 断点续传）；gzip 打包后再传（tick DSB 压 5-10 倍）
5. 一键封装：`src/WtOptionCore/replay/fetch_option_ticks.sh`（product/链前缀/日期参数化）

后台重试模板（数据面恢复后自动完成下载）见 `scripts/retry_pull.py`：

```bash
python3 .opencode/skills/ks-data-fetch/scripts/retry_pull.py \
  --keys SHFE/cu/L1/tick/20260605.tar.gz --dest-dir /tmp/opencode/ks_s3cache --minutes 45
```

下载完成的 tar 可用 TickConverter 离线转换（无需联网）：

```python
import sys; sys.path.insert(0, '/mnt/d/gf_pc/WonderTrader/wtpy/tools/ks2wtpy')
# 把 tar 放入 {cache}/{EXCHG}/{PROD}/tick/ 后正常走 main.py --tick 即可（缓存命中）
```

### 4. basefiles（commodities/contracts/sessions/hots/fees/holidays）

```bash
python3 main.py --config-only --s3 ... --products SHFE.cu --include-option --output <base>
```

注意：config-only 也需读取日线 tar 列举合约（走 GET），数据面故障时同样会卡。
WtOptionCore 回放最低要求：commodities 含 SHFE.cu（category 为期货/期权、pricetick、volscale、session 关联），contracts 含当日全部 cu 期货+期权合约条目。

## 端到端期权 tick replay 数据准备清单

一键脚本（推荐）：`src/WtOptionCore/replay/run_option_replay.sh`（缺数据时自动调用 `fetch_option_ticks.sh` 取数 → DSB校验 → WtBtRunner → p99/p99.9提取）。手动步骤如下：

1. 拉 tick（外网正常时）：`main.py --tick --products SHFE.cu --include-option --tick-dates <D>` → `dist/WtBtFutu/storage/his/ticks/SHFE/<D>/*.dsb`；外网故障时走 `fetch_option_ticks.sh`（服务器侧转换 + 分块拉回）
2. **basefiles 是硬要求**（hft 回测 replayer 只回放已订阅合约，而期权代码来自 comminfo 发现）：commodities 中 SHFE.cu `category=2`（CC_FutOption，`isOption()` 判定），contracts 含标的期货+期权链全部代码（含 expiredate）。用 `src/WtOptionCore/replay/gen_basefiles.py` 生成（输出在 `replay_common/`）
3. 运行：`LD_LIBRARY_PATH=<repo>/src/build_all/build_x64/Debug/bin ./WtBtRunner.fresh -c configbt_option_replay.yaml -l logcfgbt_option.yaml`（fresh 二进制/模块为当日构建，dist 自带旧 .so 已备份 libWtOptionCore.so.bak_20260708）
4. 延迟指标：HftOptionStrategy batch 日志输出 `p99/p99.9`（OptionTimeService+LatencyHistogram）；replay 脚本 `src/WtOptionCore/tests/test_replay_benchmark.cpp` 输出合成基线 p50/p95/p99/p999

实测备忘（2026-06 数据）：cu2606 期权 6 月初已到期（6/2 起 tick 中只剩期货 cu2606），近月期权链用 **cu2607**；单日全品种 ~480 合约（~470 期权、~190 万 tick），2607 链 57 合约 ~130 万 tick/日，gzip 后 ~14MB/日。

## 已知限制

- 该服务器 tick 仅覆盖近月活跃期（2026 年中以后数据较全，更早日期可能无 tick）
- WTSTickStruct 中 settle/涨跌停/pre_close 等字段 KS 无来源，填 0
- CZCE/CFFEX tick 仅 1 档买卖，SHFE/DCE/INE/GFEX 5 档
