import React, { useEffect } from 'react';

interface ToastProps {
    message: string;
    type: 'info' | 'success' | 'error';
    onClose: () => void;
    duration?: number;
}

export const Toast: React.FC<ToastProps> = ({ message, type, onClose, duration = 3000 }) => {
    useEffect(() => {
        const timer = setTimeout(() => {
            onClose();
        }, duration);
        return () => clearTimeout(timer);
    }, [duration, onClose]);

    const bgColors = {
        info: 'bg-blue-600',
        success: 'bg-green-600',
        error: 'bg-red-600'
    };

    return (
        <div className={`${bgColors[type]} text-white px-4 py-2 rounded shadow-lg mb-2 flex items-center justify-between min-w-[200px] animate-fade-in-up`}>
            <span>{message}</span>
            <button onClick={onClose} className="ml-4 font-bold hover:text-gray-200">×</button>
        </div>
    );
};
