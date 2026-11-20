// P1 batch-6 helpers: Black-76 IV inversion + series fetch + percentile bands.
// Shared by the AnalysisTab panels (smile band, vol cone, attribution).

// ---------------------------------------------------------------------------
// Black-76 (discount=1 — futures options, intraday horizon)
// ---------------------------------------------------------------------------
const normCdf = (x: number): number => {
  const t = 1 / (1 + 0.2316419 * Math.abs(x));
  const d = 0.3989423 * Math.exp(-x * x / 2);
  const p = t * (0.3193815 + t * (-0.3565638 + t * (1.781478 + t * (-1.821256 + t * 1.330274))));
  return x >= 0 ? 1 - d * p : d * p;
};

const blackPx = (F: number, K: number, T: number, sigma: number, isCall: boolean): number => {
  if (sigma <= 0) return isCall ? Math.max(0, F - K) : Math.max(0, K - F);
  const sq = sigma * Math.sqrt(T);
  const d1 = (Math.log(F / K) + 0.5 * sigma * sigma * T) / sq;
  const d2 = d1 - sq;
  return isCall ? F * normCdf(d1) - K * normCdf(d2)
                : K * normCdf(-d2) - F * normCdf(-d1);
};

/// Invert a futures-option price to IV by bisection.  Null when the price
/// is below intrinsic (stale/crossed book) or inputs are unusable.
export const blackIV = (px: number, F: number, K: number, dte: number, isCall: boolean): number | null => {
  if (!(px > 0) || !(F > 0) || !(K > 0) || !(dte > 0)) return null;
  const T = dte / 365.0;
  const intrinsic = isCall ? Math.max(0, F - K) : Math.max(0, K - F);
  if (px < intrinsic - 1e-9) return null;
  let lo = 1e-4, hi = 3.0;
  if (blackPx(F, K, T, hi, isCall) < px) return hi;   // above the grid -> clamp
  for (let i = 0; i < 40; ++i) {
    const mid = (lo + hi) / 2;
    if (blackPx(F, K, T, mid, isCall) > px) hi = mid; else lo = mid;
  }
  return (lo + hi) / 2;
};

// ---------------------------------------------------------------------------
// Series API (middleware /api/series -> AnalysisRecorder CSVs)
// ---------------------------------------------------------------------------
export interface SeriesResp { columns: string[]; rows: number[][]; }

export async function fetchSeries(kind: 'vol' | 'pf', tdate: number): Promise<SeriesResp | null> {
  try {
    const r = await fetch(`/api/series?kind=${kind}&tdate=${tdate}`, { cache: 'no-cache' });
    if (!r.ok) return null;
    return await r.json();
  } catch {
    return null;
  }
}

/// Quantiles of a numeric sample at ps (0..1), linear-interpolated.
export const quantiles = (sample: number[], ps: number[]): number[] => {
  if (sample.length === 0) return ps.map(() => NaN);
  const s = [...sample].sort((a, b) => a - b);
  return ps.map(p => {
    const idx = p * (s.length - 1);
    const lo = Math.floor(idx), hi = Math.ceil(idx);
    return s[lo] + (s[hi] - s[lo]) * (idx - lo);
  });
};

export const BAND_PS = [0, 0.1, 0.25, 0.5, 0.75, 0.9, 1];
export const BAND_NAMES = ['MIN', 'P10', 'P25', 'P50', 'P75', 'P90', 'MAX'];
