import React, { useCallback, useEffect, useState } from 'react';
import { DataProvider, useData } from './context/DataContext';
import { StatusBanner } from './components/StatusBanner';
import { GreeksPnlBar } from './components/GreeksPnlBar';
import { HealthPanel } from './components/HealthPanel';
import { ExpirySelector } from './components/ExpirySelector';
import { ExpirySummary } from './components/ExpirySummary';
import { UnderlierView } from './components/UnderlierView';
import { OptionGridView } from './components/OptionGridView';
import { TradesTab } from './components/TradesTab';
import { AnalysisTab } from './components/AnalysisTab';
import { CommandPanel } from './components/CommandPanel';
import { Toast } from './components/Toast';
import { ErrorBoundary } from './components/ErrorBoundary';

interface ToastItem { id: number; message: string; type: 'info' | 'success' | 'error'; }

const Shell: React.FC = () => {
  const { snapshot } = useData();
  const [selectedExpiry, setSelectedExpiry] = useState<number | null>(null);
  const [tab, setTab] = useState<'grid' | 'trades' | 'analysis'>('grid');
  const [toasts, setToasts] = useState<ToastItem[]>([]);

  const notify = useCallback((message: string, type: 'info' | 'error') => {
    const id = Date.now() + Math.random();
    setToasts(t => [...t, { id, message, type }]);
    setTimeout(() => setToasts(t => t.filter(x => x.id !== id)), 3000);
  }, []);

  // default selection: front month
  useEffect(() => {
    if (selectedExpiry === null && snapshot && snapshot.expiries.length > 0) {
      setSelectedExpiry(snapshot.expiries[0].expiry);
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
        <HealthPanel />
        <div className="flex gap-1 items-center flex-wrap">
          <ExpirySelector selected={selectedExpiry} onSelect={setSelectedExpiry} />
          <ExpirySummary selected={selectedExpiry} />
          <UnderlierView selected={selectedExpiry} />
        </div>
      </div>

      <div className="flex items-center gap-1 px-2 text-[11px] bg-white border-b border-gray-300">
        <button className={`px-3 py-1 ${tab === 'grid' ? 'border-b-2 border-blue-600 font-bold' : 'text-gray-500'}`} onClick={() => setTab('grid')}>Grid</button>
        <button className={`px-3 py-1 ${tab === 'trades' ? 'border-b-2 border-blue-600 font-bold' : 'text-gray-500'}`} onClick={() => setTab('trades')}>Trades</button>
        <button className={`px-3 py-1 ${tab === 'analysis' ? 'border-b-2 border-blue-600 font-bold' : 'text-gray-500'}`} onClick={() => setTab('analysis')}>Analysis</button>
      </div>

      <div className="flex-1 overflow-hidden">
        {tab === 'grid'
          ? <OptionGridView selectedExpiry={selectedExpiry} onExpirySelectByIndex={selectByIndex} onNotify={notify} />
          : tab === 'trades' ? <TradesTab /> : <AnalysisTab selectedExpiry={selectedExpiry} />}
      </div>

      <CommandPanel onNotify={notify} />

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
