#!/usr/bin/env python3
"""Retry-pull KS S3 objects (data-plane hang workaround).

Stat (HEAD) works even when GET hangs; this loop retries GET with short timeouts
until the server data plane recovers, writing each completed object to dest dir
as {dest}/{EXCHG}/{PROD}/tick/{date}.tar.gz so ks2wtpy s3-cache hits on rerun.

Usage:
  python3 retry_pull.py --keys SHFE/cu/L1/tick/20260605.tar.gz [--keys ...] \
      --dest-dir /tmp/opencode/ks_s3cache --minutes 45
"""
import argparse
import json
import os
import time

from minio import Minio
from urllib3 import PoolManager, Retry

CREDS = '/mnt/d/gf_pc/WonderTrader/wtpy/tools/ks2wtpy/credentials.json'


def cache_path(dest_dir: str, s3_key: str) -> str:
    # s3_key: {EXCHG}/{PROD}/L1/tick/{date}.tar.gz -> {dest}/{EXCHG}/{PROD}/tick/{date}.tar.gz
    parts = s3_key.split('/')
    rel = os.path.join(parts[0], parts[1], parts[2], parts[3])
    return os.path.join(dest_dir, rel)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--keys', nargs='+', required=True, help='S3 object keys')
    ap.add_argument('--dest-dir', required=True, help='local cache root')
    ap.add_argument('--minutes', type=float, default=45.0)
    ap.add_argument('--interval', type=float, default=60.0, help='retry interval seconds')
    ap.add_argument('--timeout', type=float, default=30.0, help='per-attempt GET timeout')
    args = ap.parse_args()

    creds = json.load(open(CREDS))
    pending = [(k, cache_path(args.dest_dir, k)) for k in args.keys]
    pending = [(k, p) for k, p in pending
               if not (os.path.exists(p) and os.path.getsize(p) > 1000000)]

    deadline = time.time() + args.minutes * 60
    attempt = 0
    while pending and time.time() < deadline:
        attempt += 1
        http_client = PoolManager(timeout=args.timeout, retries=Retry(total=0))
        client = Minio('172.16.36.3:6900', access_key=creds['accessKey'],
                       secret_key=creds['secretKey'], secure=False,
                       http_client=http_client)
        still = []
        for key, path in pending:
            try:
                resp = client.get_object('ks-data', key)
                data = resp.read()
                resp.close()
                resp.release_conn()
                if data and len(data) > 1000000:
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    tmp = path + '.tmp'
                    with open(tmp, 'wb') as f:
                        f.write(data)
                    os.replace(tmp, path)
                    print(f'[attempt {attempt}] OK {key} {len(data)/1024/1024:.1f}MB', flush=True)
                    continue
            except Exception as e:  # noqa: BLE001 - report and retry
                print(f'[attempt {attempt}] FAIL {key}: {type(e).__name__}', flush=True)
            still.append((key, path))
        pending = still
        if pending:
            time.sleep(args.interval)

    print(f'DONE pending={len(pending)}', flush=True)
    raise SystemExit(0 if not pending else 1)


if __name__ == '__main__':
    main()
