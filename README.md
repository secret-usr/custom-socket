
```
.
├── proto
│   ├── include                         # 第三方库
│   │   └── nlohmann
│   │       └── json.hpp
│   ├── README.md                       # 关于程序本体的 README
│   └── socket_comm.cpp                 # 程序本体
├── README.md                           # 本文件，关于完整仓库的 README
└── utils                               # 杂项工具
    └── func-call-analyzer              # 分析单文件cpp函数调用关系
        ├── func-call-analyzer.py
        └── README.md
```

本体编译运行：

```
cd proto
g++ -o socket_comm socket_comm.cpp -lpthread 
./socket_comm
```

每个 `socket_comm` 都可以同时作为服务端 S 和客户端 C，并与其他的 `socket_comm` 进行通信。
每个 `socket_comm` 都有自己预配置的 `g_connections` 列表，每一个 `Commloop` 项描述一个连接信息，包括 (1) socket 描述符; (2) 连接目标的 IP:port; (3) 自己是否作为服务端。
`socket_comm` 启动时尝试根据 `g_connections` 列表建立主动连接。
可能会出现以下情况：
(1) 成功连接，双方更新 socket 描述符；
(2) P1 主动连接 P2，但 P2 的 `g_connections` 列表没有对应项，无法成功连接， P1 进入主动连接失败的异常处理流程（假设是尝试重连直到次数达到最大重连次数）；
(3) P3 的 `g_connections` 列表中有某项作为服务器被 P1 连接，但 P1 的 `g_connections` 列表没有对应项，即没有主动连接的客户端，连接不能建立（这种情况下可以让每个 `socket_comm` 周期性检查 `g_connections` 列表检查出来）；
(4) ...
这时 `is_server = 1` 并且 `ip = 0.0.0.0` 就表示接受任何地址的连接请求。

---

以下 LLM 生成。

1. 本机 IP 地址为 192.168.0.1，这可能并不重要。
2. 本机作为服务器监听 8000 端口。
3. 有多个客户端需要与本机通信，各客户端的 IP 地址配置在全局数组 g_connections 中。
4. 本机也作为客户端与远端服务器建立连接，远端服务器的地址和端口号同样配置在全局数组 g_connections 中。
5. 术语定义：
   主动连接（Active Connection）：本机作为客户端与远端服务器建立的 SOCKET 连接。
   被动连接（Passive Connection）：本机作为服务器接收的、由远端客户端发起的 SOCKET 连接。
6. 为描述每个 SOCKET 连接的信息，我定义了一个名为 Commloop 的结构体。各字段的作用在下面的注释中说明：
```cpp
struct Commloop {
    int socket;   // 套接字描述符，初始化为 -1，表示无效连接
    char ip[20];  // 远端服务器的 IP 地址
    int port;     // 远端服务器的端口号，当 as_server == 1 时该字段为 0
    int as_server;// 1 表示被动连接，0 表示主动连接
};
```
7. 定义了一个 Commloop 类型的全局数组，每条记录表示一个主动或被动连接的信息：
```cpp
Commloop g_connections[4] = {
    {-1, "192.168.0.2", 0,    1},
    {-1, "192.168.0.2", 8001, 0},
    {-1, "192.168.0.3", 0,    1},
    {-1, "192.168.0.3", 8002, 0}
};
```
8. 程序启动时扫描 g_connections 数组，若 as_server 字段为 0，则根据 ip 和 port 字段主动向远端服务器发起 SOCKET 连接。
9. 对于主动连接，当连接建立后，需根据远端 IP 且满足 as_server == 0 的条件，更新 g_connections 数组中的 socket 字段。
10. 对每个主动连接，当远端服务器断开或因异常导致连接中断时，应具有自动重连机制。
11. 对每个接收到的被动连接，应根据远端 IP 且满足 as_server == 1 的条件，更新 g_connections 数组中的 socket 字段；若远端 IP 不在 g_connections 数组中，应关闭该连接。
12. 当连接断开时，应将对应连接记录在 g_connections 数组中的 socket 字段设为 -1。
13. 发送与接收应可异步、同时进行。
14. 每条消息的前 2 个字节是当前消息长度的二进制整数。
15. 若因 SOCKET 发送缓冲区满导致消息无法一次性发送，应建立应用层缓冲机制。
16. 接收消息时需要处理“粘包”问题。
17. 从发送队列中读取数据进行发送，但发送到哪个连接暂时可不实现，我会根据数据内容再决定。
