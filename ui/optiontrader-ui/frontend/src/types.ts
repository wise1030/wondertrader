// Protocol types for the WtOptionCore P1 UI bridge (JSON Lines).
// Mirrors src/WtOptionCore/UiSnapshotView.h + UiSnapshotBuilder encode*().

export interface LegView {
  code: string;
  // P1 batch-2: our resting quote + queue position (0=best, >0=ticks behind,
  // -1=crossing, 9999=none) — bqp for our bid, aqp for our ask.
  obid?: number | null;
  oask?: number | null;
  bqp?: number;
  aqp?: number;
  bid: number | null;
  bidSz: number | null;
  ask: number | null;
  askSz: number | null;
  // P1 follow-up (五档深度): levels beyond best as [px, qty][] (absent when
  // the feed only carries level 1 — CTP options usually do).
  bd?: [number, number][] | null;
  ad?: [number, number][] | null;
  last: number | null;
  theo: number | null;
  iv: number | null;
  delta: number | null;
  vshift: number | null;
  pos: number;
  qmode: number | null;
  pnl: number | null;
}

export interface StrikeView {
  k: number;
  C: LegView;
  P: LegView;
}

export interface ExpiryGreeksView {
  delta: number | null;
  vega: number | null;
  gamma?: number | null;   // batch-5.2 hedge panel
  theta?: number | null;
}

export interface UndView {
  code: string;
  bid: number | null;
  ask: number | null;
  bidSz?: number | null;
  askSz?: number | null;
  pos: number | null;
  // hedge-leg position delta in underlying units (lots x contract size) —
  // same quantity the per-expiry hedge gate uses (audit 11-22 round 2).
  undDelta?: number | null;
  // P1 UI: our futures-leg quotes (symmetric to ordBid/ordAsk on options)
  ourBid?: number | null;
  ourAsk?: number | null;
  ourBidSz?: number | null;
  ourAskSz?: number | null;
}

export interface ExpiryView {
  expiry: number;
  quoting?: boolean;   // false = quoting disabled (config enable=false) — grey out
  fwd: number | null;
  fwdReady: boolean;
  fitReady: boolean;
  underlyingValid: boolean;
  atmVol: number | null;
  atmVolSprd: number | null;
  daysToExpiry: number;
  booksWithBid: number;
  booksTotal: number;
  greeks: ExpiryGreeksView;
  targDelta?: number | null;   // batch-5.2 greek targets (absent = no target)
  targVega?: number | null;
  und: UndView | null;
  strikes: StrikeView[];
}

export interface StateView {
  trading: string;
  reason: string;
  channelReady: boolean;
  underlyingReady: boolean;
  enabled: boolean;
  panicked: boolean;
  tdate: number;
}

export interface GreeksView {
  delta: number | null;      // OPTION positions only
  gamma: number | null;
  vega: number | null;
  vegaTW: number | null;
  theta: number | null;
  // option delta + futures-hedge delta — the delta hedger's trigger
  // quantity (OptionRisk::getPortfolioDelta; audit 11-22 round 2).
  pfDelta?: number | null;
}

export interface PnlView {
  cur: number | null;
  realized: number | null;
  turnover: number | null;
}

export interface RiskView {
  limitsBreached: boolean;
  tps: number;
  tpsMax: number;
  ordersSent: number;
  fills: number;
  cancels: number;
  rejects: number;
  cancelRatio: number | null;
  fillRatio: number | null;
  sessionStopInSec: number | null;
  liveByTag?: number[];    // per-source live orders (batch-4.3)
  fillsByTag?: number[];   // per-source today's fills
}

export interface PredView {
  resolved: number;
  hitRate: number | null;
  mae: number | null;
  scanResolved: number;
  scanHitRate: number | null;
  scanIc: number | null;
}

export interface PerfView {
  batchP50: number | null;
  batchP95: number | null;
  batchP99: number | null;
  aepEvents: number;
  queueDrops: number;
  mdOverflow: number;
  traderDepth: number;
  eventDrops: number;
  snapshotCoalesced: number;
}

export interface OrderView {
  id: number;
  code: string;
  buy: boolean;
  px: number | null;
  qty: number;
  filled: number;
  t: number | null;
  tag?: number;   // 0/undefined=Unknown 1=MM 2=Hedge 3=Scanner (batch-4.3)
}

export interface Snapshot {
  type: 'snapshot';
  proto: number;
  seq: number;
  ts: number;
  sid: string;
  snapshotAgeMs?: number | null;
  state: StateView;
  greeks: GreeksView;
  pnl: PnlView;
  pred?: PredView;   // batch-6.4 prediction accuracy
  risk: RiskView;
  perf: PerfView;
  expiries: ExpiryView[];
  orders: OrderView[];
  hedgeDeltaThreshold?: number;   // batch-5.1 hedge-panel signal threshold
  meta: { contracts: Record<string, { mult: number | null; priceTick: number | null }> };
  params?: {
    wgt_vegaflow: number;
    wgt_frontfut_skew: number;
    wgt_deltaflow: number;
    wgt_atmsig: number;
    wgt_rollema: number;
    wgt_exp_ddelta?: number;
    max_tps: number;
  } | null;
}

export interface OrderEvent {
  type: 'order';
  seq: number;
  ts: number;
  id: number;
  code: string;
  buy: boolean;
  qty: number;
  left: number;
  px: number;
  canceled: boolean;
  tag?: number;
}

export interface TradeEvent {
  type: 'trade';
  seq: number;
  ts: number;
  id: number;
  code: string;
  buy: boolean;
  qty: number;
  px: number;
  tag?: number;
}

export interface RejectEvent {
  type: 'reject';
  seq: number;
  ts: number;
  id: number;
  ok: false;
}

export interface AckEvent {
  type: 'ack';
  cmdSeq: number;
  ok: boolean;
  err: string;
}

export interface AlertEvent {
  type: 'alert';
  seq: number;
  ts: number;
  level: number;   // 0=info 1=warn 2=error
  msg: string;
}

export interface FillsMessage {
  type: 'fills';
  sid: string;
  seq: number;
  items: FillView[];
}

export interface ConnStateMessage {
  type: 'conn_state';
  status: string;
  url?: string;
}

export interface HelloMessage {
  type: 'hello';
  proto: number;
  sid: string;
  readonly: boolean;
}

export interface FillCtxView {
  theo: number | null;
  mid: number | null;
  iv: number | null;
  delta: number | null;
  gamma: number | null;
  vega: number | null;
  theta: number | null;
}

export interface FillView {
  code: string;
  buy: boolean;
  px: number;
  qty: number;
  t: number;
  tag?: number;   // OrderTag of the parent order (batch-4.3)
  ctx?: FillCtxView;   // batch-6.3 issue-time greeks (fillAttribution on)
}

export type Frame =
  | Snapshot
  | OrderEvent
  | TradeEvent
  | RejectEvent
  | AckEvent
  | FillsMessage
  | AlertEvent
  | ConnStateMessage
  | HelloMessage
  | { type: string;[k: string]: any };
