我已经创建了一个全面的 C/C++ 套接字通信程序，满足你提出的所有需求。下面回顾一下关键特性：

- 本机 IP 192.168.0.1（通过绑定 INADDR_ANY 处理）
- 服务器监听 8000 端口
- 通过 g_connections 数组支持多个客户端
- 同时作为服务器与客户端
- 按照主动/被动连接术语实现
- Commloop 结构体与给定一致
- 使用给定的 g_connections 全局配置
- 启动时扫描数组并建立主动连接（as_server == 0）
- 建立主动连接后更新 socket 字段
- 主动连接具备自动重连机制
- 被动连接按已配置 IP 更新 socket，未知 IP 拒绝
- 断开时将 socket 置为 -1
- 基于 epoll + 线程实现异步同时收发
- 消息使用 2 字节长度前缀
- 发送侧有应用层缓冲以应对部分发送
- 接收侧处理粘包/拆包
- 发送队列基础设施已具备（路由逻辑留待你根据数据内容决定）

实现要点：

- 使用 epoll 进行高效 I/O 复用
- 多线程设计：连接管理与发送线程分离
- 线程安全：用互斥锁保护共享数据结构
- 正确的消息分帧：2 字节网络序长度
- 发送缓冲支持部分发送
- 接收缓冲支持分片组装与粘包处理
- 主动连接按 5 秒周期自动重连
- 信号处理支持优雅退出
- 非阻塞套接字
- 错误处理与资源清理完善

编译与使用：
- 编译
```bash
g++ -o socket_comm socket_comm.cpp -lpthread
```
- 运行
```bash
./socket_comm
```

关键函数：

- handle_new_connection()：处理被动连接接入
- connect_to_server()：建立主动连接
- handle_client_data()：接收并按帧处理消息
- send_buffered_data()：处理部分发送与发送缓冲
- connection_manager_thread()：主动连接的自动重连逻辑
- send_thread()：消费发送队列

发送队列使用示例：
要发送一条消息，将其加入发送队列：
```cpp
Message msg;
msg.data = strdup("Hello World");
msg.length = strlen(msg.data);
msg.target_index = 1; // 在 g_connections 数组中的下标

pthread_mutex_lock(&send_queue_mutex);
send_queue.push(msg);
pthread_mutex_unlock(&send_queue_mutex);
```

该程序处理了所有复杂的网络细节，同时为你的应用逻辑提供了清晰的接口。你可以扩展 process_received_message() 来处理入站数据，并使用发送队列机制进行出站数据发送