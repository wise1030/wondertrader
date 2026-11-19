// P1 (2026-11-19) batch-2: WebAudio beep alerts — trade fill & panic.
// No asset files: short oscillator blips. Rate-limited and suppressed while
// the tab is hidden so a fill storm can not spam audio.
let ctx: AudioContext | null = null;
let lastPlay = 0;

function beep(freq: number, durationMs: number, volume: number, type: OscillatorType = 'sine') {
    try {
        if (document.hidden) return;
        const now = Date.now();
        if (now - lastPlay < 1500) return;      // rate limit
        lastPlay = now;
        if (!ctx) ctx = new AudioContext();
        const osc = ctx.createOscillator();
        const gain = ctx.createGain();
        osc.type = type;
        osc.frequency.value = freq;
        gain.gain.setValueAtTime(volume, ctx.currentTime);
        gain.gain.exponentialRampToValueAtTime(0.0001, ctx.currentTime + durationMs / 1000);
        osc.connect(gain).connect(ctx.destination);
        osc.start();
        osc.stop(ctx.currentTime + durationMs / 1000);
    } catch { /* audio is best-effort */ }
}

export const sound = {
    trade: () => beep(880, 120, 0.05),
    panic: () => { beep(220, 400, 0.12, 'square'); setTimeout(() => beep(180, 500, 0.12, 'square'), 250); },
};
