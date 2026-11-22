import React, { useEffect, useState } from 'react';

// P1 UI redesign: two-tap confirmation for dangerous actions (STOP,
// cancel_all, per-expiry OFF, qmode off/close).  First tap arms the button
// (red, shows confirmLabel); a second tap within the window fires.  No
// modal — the flow stays on one line.  PANIC is intentionally NOT confirmed
// (kill switch must be zero-friction).
export const ConfirmButton: React.FC<{
  label: string;
  confirmLabel?: string;
  onConfirm: () => void;
  disabled?: boolean;
  title?: string;
  className?: string;
  window?: number; // ms, default 3000
}> = ({ label, confirmLabel = 'confirm?', onConfirm, disabled, title, className = '', window: ms = 3000 }) => {
  const [armed, setArmed] = useState(false);
  useEffect(() => {
    if (!armed || ms <= 0) return;
    const t = setTimeout(() => setArmed(false), ms);
    return () => clearTimeout(t);
  }, [armed, ms]);
  // Audit 11-22: an armed button must not survive a label/disabled change
  // (e.g. arm "quoting OFF" on month A, select month B, click again — the
  // confirm used to apply to the NEW target while showing A's intent).
  useEffect(() => { setArmed(false); }, [label, disabled]);
  // window <= 0 means "no confirmation" (recovery-class actions) — a
  // single tap fires directly.  (Bug found on review: window=0 used to
  // schedule a zero-timeout reset, so the arm state vanished before the
  // second tap and the button could never fire.)
  const onClick = ms <= 0
    ? onConfirm
    : () => { if (armed) { setArmed(false); onConfirm(); } else setArmed(true); };
  // P1 fix: REPLACE the class list when armed — appending
  // 'text-white' after a caller-supplied 'text-gray-700' does NOT win:
  // Tailwind resolves same-specificity utilities by its own generation
  // order (gray-700 > white), so the armed state rendered near-invisible
  // grey text on the red background.
  return (
    <button
      disabled={disabled}
      title={title}
      onClick={onClick}
      className={armed ? 'bg-red-600 text-white border-red-600 font-bold px-1.5 py-0.5 rounded text-[10px] border' : className}>
      {armed ? confirmLabel : label}
    </button>
  );
};
