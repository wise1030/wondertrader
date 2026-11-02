#!/usr/bin/env python3
"""Generate replay basefiles (commodities/contracts) for option tick replay.

Patches dist/common copies for option tick replay:
- Adds SHFE.{prod}_o commodity (category=2 CC_FutOption): the framework parses
  SHFE option std codes (SHFE.cu2607.C.104000) with product "cu_o"
  (CodeHelper::extractStdChnFutOptCode), so option contracts must live under a
  separate "_o" product or replayer get_session_info() returns NULL and crashes.
- Adds contract entries for the underlying future (product=prod) and the
  option chain (product=prod_o); subscription + matching require both.
"""
import json
import os
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
SRC = os.path.join(REPO, 'dist/common')
DST = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'replay_common')

def main() -> None:
    os.makedirs(DST, exist_ok=True)
    for f in ['commodities.json', 'contracts.json', 'fees.json',
              'holidays.json', 'hots.json', 'sessions.json']:
        shutil.copy(os.path.join(SRC, f), os.path.join(DST, f))

    cm = json.load(open(os.path.join(DST, 'commodities.json')))
    # Framework expects SHFE options under a separate "{prod}_o" product
    # (see CodeHelper::extractStdChnFutOptCode); futures stay under "cu".
    base = cm['SHFE']['cu']
    opt = dict(base)
    opt['category'] = 2  # CC_FutOption
    opt['name'] = base.get('name', '') + '期权'
    cm['SHFE']['cu_o'] = opt
    json.dump(cm, open(os.path.join(DST, 'commodities.json'), 'w'),
              ensure_ascii=False, indent=1)

    codes_file = sys.argv[1] if len(sys.argv) > 1 else os.path.join(DST, 'option_codes.txt')
    ct = json.load(open(os.path.join(DST, 'contracts.json')))
    shfe = ct.setdefault('SHFE', {})
    n = 0
    for line in open(codes_file):
        code = line.strip()
        if not code:
            continue
        base_m, cp, strike = code[2:6], code[6], code[7:]
        shfe[code] = {
            'name': f'沪铜{base_m}{"购" if cp == "C" else "沽"}{strike}',
            'code': code, 'exchg': 'SHFE', 'product': 'cu_o',
            'maxlimitqty': 500, 'maxmarketqty': 30,
            'minlimitqty': 1, 'minmarketqty': 1,
            'opendate': 20251117, 'expiredate': 20260625,
        }
        n += 1
    shfe['cu2607'] = {
        'name': '沪铜2607', 'code': 'cu2607', 'exchg': 'SHFE', 'product': 'cu',
        'maxlimitqty': 500, 'maxmarketqty': 30,
        'minlimitqty': 1, 'minmarketqty': 1,
        'opendate': 20250617, 'expiredate': 20260715,
    }
    json.dump(ct, open(os.path.join(DST, 'contracts.json'), 'w'),
              ensure_ascii=False, indent=1)
    print(f'replay_common generated: {n} option contracts (cu_o) + cu2607')


if __name__ == '__main__':
    main()
