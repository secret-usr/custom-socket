
import { ProtocolHandler, ProtocolCode } from '../types';

// Default implementation (JS version of your C++ code)
export const DEFAULT_PROTOCOL_CODE: ProtocolCode = {
  decoder: `
// Input: 'buf' (string)
// Output: { length: number, content: string } | null
// Return null if incomplete, throw error if invalid.

if (buf.length < 4) return null;

const lenStr = buf.substring(0, 4);
const len = parseInt(lenStr, 10);

if (isNaN(len)) throw new Error("Invalid header");
if (buf.length < 4 + len) return null; // Incomplete

return {
  length: 4 + len,
  content: buf.substring(4, 4 + len)
};
`,
  encoder: `
// Input: 'content' (string)
// Output: string

const len = content.length;
const header = len.toString().padStart(4, '0');
return header + content;
`,
  matcher: `
// Input: 'request' (string)
// Output: string (expected response)

if (request.startsWith("REVERSE:")) {
  const content = request.substring(8);
  const reversed = content.split('').reverse().join('');
  return "REVERSE:" + reversed;
} else {
  return "LEN:" + request.length.toString();
}
`,
  payloadGenerator: `
// Output: string
// Generate a random message content

const minLen = 114;
const maxLen = 514;
const len = Math.floor(Math.random() * (maxLen - minLen + 1)) + minLen;

let payload = "";
for (let i = 0; i < len; i++) {
  const code = Math.floor(Math.random() * (126 - 32 + 1)) + 32;
  payload += String.fromCharCode(code);
}

const prefix = Math.random() < 0.5 ? "REVERSE:" : "";
return prefix + payload;
`
};

/**
 * Compiles the user's string code into executable functions.
 */
export const compileProtocol = (code: ProtocolCode): ProtocolHandler => {
  try {
    // We wrap the user's code in new Function.
    
    // Encoder: (content) => result
    const encodeFn = new Function('content', code.encoder);

    // Decoder: (buf) => result
    const decodeFn = new Function('buf', code.decoder);

    // Matcher: (request) => result
    const matcherFn = new Function('request', code.matcher);

    // PayloadGenerator: () => result
    const payloadGenFn = new Function(code.payloadGenerator);

    return {
      encode: (content: string) => {
        return encodeFn(content);
      },
      tryDecode: (buf: string) => {
        const result = decodeFn(buf);
        return result;
      },
      getExpectedResponse: (sentContent: string) => {
        return matcherFn(sentContent);
      },
      generatePayload: () => {
        return payloadGenFn();
      }
    };
  } catch (e) {
    console.error("Failed to compile protocol", e);
    throw e;
  }
};
