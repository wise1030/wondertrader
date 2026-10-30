#!/usr/bin/env bash
# 从KS服务器取期权tick并转为WT DSB（服务器侧转换 + 分块拉回，绕过S3/SSH大数据面故障）
# 实测背景(2026-10-31): MinIO外网数据面挂起(GET 0字节)、SSH大数据流间歇性停滞，
# 但 ①root SSH可用 ②服务器环回(127.0.0.1:6900)MinIO正常 ③SSH exec小命令正常。
# 因此: 环回presigned拉tar -> 服务器侧convert_local.py转DSB -> 1MB分块dd拉回。
#
# 前置: ~/.ssh/id_rsa 可登录 root@172.16.36.3 (MinIO宿主机)
# 用法: fetch_option_ticks.sh <product> <chain_prefix> <dates_csv>
#   例: fetch_option_ticks.sh cu cu2607 20260602,20260603,20260604,20260605
set -euo pipefail

PRODUCT="${1:-cu}"
CHAIN="${2:-cu2607}"
DATES="${3:-20260602,20260603,20260604,20260605}"
EXCHG=SHFE
REPO=/mnt/d/gf_pc/WonderTrader/wondertrader
SERVER=root@172.16.36.3
MINIO_HOST=127.0.0.1:6900
BUCKET=ks-data
WORK=/tmp/ksconv
STORAGE="$REPO/dist/WtBtFutu/storage"

echo "=== [0/4] presign loopback URLs (local minio client, region pinned) ==="
python3 - "$MINIO_HOST" "$BUCKET" "$EXCHG" "$PRODUCT" "$DATES" <<'PY'
import json, sys
from datetime import timedelta
from minio import Minio
host, bucket, exchg, product, dates = sys.argv[1:6]
creds = json.load(open('/mnt/d/gf_pc/WonderTrader/wtpy/tools/ks2wtpy/credentials.json'))
client = Minio(host, access_key=creds['accessKey'], secret_key=creds['secretKey'],
               secure=False, region='us-east-1')
urls = {}
for d in dates.split(','):
    urls[d] = client.presigned_get_object(
        bucket, f'{exchg}/{product}/L1/tick/{d}.tar.gz', expires=timedelta(hours=2))
json.dump(urls, open('/tmp/opencode/loop_urls.json', 'w'))
print('presigned', len(urls), 'urls')
PY

echo "=== [1/4] server: loopback fetch tars + verify gzip ==="
python3 - "$SERVER" "$WORK" <<'PY'
import json, subprocess, sys
server, work = sys.argv[1:3]
urls = json.load(open('/tmp/opencode/loop_urls.json'))
cmds = [f'mkdir -p {work}/data {work}/out']
for d, u in urls.items():
    cmds.append(f"curl -s --max-time 120 -o {work}/data/{d}.tar.gz '{u}'"
                f" && gzip -t {work}/data/{d}.tar.gz && echo '{d} OK' || echo '{d} FAILED'")
out = subprocess.run(['ssh', '-o', 'BatchMode=yes', server, ' && '.join(cmds)],
                     capture_output=True, text=True, timeout=600)
print(out.stdout)
if 'FAILED' in out.stdout:
    sys.exit('some tars failed')
PY

echo "=== [2/4] server: convert to DSB (raw V2, futures+options) ==="
ssh -o BatchMode=yes "$SERVER" "cd $WORK && python3 convert_local.py --data-root data --out-root out --exchange $EXCHG --product $PRODUCT --dates $DATES 2>&1 | grep -E 'INFO|ERROR' | tail -8"

echo "=== [3/4] server: package $CHAIN chain + chunk pull back ==="
ssh -o BatchMode=yes "$SERVER" "cd $WORK/out/his/ticks/$EXCHG && rm -f $WORK/pkg/chain_*.tgz && for d in ${DATES//,/ }; do tar czf $WORK/pkg/chain_\$d.tgz -C \$d \$(ls \$d | grep -E '^${CHAIN}[CP]') ${CHAIN}.dsb && echo \"\$d \$(stat -c%s $WORK/pkg/chain_\$d.tgz)\"; done"
for d in ${DATES//,/ }; do
  size=$(ssh -o BatchMode=yes "$SERVER" "stat -c%s $WORK/pkg/chain_$d.tgz")
  python3 /tmp/opencode/pull_chunks.py "$WORK/pkg/chain_$d.tgz" "/tmp/opencode/chain_$d.tgz" "$size"
  dstdir="$STORAGE/his/ticks/$EXCHG/$d"
  mkdir -p "$dstdir"
  tar xzf "/tmp/opencode/chain_$d.tgz" -C "$dstdir"
  echo "$d -> $dstdir ($(ls "$dstdir" | grep -c -E '^'"$CHAIN"'[CP]') option DSBs)"
done

echo "=== [4/4] done ==="
