#!/usr/bin/env python3
"""Generate replay basefiles (commodities/contracts) for option tick replay.

Patches dist/common copies: SHFE.cu commodity category=2 (CC_FutOption, required
by WtOptionCore auto-discovery via commInfo->isOption()), and adds contract
entries for the underlying future + option chain (subscription + matching).
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
    cm['SHFE']['cu']['category'] = 2
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
        base, cp, strike = code[2:6], code[6], code[7:]
        shfe[code] = {
            'name': f'沪铜{base}{"购" if cp == "C" else "沽"}{strike}',
            'code': code, 'exchg': 'SHFE', 'product': 'cu',
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
    print(f'replay_common generated: {n} option contracts + cu2607')


if __name__ == '__main__':
    main()
