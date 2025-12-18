import React, { useEffect, useRef } from 'react';
import { MessageItem } from '../types';

interface Props {
  msg: MessageItem;
  configDisappearTime: number;
}

export const MessageBubble: React.FC<Props> = ({ msg, configDisappearTime }) => {
  const isMatched = !!msg.matchedId;
  const elementRef = useRef<HTMLDivElement>(null);

  // Time formatting
  const date = new Date(msg.timestamp);
  const timeStr = date.toLocaleTimeString('en-US', {
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }) + '.' + date.getMilliseconds().toString().padStart(3, '0');

  return (
    <div
      ref={elementRef}
      id={`msg-${msg.id}`}
      className={`
        relative mb-2 p-3 rounded-lg border-2 w-full max-w-sm transition-all duration-500
        flex flex-col gap-1 text-sm font-mono shadow-md
        ${isMatched 
          ? 'border-green-400 bg-green-900/20 shadow-[0_0_10px_rgba(74,222,128,0.3)]' 
          : msg.type === 'sent' 
            ? 'border-blue-500/30 bg-blue-900/10' 
            : 'border-orange-500/30 bg-orange-900/10'
        }
        ${msg.isExpired ? 'opacity-0 scale-95' : 'opacity-100 scale-100'}
      `}
      style={{
        // If matched, we start the fade out timer.
        transitionDuration: msg.isExpired ? '0.5s' : '0.2s',
      }}
    >
      <div className="flex justify-between text-xs opacity-60">
        <span>{msg.type.toUpperCase()}</span>
        <span>{timeStr}</span>
      </div>
      <div className="break-all font-bold text-white max-h-32 overflow-y-auto no-scrollbar">
        {msg.content}
      </div>
      {/* Indicator dot for connection point */}
      <div 
        className={`absolute top-1/2 -translate-y-1/2 w-2 h-2 rounded-full bg-current 
          ${msg.type === 'sent' ? '-right-1.5 text-blue-500' : '-left-1.5 text-orange-500'}
          ${isMatched ? 'text-green-400' : ''}
        `}
      />
    </div>
  );
};