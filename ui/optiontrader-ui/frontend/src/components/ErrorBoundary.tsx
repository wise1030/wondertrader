import { Component, type ErrorInfo, type ReactNode } from 'react';

interface Props {
    children?: ReactNode;
}

interface State {
    hasError: boolean;
    error: Error | null;
    errorInfo: ErrorInfo | null;
}

export class ErrorBoundary extends Component<Props, State> {
    public state: State = {
        hasError: false,
        error: null,
        errorInfo: null
    };

    public static getDerivedStateFromError(error: Error): State {
        return { hasError: true, error, errorInfo: null };
    }

    public componentDidCatch(error: Error, errorInfo: ErrorInfo) {
        console.error('Uncaught error:', error, errorInfo);
        this.setState({ errorInfo });
    }

    public render() {
        if (this.state.hasError) {
            return (
                <div className="flex flex-col items-center justify-center h-screen bg-gray-900 text-white p-8 font-sans">
                    <div className="bg-red-900/20 border border-red-500 p-8 rounded-lg max-w-2xl w-full shadow-2xl">
                        <h1 className="text-3xl font-bold mb-4 text-red-400 flex items-center gap-2">
                            <span>⚠️</span> UI Runtime Error
                        </h1>
                        <p className="text-gray-300 mb-6">
                            The application encountered an unexpected error and had to stop.
                            This is usually caused by high-frequency data updates or a component failure.
                        </p>

                        <div className="bg-black/50 p-4 rounded border border-gray-700 mb-6 overflow-auto max-h-60">
                            <p className="text-red-300 font-mono text-sm mb-2 font-bold">
                                {this.state.error?.toString()}
                            </p>
                            <pre className="text-gray-400 font-mono text-xs whitespace-pre-wrap">
                                {this.state.errorInfo?.componentStack}
                            </pre>
                        </div>

                        <div className="flex gap-4">
                            <button
                                onClick={() => window.location.reload()}
                                className="px-6 py-2 bg-blue-600 hover:bg-blue-500 text-white rounded font-bold transition-colors"
                            >
                                Reload Application
                            </button>
                            <button
                                onClick={() => this.setState({ hasError: false, error: null, errorInfo: null })}
                                className="px-6 py-2 bg-gray-700 hover:bg-gray-600 text-white rounded font-bold transition-colors"
                            >
                                Try to Recover
                            </button>
                        </div>
                    </div>
                </div>
            );
        }

        return this.props.children;
    }
}
