import React, { useCallback, useEffect, useState } from 'react';
import { DataProvider, useData } from './context/DataContext';
import { StatusBanner } from './components/StatusBanner';
import { GreeksPnlBar } from './components/GreeksPnlBar';
import { StatusFooter } from './components/StatusFooter';
import { ExpiryBoard } from './components/ExpiryBoard';
import { UnderlierCard } from './components/UnderlierCard';
import { ExpirySummary } from './components/ExpirySummary';
import { OptionGridView } from './components/OptionGridView';
import { TradesTab } from './components/TradesTab';
import { AnalysisTab } from './components/AnalysisTab';
import { CommandPanel } from './components/CommandPanel';
import { Toast } from './components/Toast';
import { ErrorBoundary } from './components/ErrorBoundary';

interface ToastItem { id: number; message: string; type: 'info' | 'success' | 'error'; }

const Shell: React.FC = () => {
  const { snapshot, lastAlert, lastAckError } = useData();
  const [selectedExpiry, setSelectedExpiry] = useState<number | null>(null);
  const [tab, setTab] = useState<'grid' | 'trades' | 'analysis'>('grid');
  const [toasts, setToasts] = useState<ToastItem[]>([]);

  const notify = useCallback((message: string, type: 'info' | 'error') => {
    const id = Date.now() + Math.random();
    setToasts(t => [...t, { id, message, type }]);
    setTimeout(() => setToasts(t => t.filter(x => x.id !== id)), 3000);
  }, []);

  // P1 batch-5.5: graded alerts toast (order anomalies, risk events pushed
  // through the UiBridge alert frame; strategy-side 2s rate limit).
  useEffect(() => {
    if (!lastAlert) return;
    notify(lastAlert.msg, lastAlert.level >= 2 ? 'error' : 'info');
  }, [lastAlert, notify]);

  // Audit 11-22: surface rejected commands as a toast, not just a log line.
  // (The entry form used to say "sent" even when the bridge ack was ok=false.)
  useEffect(() => {
    if (!lastAckError) return;
    notify(`command rejected: ${lastAckError.msg}`, 'error');
  }, [lastAckError, notify]);

  // default selection: first QUOTING-enabled expiry (a disabled series would
  // show an empty grid); fall back to the front month.
  // Audit 11-22: also re-validate on every new snapshot — after a session
  // rollover the selected expiry can disappear, leaving BOARD/Grid in a
  // permanent "select an expiry" state even though data is flowing.
  useEffect(() => {
    if (!snapshot || snapshot.expiries.length === 0) return;
    const stillThere = selectedExpiry != null
      && snapshot.expiries.some(e => e.expiry === selectedExpiry);
    if (!stillThere) {
      const first = snapshot.expiries.find(e => e.quoting !== false) ?? snapshot.expiries[0];
      setSelectedExpiry(first.expiry);
    }
  }, [snapshot, selectedExpiry]);

  const selectByIndex = useCallback((i: number) => {
    const e = snapshot?.expiries[i];
    if (e) setSelectedExpiry(e.expiry);
  }, [snapshot]);

  return (
    <div className="h-screen flex flex-col bg-gray-100 text-gray-900">
      <div className="flex flex-col gap-1 p-1">
        <StatusBanner />
        <div className="flex gap-1">
          <div className="flex-1"><GreeksPnlBar /></div>
        </div>
        <ExpiryBoard selected={selectedExpiry} onSelect={setSelectedExpiry} onNotify={notify} />
        <div className="flex gap-1 items-center flex-wrap">
          <ExpirySummary selected={selectedExpiry} />
          <UnderlierCard selected={selectedExpiry} />
        </div>
      </div>

      <div className="flex items-center gap-1 px-2 text-[11px] bg-white border-b border-gray-300">
        <button className={`px-3 py-1 ${tab === 'grid' ? 'border-b-2 border-blue-600 font-bold' : 'text-gray-500'}`} onClick={() => setTab('grid')}>Grid</button>
        <button className={`px-3 py-1 ${tab === 'trades' ? 'border-b-2 border-blue-600 font-bold' : 'text-gray-500'}`} onClick={() => setTab('trades')}>Trades</button>
        <button className={`px-3 py-1 ${tab === 'analysis' ? 'border-b-2 border-blue-600 font-bold' : 'text-gray-500'}`} onClick={() => setTab('analysis')}>Analysis</button>
      </div>

      {/* Audit 11-22: keep the three tab bodies MOUNTED and hide the
          inactive ones.  Before this, switching tabs unmounted the Grid
          (losing row selection/scroll and its keyboard context), the Trades
          filter and the Analysis PnL/latency rings (which restart from zero
          on every visit).  The render cost is one hidden tree at 1Hz. */}
      <div className="flex-1 overflow-hidden">
        <div className={tab === 'grid' ? 'h-full' : 'hidden'}>
          <OptionGridView selectedExpiry={selectedExpiry} onExpirySelectByIndex={selectByIndex}
            onNotify={notify} active={tab === 'grid'} />
        </div>
        <div className={tab === 'trades' ? 'h-full' : 'hidden'}>
          <TradesTab onNotify={notify} />
        </div>
        <div className={tab === 'analysis' ? 'h-full' : 'hidden'}>
          <AnalysisTab selectedExpiry={selectedExpiry} />
        </div>
      </div>

      <CommandPanel onNotify={notify} selectedExpiry={selectedExpiry} />
      <StatusFooter />

      <div className="fixed bottom-2 right-2 z-50 flex flex-col">
        {toasts.map(t => <Toast key={t.id} message={t.message} type={t.type} onClose={() => setToasts(x => x.filter(y => y.id !== t.id))} />)}
      </div>
    </div>
  );
};

const App: React.FC = () => (
  <ErrorBoundary>
    <DataProvider>
      <Shell />
    </DataProvider>
  </ErrorBoundary>
);

export default App;
