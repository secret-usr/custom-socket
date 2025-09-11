## 编译使用

- 编译
```bash
g++ -o socket_comm socket_comm.cpp -lpthread
```

现在可以使用 Makefile 进行编译了。

```bash
make clean && make
```

通过不同的构建目标指定日志等级，可选 `debug`/`info`/`warning`/`error`/`build`，默认构建目标的日志等级为 `info`。

运行

```bash
./custom_socket
```

## 功能特性

每个 `socket_comm` 都可以同时作为服务端 S 和客户端 C，并与其他的 `socket_comm` 进行通信。
每个 `socket_comm` 都有自己预配置的 `g_connections` 列表，每一个 `Commloop` 项描述一个连接信息，包括 (1) socket 描述符; (2) 连接目标的 IP:port; (3) 自己是否作为服务端。
`socket_comm` 启动时尝试根据 `g_connections` 列表建立主动连接。

`g_connections` 列表的每个元素定义为：

```cpp
struct Commloop {
    int socket;   // 套接字描述符，初始化为 -1，表示无效连接
    char ip[20];  // 远端服务器的 IP 地址
    int port;     // 远端服务器的端口号，当 as_server == 1 时该字段为 0
    int as_server;// 1 表示被动连接，0 表示主动连接
};
```

功能：

- 程序同时支持作为服务器 S 或客户端 C
- 程序在 `SERVER_PORT = 8002` 端口监听来自其他客户端的连接请求，这种连接被称为被动链接
- 程序为每个被动连接创建一个新的套接字
- 程序可以根据 `g_connections` 列表的配置向其他服务端发起连接请求，这种连接被称为主动连接
- 程序为每个主动连接创建一个新的套接字
- 对每个主动连接，当远端服务器断开或因异常导致连接中断时，具有自动重连机制
- 基于 epoll + 线程实现异步同时收发
- 每条电文的电文头可更具实际需求扩展
- 程序建立了待发送电文的缓冲区 `SendBuffer`，并设置发送线程 `send_thread` 专门负责向该缓冲区填充数据
- 接收消息时能够处理“粘包”问题

---

消息发送流程：

1. 发送者主动调用 `add_to_send_queue_std_string()` 将待发送的电文体、对端连接号加入发送队列 `send_queue`。
2. `send_thread()` 等待 `send_queue` 的 cv 锁并被唤醒，将 `send_queue` 中的数据组装为符合格式的电文，并移动到连接号所对应的 `send_buffers` 项中。
3. 程序主循环收到 `EPOLLOUT`，调用 `send_buffered_data()` 发送到对应的 socket 连接。

消息接收（来自内部）：

1. `get_sendmsg_thread()` 负责接受由其他线程提供的，待发送的消息

消息接收（来自连接）：

1. 程序主循环收到 `EPOLLIN`，调用 `handle_client_data()` 处理来自对端的电文，获取完整的电文体后调用 `process_received_message()` 进行进一步的处理

---

可以扩展 `process_received_message()` 来处理入站数据，并使用发送队列机制进行出站数据发送

暂时能够通过 `add_to_send_queue_std_string()` 强制手动发送消息

可以通过在另外一个终端 `sudo fuser -k 8002/tcp` 杀死程序
