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
  // window <= 0 means "no confirmation" (recovery-class actions) — a
  // single tap fires directly.  (Bug found on review: window=0 used to
  // schedule a zero-timeout reset, so the arm state vanished before the
  // second tap and the button could never fire.)
  const onClick = ms <= 0
    ? onConfirm
    : () => { if (armed) { setArmed(false); onConfirm(); } else setArmed(true); };
  return (
    <button
      disabled={disabled}
      title={title}
      onClick={onClick}
      className={`${className} ${armed ? 'bg-red-600 text-white border-red-600 font-bold' : ''}`}>
      {armed ? confirmLabel : label}
    </button>
  );
};
