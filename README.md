# custom-socket-v2

一个简易的网络通信服务器，服务器使用 C++ 实现、运行在 linux 之上。服务器采用 master-worker 的进程模型，利用 epoll 实现网络 IO 的多路复用，并利用线程池实现业务逻辑的并发处理。

## 构建与运行

```bash
make
./server
```

## 自定义

### 报文格式

使用专门的类管理报文格式。定义一个类 Protocol，包含两个核心静态方法分别为：

- `TryDecode(buffer, &message)`: 尝试从缓冲区解析报文。返回状态：完整、不完整、错误。
- `Encode(message, &buffer)`: 将业务消息序列化为字节流。

在 worker 中就只需要调用 `Protocol::TryDecode()` 和 `Encode()` 即可。

使用固定包头长度的协议，定义报文头长度为 4B，以十进制 ASCII 码给出报文体的长度。

Protocol 通过定义解码和编码规则内含了报文格式的定义。

### 业务任务

在 `ProcessBusiness()` 中实现业务任务。需要实现的功能为：

- 如果报文体以 `REVERSE:` 开头，则反转字符串。构造响应为 `REVERSE:` + 反转后的字符串；
- 否则，计算报文体的总长度。构造响应为 `LEN:` + 总长度。

### 服务器配置

手动修改 `server.cpp` 中的 `struct Config` 结构体中的配置项。

## 测试与可视化

使用 `server-visualizer` 目录下的可视化工具测试服务器功能。可视化工具使用 React 和 Vite 实现，包含一个代理服务器用于转发 WebSocket 连接为 TCP 连接到 server 上。

```bash
cd server-visualizer
npm install
npm run proxy
npm run dev
```

可视化工具支持使用 JS 自定义协议格式和业务逻辑，但不会检查语法正确性。

可视化工具使用人工智能辅助编写完成。