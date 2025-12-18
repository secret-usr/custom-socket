
import React, { useState, useEffect } from 'react';
import { X, Save, FileCode, CheckCircle, AlertCircle } from 'lucide-react';
import { AppConfig, ProtocolCode } from '../types';
import { DEFAULT_PROTOCOL_CODE, compileProtocol } from '../utils/protocol';

interface Props {
  isOpen: boolean;
  onClose: () => void;
  config: AppConfig;
  onSave: (newConfig: Partial<AppConfig>) => void;
}

export const SettingsModal: React.FC<Props> = ({ isOpen, onClose, config, onSave }) => {
  const [activeTab, setActiveTab] = useState<'network' | 'protocol'>('network');
  const [proxyAddr, setProxyAddr] = useState(config.proxyAddress);
  const [serverAddress, setServerAddress] = useState(`${config.serverHost}:${config.serverPort}`);
  
  const [protocolCode, setProtocolCode] = useState<ProtocolCode>(config.protocolCode);
  const [compileError, setCompileError] = useState<string | null>(null);

  useEffect(() => {
    if (isOpen) {
      setProxyAddr(config.proxyAddress);
      setServerAddress(`${config.serverHost}:${config.serverPort}`);
      setProtocolCode(config.protocolCode);
      setCompileError(null);
    }
  }, [isOpen, config]);

  const handleSave = () => {
    // Validate protocol code by trying to compile it
    try {
      compileProtocol(protocolCode);
      setCompileError(null);
    } catch (e: any) {
      setCompileError(e.message);
      return;
    }

    const [newHost, newPortStr] = serverAddress.split(':');
    const newPort = parseInt(newPortStr, 10);

    if (!newHost || isNaN(newPort)) {
        setCompileError("Invalid Server Address format. Use host:port");
        return;
    }

    onSave({
      proxyAddress: proxyAddr,
      serverHost: newHost,
      serverPort: newPort,
      protocolCode: protocolCode
    });
    onClose();
  };

  const resetToDefault = () => {
    setProtocolCode(DEFAULT_PROTOCOL_CODE);
  };

  if (!isOpen) return null;

  return (
    <div className="fixed inset-0 bg-black/80 backdrop-blur-sm z-50 flex items-center justify-center p-4">
      <div className="bg-gray-900 border border-gray-700 w-full max-w-4xl h-[85vh] rounded-xl flex flex-col shadow-2xl">
        
        {/* Header */}
        <div className="flex justify-between items-center p-4 border-b border-gray-800">
          <h2 className="text-xl font-bold text-white flex items-center gap-2">
            <FileCode className="text-blue-400" /> Configuration
          </h2>
          <button onClick={onClose} className="text-gray-400 hover:text-white transition">
            <X size={24} />
          </button>
        </div>

        {/* Content */}
        <div className="flex-1 flex overflow-hidden">
          {/* Sidebar */}
          <div className="w-48 bg-gray-950/50 border-r border-gray-800 flex flex-col pt-4 gap-1 px-2">
            <button 
              onClick={() => setActiveTab('network')}
              className={`text-left px-4 py-3 rounded-lg text-sm font-medium transition ${activeTab === 'network' ? 'bg-blue-600/20 text-blue-400 border border-blue-500/30' : 'text-gray-400 hover:bg-gray-800'}`}
            >
              Network
            </button>
            <button 
              onClick={() => setActiveTab('protocol')}
              className={`text-left px-4 py-3 rounded-lg text-sm font-medium transition ${activeTab === 'protocol' ? 'bg-blue-600/20 text-blue-400 border border-blue-500/30' : 'text-gray-400 hover:bg-gray-800'}`}
            >
              Protocol & Logic
            </button>
          </div>

          {/* Main Panel */}
          <div className="flex-1 overflow-y-auto p-6 bg-gray-900 no-scrollbar">
            
            {activeTab === 'network' && (
              <div className="space-y-6 max-w-md">
                <div>
                  <label className="block text-sm text-gray-400 mb-2">Proxy Address (WebSocket)</label>
                  <input 
                    type="text" 
                    value={proxyAddr}
                    onChange={e => setProxyAddr(e.target.value)}
                    className="w-full bg-gray-950 border border-gray-700 rounded p-3 text-white focus:border-blue-500 focus:outline-none"
                    placeholder="localhost:8081"
                  />
                  <p className="text-xs text-gray-500 mt-1">The address where 'npm run proxy' is running.</p>
                </div>

                <div className="border-t border-gray-800 pt-4">
                  <h3 className="text-sm font-semibold text-gray-300 mb-4">Target TCP Server</h3>
                  
                  <div>
                    <label className="block text-sm text-gray-400 mb-2">Server Address</label>
                    <input 
                      type="text" 
                      value={serverAddress}
                      onChange={e => setServerAddress(e.target.value)}
                      className="w-full bg-gray-950 border border-gray-700 rounded p-3 text-white focus:border-blue-500 focus:outline-none"
                      placeholder="localhost:8080"
                    />
                  </div>
                </div>
              </div>
            )}

            {activeTab === 'protocol' && (
              <div className="space-y-8">
                <div className="grid grid-cols-1 gap-6">
                  {/* Encoder */}
                  <div>
                    <div className="flex justify-between items-baseline mb-2">
                      <label className="text-sm font-bold text-green-400">1. Encoder</label>
                      <code className="text-[10px] text-gray-500 bg-gray-950 px-2 py-1 rounded">
                        (content: string) =&gt; string
                      </code>
                    </div>
                    <p className="text-xs text-gray-400 mb-2">
                      Logic to wrap the message payload into a network packet (e.g. add headers).
                    </p>
                    <textarea 
                      value={protocolCode.encoder}
                      onChange={e => setProtocolCode(p => ({...p, encoder: e.target.value}))}
                      className="w-full h-32 bg-gray-950 text-green-100 font-mono text-xs p-3 rounded border border-gray-700 focus:border-green-500 focus:outline-none leading-relaxed"
                      spellCheck={false}
                    />
                  </div>

                  {/* Decoder */}
                  <div>
                    <div className="flex justify-between items-baseline mb-2">
                      <label className="text-sm font-bold text-orange-400">2. Decoder</label>
                      <code className="text-[10px] text-gray-500 bg-gray-950 px-2 py-1 rounded">
                        (buf: string) =&gt; &#123;length, content&#125; | null
                      </code>
                    </div>
                    <p className="text-xs text-gray-400 mb-2">
                       Logic to try extracting a single complete message from the TCP buffer. Return <code>null</code> if data is incomplete.
                    </p>
                    <textarea 
                      value={protocolCode.decoder}
                      onChange={e => setProtocolCode(p => ({...p, decoder: e.target.value}))}
                      className="w-full h-40 bg-gray-950 text-orange-100 font-mono text-xs p-3 rounded border border-gray-700 focus:border-orange-500 focus:outline-none leading-relaxed"
                      spellCheck={false}
                    />
                  </div>

                  {/* Matcher */}
                  <div>
                    <div className="flex justify-between items-baseline mb-2">
                      <label className="text-sm font-bold text-purple-400">3. Business Logic (Matcher)</label>
                      <code className="text-[10px] text-gray-500 bg-gray-950 px-2 py-1 rounded">
                        (request: string) =&gt; expectedResponse
                      </code>
                    </div>
                    <p className="text-xs text-gray-400 mb-2">
                      Defines the expected response for a given sent message. Used to calculate match rate and verify server correctness.
                    </p>
                    <textarea 
                      value={protocolCode.matcher}
                      onChange={e => setProtocolCode(p => ({...p, matcher: e.target.value}))}
                      className="w-full h-40 bg-gray-950 text-purple-100 font-mono text-xs p-3 rounded border border-gray-700 focus:border-purple-500 focus:outline-none leading-relaxed"
                      spellCheck={false}
                    />
                  </div>

                  {/* Payload Generator */}
                  <div>
                    <div className="flex justify-between items-baseline mb-2">
                      <label className="text-sm font-bold text-pink-400">4. Payload Generator</label>
                      <code className="text-[10px] text-gray-500 bg-gray-950 px-2 py-1 rounded">
                        () =&gt; string
                      </code>
                    </div>
                    <p className="text-xs text-gray-400 mb-2">
                      Logic to generate the content of the messages to be sent.
                    </p>
                    <textarea 
                      value={protocolCode.payloadGenerator}
                      onChange={e => setProtocolCode(p => ({...p, payloadGenerator: e.target.value}))}
                      className="w-full h-32 bg-gray-950 text-pink-100 font-mono text-xs p-3 rounded border border-gray-700 focus:border-pink-500 focus:outline-none leading-relaxed"
                      spellCheck={false}
                    />
                  </div>

                  <div className="flex justify-end">
                     <button onClick={resetToDefault} className="text-xs text-gray-500 hover:text-white underline">Reset to Default</button>
                  </div>
                </div>
              </div>
            )}

          </div>
        </div>

        {/* Footer */}
        <div className="p-4 border-t border-gray-800 flex justify-between items-center bg-gray-900">
           <div>
             {compileError && (
               <span className="text-red-400 text-sm flex items-center gap-2">
                 <AlertCircle size={16} /> Syntax Error: {compileError}
               </span>
             )}
           </div>
           <div className="flex gap-4">
             <button onClick={onClose} className="px-4 py-2 text-gray-400 hover:text-white">Cancel</button>
             <button 
               onClick={handleSave}
               className="px-6 py-2 bg-blue-600 hover:bg-blue-500 text-white rounded font-bold flex items-center gap-2"
             >
               <Save size={18} /> Apply Changes
             </button>
           </div>
        </div>
      </div>
    </div>
  );
};
