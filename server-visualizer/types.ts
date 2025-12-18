
export interface MessageItem {
  id: string; // Unique ID (UUID)
  timestamp: number;
  content: string; // The raw string content
  type: 'sent' | 'received';
  matchedId?: string; // ID of the counterpart message if matched
  isExpired?: boolean; // If true, ready to be removed from DOM
}

export interface Stats {
  sent: number;
  received: number;
  matched: number;
}

export interface ProtocolCode {
  decoder: string; // JS code body for tryDecode
  encoder: string; // JS code body for encode
  matcher: string; // JS code body for getExpectedResponse
  payloadGenerator: string; // JS code body for generatePayload
}

export interface AppConfig {
  sendRate: number; // Messages per second
  disappearTime: number; // Seconds before matched messages fade out
  isRunning: boolean;
  proxyAddress: string; // e.g. "localhost:8081"
  serverHost: string;
  serverPort: number;
  protocolCode: ProtocolCode;
}

// The interface for the compiled handler
export interface ProtocolHandler {
  encode: (content: string) => string | ArrayBuffer;
  tryDecode: (buffer: string) => { length: number; content: string } | null;
  getExpectedResponse: (sentContent: string) => string;
  generatePayload: () => string;
}
