import { useEffect, useState } from 'react';

/**
 * Wall-clock ticker for client-side staleness math (audit batch B, H1):
 * re-renders every `intervalMs` so components can recompute the live
 * snapshot age between frames.
 */
export function useNowMs(intervalMs = 1000): number {
    const [now, setNow] = useState(() => Date.now());
    useEffect(() => {
        const t = setInterval(() => setNow(Date.now()), intervalMs);
        return () => clearInterval(t);
    }, [intervalMs]);
    return now;
}

/**
 * Live snapshot age = (middleware-injected forward-time age, clock-skew-free)
 * + (local wall time elapsed since the frame arrived). Never negative.
 */
export function liveAgeMs(
    snapshotAgeMs: number | null | undefined,
    arrivalMs: number,
    nowMs: number,
): number | null {
    if (snapshotAgeMs == null) return null;
    return snapshotAgeMs + Math.max(0, nowMs - arrivalMs);
}
