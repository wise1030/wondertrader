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
    if (!armed) return;
    const t = setTimeout(() => setArmed(false), ms);
    return () => clearTimeout(t);
  }, [armed, ms]);
  return (
    <button
      disabled={disabled}
      title={title}
      onClick={() => { if (armed) { setArmed(false); onConfirm(); } else setArmed(true); }}
      className={`${className} ${armed ? 'bg-red-600 text-white border-red-600 font-bold' : ''}`}>
      {armed ? confirmLabel : label}
    </button>
  );
};
