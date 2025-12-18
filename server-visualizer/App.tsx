
import React, { useState, useEffect, useRef, useCallback } from 'react';
import { Play, Square, Settings, Wifi, WifiOff, RefreshCw } from 'lucide-react';
import { AppConfig, MessageItem, Stats, ProtocolHandler } from './types';
import { DEFAULT_PROTOCOL_CODE, compileProtocol } from './utils/protocol';
import { MessageBubble } from './components/MessageBubble';
import { SettingsModal } from './components/SettingsModal';

// Simple UUID generator
const generateId = () => Math.random().toString(36).substring(2, 15) + Math.random().toString(36).substring(2, 15);

const MAX_QUEUE_SIZE = 1000; 

export default function App() {
  // --- State ---
  const [config, setConfig] = useState<AppConfig>({
    sendRate: 1,
    disappearTime: 2,
    isRunning: false,
    proxyAddress: 'localhost:8081',
    serverHost: 'localhost',
    serverPort: 8080,
    protocolCode: DEFAULT_PROTOCOL_CODE
  });

  const [isSettingsOpen, setIsSettingsOpen] = useState(false);
  const [stats, setStats] = useState<Stats>({ sent: 0, received: 0, matched: 0 });
  const [isConnected, setIsConnected] = useState(false);
  const [sentMessages, setSentMessages] = useState<MessageItem[]>([]);
  const [receivedMessages, setReceivedMessages] = useState<MessageItem[]>([]);
  const [lines, setLines] = useState<{ x1: number, y1: number, x2: number, y2: number, key: string }[]>([]);

  // --- Refs ---
  const socketRef = useRef<WebSocket | null>(null);
  const bufferRef = useRef<string>(""); 
  const sentQueueRef = useRef<MessageItem[]>([]);
  const receivedQueueRef = useRef<MessageItem[]>([]);
  const intervalRef = useRef<number | null>(null);
  const containerRef = useRef<HTMLDivElement>(null);
  const configRef = useRef(config);

  useEffect(() => {
    configRef.current = config;
  }, [config]);
  
  // Dynamic Protocol Handler Ref
  const protocolRef = useRef<ProtocolHandler | null>(null);

  // Initialize Protocol Handler
  useEffect(() => {
    try {
      protocolRef.current = compileProtocol(config.protocolCode);
    } catch (e) {
      console.error("Critical error compiling protocol", e);
    }
  }, [config.protocolCode]);

  // --- Socket Connection ---
  useEffect(() => {
    const connect = () => {
      // Build URL from config
      // Use proxyAddress and pass target host/port as query params
      const url = `ws://${config.proxyAddress}/?host=${config.serverHost}&port=${config.serverPort}`;
      
      try {
        const ws = new WebSocket(url);
        
        ws.onopen = () => {
          setIsConnected(true);
          console.log("Connected to " + url);
        };

        ws.onclose = () => {
          setIsConnected(false);
          console.log("Disconnected");
        };

        ws.onerror = (err) => {
          console.error("Socket error", err);
          setIsConnected(false);
        };

        ws.onmessage = async (event) => {
          const text = await event.data.text();
          handleIncomingData(text);
        };

        socketRef.current = ws;
      } catch (e) {
        console.error("Connection failed", e);
      }
    };

    connect();

    return () => {
      if (socketRef.current) {
        socketRef.current.close();
      }
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [config.proxyAddress, config.serverHost, config.serverPort]);

  // --- Protocol Logic ---
  
  const handleIncomingData = (chunk: string) => {
    if (!protocolRef.current) return;

    bufferRef.current += chunk;

    // Try to decode as many messages as possible from the buffer
    let loopGuard = 0;
    while (true && loopGuard < 100) {
      loopGuard++;
      let decoded;
      try {
         decoded = protocolRef.current.tryDecode(bufferRef.current);
      } catch (e) {
         console.error("Decode error:", e);
         // If error, maybe clear buffer or skip byte? For now clear to avoid infinite loop
         bufferRef.current = "";
         break;
      }

      if (!decoded) break; // Incomplete data, wait for more

      // Remove consumed data from buffer
      bufferRef.current = bufferRef.current.substring(decoded.length);

      handleNewMessage(decoded.content);
    }
  };

  const handleNewMessage = (content: string) => {
    const newMsg: MessageItem = {
      id: generateId(),
      timestamp: Date.now(),
      content,
      type: 'received',
    };

    // Update Received Queue
    const newQueue = [newMsg, ...receivedQueueRef.current].slice(0, MAX_QUEUE_SIZE);
    receivedQueueRef.current = newQueue;
    setReceivedMessages(newQueue);
    
    setStats(prev => ({ ...prev, received: prev.received + 1 }));

    // Try to match
    checkForMatch(newMsg);
  };

  const checkForMatch = (receivedMsg: MessageItem) => {
    if (!protocolRef.current) return;

    const sentQueue = sentQueueRef.current;
    
    // We iterate backwards (most recent first) to find the likely pair
    for (let i = 0; i < sentQueue.length; i++) {
      const sentMsg = sentQueue[i];
      if (sentMsg.matchedId) continue; // Already matched

      try {
        const expected = protocolRef.current.getExpectedResponse(sentMsg.content);
        if (receivedMsg.content === expected) {
          matchPair(sentMsg, receivedMsg);
          break; 
        }
      } catch (e) {
        console.error("Matcher error", e);
      }
    }
  };

  const matchPair = (sentMsg: MessageItem, receivedMsg: MessageItem) => {
    const updateQueue = (
      queueRef: React.MutableRefObject<MessageItem[]>, 
      setQueue: React.Dispatch<React.SetStateAction<MessageItem[]>>, 
      id: string, 
      updates: Partial<MessageItem>
    ) => {
      const idx = queueRef.current.findIndex(m => m.id === id);
      if (idx !== -1) {
        queueRef.current[idx] = { ...queueRef.current[idx], ...updates };
        setQueue([...queueRef.current]);
      }
    };

    updateQueue(sentQueueRef, setSentMessages, sentMsg.id, { matchedId: receivedMsg.id });
    updateQueue(receivedQueueRef, setReceivedMessages, receivedMsg.id, { matchedId: sentMsg.id });

    setStats(prev => ({ ...prev, matched: prev.matched + 1 }));

    const currentDisappearTime = configRef.current.disappearTime;
    if (currentDisappearTime > 0) {
        setTimeout(() => {
          expireMessage(sentMsg.id, 'sent');
          expireMessage(receivedMsg.id, 'received');
        }, currentDisappearTime * 1000);
    }
  };

  const expireMessage = (id: string, type: 'sent' | 'received') => {
    const isSent = type === 'sent';
    const queueRef = isSent ? sentQueueRef : receivedQueueRef;
    const setQueue = isSent ? setSentMessages : setReceivedMessages;

    const idx = queueRef.current.findIndex(m => m.id === id);
    if (idx !== -1) {
      queueRef.current[idx] = { ...queueRef.current[idx], isExpired: true };
      setQueue([...queueRef.current]);
    }

    setTimeout(() => {
      queueRef.current = queueRef.current.filter(m => m.id !== id);
      setQueue([...queueRef.current]);
    }, 600);
  };

  // --- Sending Logic ---

  const sendMessage = useCallback(() => {
    if (!socketRef.current || socketRef.current.readyState !== WebSocket.OPEN || !protocolRef.current) return;

    // Generate content using user defined logic
    let content = "";
    try {
        content = protocolRef.current.generatePayload();
    } catch (e) {
        console.error("Payload generation error", e);
        return;
    }

    try {
      const encoded = protocolRef.current.encode(content);
      socketRef.current.send(encoded);

      const newMsg: MessageItem = {
        id: generateId(),
        timestamp: Date.now(),
        content,
        type: 'sent'
      };

      const newQueue = [newMsg, ...sentQueueRef.current].slice(0, MAX_QUEUE_SIZE);
      sentQueueRef.current = newQueue;
      setSentMessages(newQueue);
      
      setStats(prev => ({ ...prev, sent: prev.sent + 1 }));
    } catch (e) {
      console.error("Encode error", e);
    }
  }, []);

  // --- Loop Control ---
  useEffect(() => {
    if (config.isRunning && isConnected) {
      const intervalMs = 1000 / config.sendRate;
      intervalRef.current = window.setInterval(sendMessage, intervalMs);
    } else {
      if (intervalRef.current) clearInterval(intervalRef.current);
    }

    return () => {
      if (intervalRef.current) clearInterval(intervalRef.current);
    };
  }, [config.isRunning, config.sendRate, isConnected, sendMessage]);

  // --- Line Drawing Loop ---
  useEffect(() => {
    let rAF: number;
    const updateLines = () => {
      const newLines: typeof lines = [];
      const containerRect = containerRef.current?.getBoundingClientRect();

      if (!containerRect) return;

      sentMessages.forEach(sMsg => {
        if (sMsg.matchedId && !sMsg.isExpired) {
          const startEl = document.getElementById(`msg-${sMsg.id}`);
          const endEl = document.getElementById(`msg-${sMsg.matchedId}`);

          if (startEl && endEl) {
            const startRect = startEl.getBoundingClientRect();
            const endRect = endEl.getBoundingClientRect();

            newLines.push({
              key: sMsg.id,
              x1: startRect.right - containerRect.left,
              y1: startRect.top + (startRect.height / 2) - containerRect.top,
              x2: endRect.left - containerRect.left,
              y2: endRect.top + (endRect.height / 2) - containerRect.top
            });
          }
        }
      });
      setLines(newLines);
      rAF = requestAnimationFrame(updateLines);
    };

    if (config.isRunning || sentMessages.length > 0) {
      rAF = requestAnimationFrame(updateLines);
    }
    return () => cancelAnimationFrame(rAF);
  }, [sentMessages, receivedMessages, config.isRunning]);


  // --- Render ---

  const matchRate = stats.sent > 0 ? ((stats.matched / stats.sent) * 100).toFixed(1) : "0.0";
  const serverUrl = `ws://${config.serverHost}:${config.serverPort}`;

  return (
    <div className="flex flex-col h-screen bg-gray-900 text-gray-200 overflow-hidden font-sans">
      
      <SettingsModal 
        isOpen={isSettingsOpen} 
        onClose={() => setIsSettingsOpen(false)}
        config={config}
        onSave={(newCfg) => setConfig(prev => ({...prev, ...newCfg}))}
      />

      {/* Top Bar */}
      <header className="flex items-center justify-between px-6 py-4 bg-gray-800 border-b border-gray-700 shadow-md z-20">
        <div className="flex items-center gap-3">
          <div className={`p-2 rounded-full ${isConnected ? 'bg-green-500/20 text-green-400' : 'bg-red-500/20 text-red-400'}`}>
            {isConnected ? <Wifi size={20} /> : <WifiOff size={20} />}
          </div>
          <div>
            <h1 className="font-bold text-lg text-white tracking-wide">Server Visualizer</h1>
            <p className="text-xs text-gray-400 font-mono">{serverUrl}</p>
          </div>
        </div>

        <div className="flex gap-6 text-sm font-mono">
          <div className="flex flex-col items-center">
            <span className="text-gray-500 uppercase text-[10px]">Sent</span>
            <span className="text-blue-400 font-bold text-xl">{stats.sent}</span>
          </div>
          <div className="flex flex-col items-center">
            <span className="text-gray-500 uppercase text-[10px]">Received</span>
            <span className="text-orange-400 font-bold text-xl">{stats.received}</span>
          </div>
          <div className="flex flex-col items-center">
            <span className="text-gray-500 uppercase text-[10px]">Matched</span>
            <span className="text-green-400 font-bold text-xl">{stats.matched}</span>
          </div>
          <div className="flex flex-col items-center">
            <span className="text-gray-500 uppercase text-[10px]">Rate</span>
            <span className="text-white font-bold text-xl">{matchRate}%</span>
          </div>
        </div>

        <div className="flex gap-2">
          <button 
            onClick={() => window.location.reload()}
            className="p-2 hover:bg-gray-700 rounded-full transition"
            title="Restart / Refresh"
          >
            <RefreshCw size={20} className="text-gray-400" />
          </button>
          <button 
            onClick={() => setIsSettingsOpen(true)}
            className="p-2 hover:bg-gray-700 rounded-full transition"
            title="Settings"
          >
            <Settings size={20} className="text-gray-400" />
          </button>
        </div>
      </header>

      {/* Main Content */}
      <main ref={containerRef} className="flex-1 relative flex overflow-hidden">
        
        {/* SVG Layer */}
        <svg className="absolute inset-0 w-full h-full pointer-events-none z-10">
          {lines.map(line => (
            <path
              key={line.key}
              d={`M ${line.x1} ${line.y1} C ${(line.x1 + line.x2) / 2} ${line.y1}, ${(line.x1 + line.x2) / 2} ${line.y2}, ${line.x2} ${line.y2}`}
              stroke="rgba(74, 222, 128, 0.5)"
              strokeWidth="2"
              fill="none"
            />
          ))}
        </svg>

        {/* Left Column: Sent */}
        <div className="flex-1 border-r border-gray-800 bg-gray-900/50 flex flex-col items-end p-4 relative z-0">
           <h2 className="absolute top-4 right-4 text-blue-500/50 font-bold text-4xl select-none pointer-events-none uppercase">Tx (Sent)</h2>
           <div className="w-full h-full overflow-y-auto flex flex-col items-end pr-8 no-scrollbar pt-20">
             {sentMessages.map((msg) => (
               <MessageBubble key={msg.id} msg={msg} configDisappearTime={config.disappearTime} />
             ))}
           </div>
        </div>

        {/* Right Column: Received */}
        <div className="flex-1 bg-gray-900/50 flex flex-col items-start p-4 relative z-0">
          <h2 className="absolute top-4 left-4 text-orange-500/50 font-bold text-4xl select-none pointer-events-none uppercase">Rx (Recv)</h2>
          <div className="w-full h-full overflow-y-auto flex flex-col items-start pl-8 no-scrollbar pt-20">
            {receivedMessages.map((msg) => (
              <MessageBubble key={msg.id} msg={msg} configDisappearTime={config.disappearTime} />
            ))}
          </div>
        </div>
      </main>

      {/* Bottom Bar: Controls */}
      <footer className="h-20 bg-gray-800 border-t border-gray-700 flex items-center justify-center gap-8 px-6 z-20">
        
        {/* Start/Stop Button */}
        <button
          onClick={() => setConfig(prev => ({ ...prev, isRunning: !prev.isRunning }))}
          className={`flex items-center gap-2 px-6 py-3 rounded-md font-bold transition-all ${
            config.isRunning 
              ? 'bg-red-500/10 text-red-500 hover:bg-red-500/20 ring-1 ring-red-500' 
              : 'bg-green-500 text-black hover:bg-green-400'
          }`}
        >
          {config.isRunning ? <><Square size={18} fill="currentColor" /> Stop</> : <><Play size={18} fill="currentColor" /> Start</>}
        </button>

        <div className="h-8 w-px bg-gray-600 mx-2"></div>

        {/* Rate Control */}
        <div className="flex flex-col gap-1 w-48">
          <div className="flex justify-between text-xs text-gray-400">
            <span>Send Rate</span>
            <span className="text-white font-mono">{config.sendRate} /s</span>
          </div>
          <input
            type="range"
            min="1"
            max="100"
            step="1"
            value={config.sendRate}
            onChange={(e) => setConfig(prev => ({ ...prev, sendRate: parseFloat(e.target.value) }))}
            className="w-full h-2 bg-gray-700 rounded-lg appearance-none cursor-pointer accent-blue-500"
          />
        </div>

        {/* Disappear Time Control */}
        <div className="flex flex-col gap-1 w-48">
          <div className="flex justify-between text-xs text-gray-400">
            <span>Disappear Time</span>
            <span className="text-white font-mono">
                {config.disappearTime === -1 ? 'Never' : `${config.disappearTime}s`}
            </span>
          </div>
          <div className="flex items-center gap-2">
              <input
                type="range"
                min="1"
                max="10"
                step="1"
                disabled={config.disappearTime === -1}
                value={config.disappearTime === -1 ? 5 : config.disappearTime}
                onChange={(e) => setConfig(prev => ({ ...prev, disappearTime: parseFloat(e.target.value) }))}
                className={`w-full h-2 bg-gray-700 rounded-lg appearance-none cursor-pointer accent-purple-500 ${config.disappearTime === -1 ? 'opacity-50' : ''}`}
              />
              <button 
                  onClick={() => setConfig(prev => ({ ...prev, disappearTime: prev.disappearTime === -1 ? 2 : -1 }))}
                  className={`text-xs px-2 py-1 rounded border transition-colors ${config.disappearTime === -1 ? 'bg-purple-500 border-purple-500 text-white' : 'border-gray-600 text-gray-400 hover:text-white'}`}
              >
                  Never
              </button>
          </div>
        </div>
      </footer>
    </div>
  );
}
