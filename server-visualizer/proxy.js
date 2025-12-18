import { WebSocketServer, WebSocket } from 'ws';
import * as net from 'net';
import * as url from 'url';

const WS_PORT = 8081;
const TCP_HOST = 'localhost';
const TCP_PORT = 8080;

const wss = new WebSocketServer({ port: WS_PORT });

console.log(`WebSocket Proxy running on ws://localhost:${WS_PORT}`);
console.log(`Forwarding to dynamic targets`);

wss.on('connection', (ws, req) => {
    console.log('Client connected to WebSocket');

    // Parse query params for dynamic target
    // Expected URL: ws://localhost:8081/?host=localhost&port=8002
    const parameters = url.parse(req.url, true).query;
    const targetHost = parameters.host || TCP_HOST;
    const targetPort = parameters.port ? parseInt(parameters.port) : TCP_PORT;

    console.log(`Targeting: ${targetHost}:${targetPort}`);

    const tcpClient = new net.Socket();
    let isTcpConnected = false;

    // Connect to C++ Server
    tcpClient.connect(targetPort, targetHost, () => {
        console.log(`Connected to TCP Server at ${targetHost}:${targetPort}`);
        isTcpConnected = true;
    });

    // WebSocket -> TCP
    ws.on('message', (message) => {
        if (isTcpConnected) {
            // message is Buffer or string. net.Socket.write accepts Uint8Array or string.
            // If message is a Buffer (which is a Uint8Array), it works.
            tcpClient.write(message); 
        }
    });

    // TCP -> WebSocket
    tcpClient.on('data', (data) => {
        if (ws.readyState === WebSocket.OPEN) {
            ws.send(data);
        }
    });

    // Error handling & Cleanup
    const cleanup = () => {
        console.log('Closing connection');
        isTcpConnected = false;
        tcpClient.destroy();
        if (ws.readyState === WebSocket.OPEN) {
            ws.close();
        }
    };

    ws.on('close', cleanup);
    ws.on('error', cleanup);
    tcpClient.on('close', cleanup);
    tcpClient.on('error', (err) => {
        console.error('TCP Error:', err.message);
        cleanup();
    });
});
