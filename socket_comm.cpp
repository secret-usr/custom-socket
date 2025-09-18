/**
 * socket_comm.cpp
 * Encoding: UTF-8
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <queue>
#include <mutex>
#include <iostream>
#include <fstream>
#include <vector>
#include <string> 

#include "include/nlohmann/json.hpp"
using json = nlohmann::json;

#include "include/log.h" // 日志打印宏, 如 LOGD, LOGI, LOGW, LOGE, LOG_SYSERR
#include "include/msghead.h" // 电文头定义

#define SERVER_PORT 8002 // 用于监听连接请求的端口号
#define MAX_EVENTS 10
#define MAX_MESSAGE_SIZE 65535
#define BUFFER_SIZE MAX_MESSAGE_SIZE
#define RECONNECT_INTERVAL 5  // 秒

// 连接结构体
struct Commloop {
    int socket;     // 套接字描述符，初始化为 -1，表示无效连接
    char ip[20];    // 远端服务器的 IP 地址
                    // - 当 as_server == 1 时代表允许连接的远端 IP（白名单）
                    // - 当 as_server == 0 时代表要连接的远端服务器 IP
    int port;       // 远端服务器的端口号，当 as_server == 1 时无效
    int as_server;  // 1 表示被动连接，0 表示主动连接
};

// 发送队列的消息结构
struct Message {
    char* data;         // 待发送数据，不包含电文头
    int length;         // 数据长度
    int target_index;   // 在 g_connections 数组中的目标下标
};

// 发送缓冲链
struct SendBuffer {
    char* data;         // 当前节点的待发送数据，已经包含电文头
    int total_length;   // 当前节点的数据总长度
    int sent_bytes;     // 已发送的字节数
    struct SendBuffer* next;
};

// 每个连接的接收缓冲
struct ReceiveBuffer {
    char data[BUFFER_SIZE];
    int received_bytes;
    int expected_length;
    bool header_received;
};

// 全局连接数组
// 当 as_server == 1 时，表示被动连接，本端作为服务端，等待远端连接。每一个远端连接占用这样的一个条目（插槽）
// 当 as_server == 0 时，表示主动连接，本端作为客户端，主动连接远端服务器
Commloop g_connections[] = {
    {-1, "127.0.0.1", 0, 1},   // 本机作为服务端监听 lo，插槽 #0
    {-1, "127.0.0.1", 0, 1},   // 本机作为服务端监听 lo，插槽 #1
    // {-1, "127.0.0.1", 0, 1},   // 本机作为服务端监听 lo，插槽 #2
    {-1, "192.168.199.1", 0, 1},   // 本机作为服务端监听 NetAssist
    {-1, "192.168.199.1", 8080, 0},    // 本机作为客户端，连接 NetAssist
};

// 全局变量
static const int g_connections_len = sizeof(g_connections) / sizeof(Commloop);
static int server_fd = -1;
static int epoll_fd = -1;
static bool running = true;
// 互斥锁保护 g_connections 数组及相关资源（如 socket、接收/发送缓冲区）
static pthread_mutex_t connections_mutex = PTHREAD_MUTEX_INITIALIZER;
// 互斥锁保护发送队列
static pthread_mutex_t send_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
// 发送队列条件变量
static pthread_cond_t  send_queue_cv    = PTHREAD_COND_INITIALIZER;

// 发送队列与缓冲
static std::queue<Message> send_queue;                          // 发送队列
static SendBuffer* send_buffers[g_connections_len] = {};        // 每个连接的发送缓冲链头指针
static ReceiveBuffer receive_buffers[g_connections_len];

// 函数声明
void dummy_function();
void signal_handler(int sig);
int create_server_socket();
int create_client_socket(const char* ip, int port);
void set_nonblocking(int sock);
void* connection_manager_thread(void* arg);
void* send_thread(void* arg);
void* get_sendmsg_thread(void* arg);
int find_connection_by_socket(int socket);
int find_connection_by_ip_and_type(const char* ip, int as_server);
void handle_new_connection(int server_fd);
void handle_client_data(int conn_index);
void handle_client_disconnect(int conn_index);
bool connect_to_server(int conn_index);
void add_to_send_buffer(int conn_index, const char* data, int length);
bool send_buffered_data(int conn_index);
bool add_to_send_queue_std_string(int conn_index, const std::string& data);
void process_received_message(int conn_index, const char* data, int length);
void cleanup_connection(int conn_index);
std::vector<Commloop> load_connections(const std::string& filename);
void save_connections(const std::string& filename, const std::vector<Commloop>& conns);

// 信号处理
void signal_handler(int sig) {
    LOGI("收到信号 %d，正在退出...", sig);
    running = false;
}

// 创建并配置服务器套接字，用于监听连接请求
int create_server_socket() {
    int sock = socket(AF_INET, SOCK_STREAM, 0); // 创建 ipv4 TCP 套接字，返回套接字描述符
    if (sock < 0) {
        LOG_SYSERR("socket");
        return -1;
    }

    int opt = 1;
    // 设置地址复用
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_SYSERR("setsockopt");
        close(sock);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;          // 地址族为 IPv4
    addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有地址
    addr.sin_port = htons(SERVER_PORT); // 监听端口 SERVER_PORT，端口转为大端序

    // 绑定套接字到地址
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_SYSERR("bind");
        close(sock);
        return -1;
    }

    // 开始监听连接
    if (listen(sock, SOMAXCONN) < 0) {
        LOG_SYSERR("listen");
        close(sock);
        return -1;
    }

    // 设置非阻塞模式
    set_nonblocking(sock);
    LOGI("连接监听服务器运行在 %s:%d，套接字描述符: %d",
         inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), sock);
    return sock;
}

// 创建客户端套接字并连接服务器
int create_client_socket(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOG_SYSERR("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // 将 IP 地址从字符串转换为二进制格式
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        LOGW("无效的 IP 地址: %s", ip);
        close(sock);
        return -1;
    }

    // 尝试连接到服务器
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_SYSERR("connect");
        close(sock);
        return -1;
    }

    // 设置非阻塞模式
    set_nonblocking(sock);
    LOGI("已连接到 %s:%d，套接字描述符: %d", ip, port, sock);
    return sock;
}

// 把套接字设为非阻塞模式
void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

// 通过套接字描述符查找连接下标
int find_connection_by_socket(int socket) {
    for (int i = 0; i < g_connections_len; i++) {
        if (g_connections[i].socket == socket) {
            return i;
        }
    }
    return -1;
}

// 通过 IP 与类型查找并分配连接下标
// 策略：
// 1) 仅考虑 as_server 匹配的条目
// 2) IP 精确匹配视为匹配
// 3) 返回第一个空槽位
// 4) 如果有匹配但全被占用返回 -1
// 5) 如果没有任何匹配，返回 -1
int find_connection_by_ip_and_type(const char* ip, int as_server) {
    bool has_match = false;
    for (int i = 0; i < g_connections_len; i++) {
        if (g_connections[i].as_server != as_server) continue;
        bool ip_match = (strcmp(g_connections[i].ip, ip) == 0);
        if (!ip_match) continue;
        has_match = true;
        if (g_connections[i].socket == -1) {
            return i;
        }
    }
    if (has_match) {
        // 有匹配但无空槽位
        LOGW("无空余槽位可分配给来自 %s 的连接，拒绝新连接", ip);
    }
    return -1;
}

// 处理新的被动连接
void handle_new_connection(int server_fd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    // 接受新的连接，获得新的套接字描述符用于连接和对端地址
    int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

    if (client_sock < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_SYSERR("accept");
        }
        return;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

    // 在 g_connections 中查找匹配的连接
    pthread_mutex_lock(&connections_mutex);
    int conn_index = find_connection_by_ip_and_type(client_ip, 1);

    if (conn_index == -1) {
        LOGI("拒绝来自未知 IP %s 的连接", client_ip);
        close(client_sock);
        pthread_mutex_unlock(&connections_mutex);
        return;
    }

    if (g_connections[conn_index].socket != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, g_connections[conn_index].socket, NULL);
        close(g_connections[conn_index].socket);
    }

    g_connections[conn_index].socket = client_sock;
    set_nonblocking(client_sock);

    // 加入 epoll
    struct epoll_event ev;
    // NOTE
    // 在被动连接中，当 EPOLLOUT 事件触发时，如果 send_thread 尚未将队列中的消息添加到 send_buffers 中，
    // 那么 send_buffered_data 会发现缓冲为空，无法发送数据。
    // 在程序中主动发送消息的流程如下：
    // 1. 调用 add_to_send_queue_std_string 将消息加入 send_queue
    // 2. 等待 send_thread 线程将消息从 send_queue 移动到 send_buffers
    // 3. 等待 epoll 触发 EPOLLOUT 事件，调用 send_buffered_data 发送数据
    // 如果采用边缘触发，那么直到下次写操作失败导致不可写状态，恢复可写状态时才会发送这个消息，造成发送延迟。
    // 经过调试，当连接有数据可读时，EPOLLIN 会触发，同时 EPOLLOUT 也会触发。从而，上面的情况下，消息延迟会等到下一次接收数据时才发送。
    // 目前的解决方案是：send_thread 中处理消息后立即尝试发送一次数据
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = client_sock;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &ev);

    // 初始化接收缓冲
    memset(&receive_buffers[conn_index], 0, sizeof(ReceiveBuffer));

    LOGI("已接受来自 %s 的被动连接，作为连接 %d", client_ip, conn_index);
    add_to_send_queue_std_string(conn_index, "hello");
    pthread_mutex_unlock(&connections_mutex);
}

// 处理连接上的数据
void handle_client_data(int conn_index) {
    ReceiveBuffer* rb = &receive_buffers[conn_index];
    int sock = g_connections[conn_index].socket;

    while (true) {
        int bytes_to_read;
        int read_offset;
        int head_len = MsgHead::get_head_length();

        if (!rb->header_received) {
            // 读取消息头
            bytes_to_read = head_len - rb->received_bytes;
            read_offset = rb->received_bytes;
        } else {
            // 读取消息体
            bytes_to_read = rb->expected_length - rb->received_bytes;
            read_offset = rb->received_bytes;
        }

        int bytes_read = recv(sock, rb->data + read_offset, bytes_to_read, 0);

        if (bytes_read <= 0) {
            if (bytes_read == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                // 连接关闭或错误
                handle_client_disconnect(conn_index);
            }
            break;
        }

        rb->received_bytes += bytes_read;
        if (rb->header_received) {
            LOGI("尝试从连接 %d 读取，%d/%d", conn_index , rb->received_bytes, rb->expected_length);
        }

        if (!rb->header_received && rb->received_bytes >= head_len) {
            // 头部读取完成，解析消息长度
            // 可以确定 rb->data 指针必然已经指向了一个完整的 MsgHead 结构
            MsgHead* mh = (MsgHead*)rb->data;
            // 调用这个完整的 MsgHead 结构的成员函数以获取消息体长度
            rb->expected_length = mh->get_body_length();
            rb->header_received = true;
            rb->received_bytes = 0; // 重置用于读取消息体

            if (rb->expected_length > MAX_MESSAGE_SIZE) {
                LOGW("消息过大（%d 字节），断开连接", rb->expected_length);
                handle_client_disconnect(conn_index);
                break;
            }
        } else if (rb->header_received && rb->received_bytes >= rb->expected_length) {
            // 收到完整消息
            process_received_message(conn_index, rb->data, rb->expected_length);

            // 重置缓冲，准备下一条消息
            memset(rb, 0, sizeof(ReceiveBuffer));
        }
    }
}

// 处理连接断开
void handle_client_disconnect(int conn_index) {
    LOGI("连接 %d 已断开", conn_index);
    cleanup_connection(conn_index);
}

// 主动连接远端服务器
bool connect_to_server(int conn_index) {
    if (g_connections[conn_index].as_server == 1) {
        return false; // 检查 as_server 项，避免非预期的调用
    }

    int sock = create_client_socket(g_connections[conn_index].ip, g_connections[conn_index].port);
    if (sock < 0) {
        return false;
    }

    pthread_mutex_lock(&connections_mutex);
    g_connections[conn_index].socket = sock;

    // 加入 epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = sock;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev);

    // 初始化接收缓冲
    // 每次重连也会重置接收缓冲
    memset(&receive_buffers[conn_index], 0, sizeof(ReceiveBuffer));

    LOGI("已连接到 %s:%d",
           g_connections[conn_index].ip, g_connections[conn_index].port);
    pthread_mutex_unlock(&connections_mutex);

    return true;
}

// 为数据加上电文头，组装成完整电文后，发送到缓冲链，调用时需持有 connections_mutex 锁
void add_to_send_buffer(int conn_index, const char* data, int length) {
    int head_len = MsgHead::get_head_length();
    SendBuffer* new_buffer = (SendBuffer*)malloc(sizeof(SendBuffer));
    new_buffer->total_length = length + head_len; // 此长度包含电文头
    new_buffer->data = (char*)malloc(new_buffer->total_length);
    new_buffer->sent_bytes = 0;
    new_buffer->next = NULL;

    // 生成电文头，组装成完整电文
    MsgHead msg_head = {};
    msg_head.msglen = htons(length);
    memcpy(new_buffer->data, &msg_head, head_len);
    memcpy(new_buffer->data + head_len, data, length);

    // 加到缓冲链末尾
    if (send_buffers[conn_index] == NULL) {
        send_buffers[conn_index] = new_buffer;
    } else {
        SendBuffer* current = send_buffers[conn_index];
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_buffer;
    }
}

// 尝试发送缓冲链中的数据，调用时需持有 connections_mutex 锁
bool send_buffered_data(int conn_index) {
    int sock = g_connections[conn_index].socket;
    if (sock == -1) return false;

    while (send_buffers[conn_index] != NULL) {
        // 拷贝当前缓冲
        SendBuffer* buffer = send_buffers[conn_index];
        // 计算剩余的未发送字符，并发送这些字符
        int remaining = buffer->total_length - buffer->sent_bytes;
        int sent = send(sock, buffer->data + buffer->sent_bytes, remaining, MSG_NOSIGNAL);

        if (sent > 0) {
            LOGI("尝试发送连接 %d 的缓冲数据 %d 字节，实际发送 %d 字节；前 %d 字节：%s",
                 conn_index, remaining, sent, (int)std::min<size_t>(sent, 128),
                 HEX_DUMP(buffer->data + buffer->sent_bytes, sent));
        } else {
            LOGI("尝试发送连接 %d 的缓冲数据 %d 字节，实际发送 %d 字节（失败或无数据）",
                 conn_index, remaining, sent);
        }

        if (sent <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;    // 内核发送缓冲满，稍后再试
            } else {
                return false;   // 发生其他错误
            }
        }
        // 更新已发送字节数            
        buffer->sent_bytes += sent;

        // 如果当前缓冲已全部发送，释放该节点
        if (buffer->sent_bytes >= buffer->total_length) {
            send_buffers[conn_index] = buffer->next;
            free(buffer->data);
            free(buffer);
        }
    }

    return true;
}

// 处理收到的消息，已由上层函数去除电文头
void process_received_message(int conn_index, const char* data, int length) {
    LOGI("来自连接 %d 的消息接收完成，电文体总长度 = %d，前 %d 字节：%s",
         conn_index, length, (int)std::min<size_t>(length, 128), HEX_DUMP(data, length));
    // ======================
    // 在此加入业务处理逻辑
    // ======================
    // WARNNING: FOR DEBUG ONLY, REMEMBER TO REMOVE
    add_to_send_queue_std_string(2, std::string(data, length));
}

// 清理连接（从 epoll 移除、关闭、清空缓冲）
void cleanup_connection(int conn_index) {
    pthread_mutex_lock(&connections_mutex);

    if (g_connections[conn_index].socket != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, g_connections[conn_index].socket, NULL);
        close(g_connections[conn_index].socket);
        g_connections[conn_index].socket = -1;
    }

    // 清空发送缓冲
    while (send_buffers[conn_index] != NULL) {
        SendBuffer* buffer = send_buffers[conn_index];
        send_buffers[conn_index] = buffer->next;
        free(buffer->data);
        free(buffer);
    }

    // 清空接收缓冲
    memset(&receive_buffers[conn_index], 0, sizeof(ReceiveBuffer));

    pthread_mutex_unlock(&connections_mutex);
}

// 连接管理线程，用于负责主动连接的重连
void* connection_manager_thread(void* arg) {
    while (running) {
        sleep(RECONNECT_INTERVAL);

        for (int i = 0; i < g_connections_len; i++) {
            // 只在检查状态时加锁，防止与 connect_to_server 的内部加锁发生死锁
            pthread_mutex_lock(&connections_mutex);
            bool need_reconnect = g_connections[i].as_server == 0 && g_connections[i].socket == -1;
            pthread_mutex_unlock(&connections_mutex);
            if (need_reconnect) {
                LOGI("尝试重连到 %s:%d",
                     g_connections[i].ip, g_connections[i].port);
                connect_to_server(i);
            }
        }
    }
    return NULL;
}
// 发送线程，从发送队列取数据并发送
void* send_thread(void* arg) {
    while (running) {
        pthread_mutex_lock(&send_queue_mutex);
        // 队列空则等待
        while (send_queue.empty() && running) {
            // 释放锁并等待条件变量
            pthread_cond_wait(&send_queue_cv, &send_queue_mutex);
        }
        if (!running) { // 退出
            pthread_mutex_unlock(&send_queue_mutex);
            break;
        }
        Message msg = send_queue.front();
        send_queue.pop();
        pthread_mutex_unlock(&send_queue_mutex);

        // 此处对 g_connections 数组对应的缓冲进行加锁
        pthread_mutex_lock(&connections_mutex);
        
        if (g_connections[msg.target_index].socket != -1) {
            // 将发送数据加入对应连接的发送缓冲
            add_to_send_buffer(msg.target_index, msg.data, msg.length);
            // 立即尝试一次发送
            send_buffered_data(msg.target_index);
        }
        pthread_mutex_unlock(&connections_mutex);

        free(msg.data);
    }
    return NULL;
}

// 获取待发送电文线程
// 
// 一般来讲，此项目仅用于电文的收发，待发送电文是从别的进程获取的。 
// 整个电文（含电文头）应该由业务进程组装，本项目仅负责发送数据。
// 未来可在此处阻塞/轮询业务模块或读取文件/消息队列以获取要发送的电文。
void* get_sendmsg_thread(void* arg) {
    while (running) {
        // 占位：模拟周期性检查外部来源是否有新电文
        // 在这里未来可以添加：
        // 1. 读取文件 / 命名管道 / 消息队列
        // 2. 从共享内存或业务模块获取待发送消息
        // 3. 解析并加入到对应的 send_buffer
        LOGD("get_sendmsg_thread 周期检查，占位实现");
        sleep(1); // 休眠 1 秒，避免空转占用 CPU
    }
    return NULL;
}

// 将数据加入到发送队列，使用 std::string 作为输入
// 如果数据长度超过 MAX_MESSAGE_SIZE，则拆分为多段发送
bool add_to_send_queue_std_string(int conn_index, const std::string& data) {
    if (conn_index < 0 || conn_index >= g_connections_len) {
        LOGW("参数非法 conn_index=%d", conn_index);
        return false;
    }
    if (data.empty()) {
        LOGW("数据为空 conn_index=%d", conn_index);
        return false;
    }

    size_t total = data.size();
    size_t offset = 0;
    int chunks = 0;

    // 再持锁的状态下将数据拆分并加入发送队列, 然后通过条件变量唤醒发送线程
    pthread_mutex_lock(&send_queue_mutex);
    while (offset < total) {
        size_t chunk_len = std::min(static_cast<size_t>(MAX_MESSAGE_SIZE),
                                    total - offset);

        Message msg;
        msg.length = static_cast<int>(chunk_len);
        msg.target_index = conn_index;
        msg.data = (char*)malloc(chunk_len);
        if (!msg.data) {
            LOGE("内存分配失败 chunk_len=%zu", chunk_len);
            // 已经排入队列的数据保持；退出
            pthread_mutex_unlock(&send_queue_mutex);
            return false;
        }
        memcpy(msg.data, data.data() + offset, chunk_len);
        send_queue.push(msg);

        offset += chunk_len;
        ++chunks;
    }
    // 入队完成，唤醒发送线程
    pthread_cond_signal(&send_queue_cv);
    pthread_mutex_unlock(&send_queue_mutex);

    LOGI("已将消息加入发送队列，目标连接 %d，总长度 %zu 字节，共分 %d 段；前 %d 字节：%s",
         conn_index, total, chunks,
         (int)std::min<size_t>(total, 128),
         HEX_DUMP(data.data(), total));

    return true;
}

// 从文件加载连接配置，以 JSON 格式【未测试】
std::vector<Commloop> load_connections(const std::string& filename) {
    std::ifstream fin(filename);
    json j;
    fin >> j;
    std::vector<Commloop> result;
    for (auto& item : j) {
        Commloop c;
        c.socket = item["socket"];
        strcpy(c.ip, item["ip"].get<std::string>().c_str());
        c.port = item["port"];
        c.as_server = item["as_server"];
        result.push_back(c);
    }
    return result;
}

// 将连接配置保存到文件，以 JSON 格式
void save_connections(const std::string& filename, const std::vector<Commloop>& conns) {
    json j = json::array();
    for (const auto& c : conns) {
        j.push_back({
            {"socket", c.socket},
            {"ip", c.ip},
            {"port", c.port},
            {"as_server", c.as_server}
        });
    }
    std::ofstream fout(filename);
    fout << j.dump(4);
}

// 主函数
int main() {
    signal(SIGINT, signal_handler);     // 处理 Ctrl+C 终止信号
    signal(SIGTERM, signal_handler);    // 处理 kill 命令的终止信号
    signal(SIGPIPE, SIG_IGN);           // 忽略 SIGPIPE 信号，防止写断开的 socket 导致程序退出

    // 初始化接收缓冲
    memset(receive_buffers, 0, sizeof(receive_buffers));

    // 创建服务器套接字，用于监听连接请求
    server_fd = create_server_socket();
    if (server_fd < 0) {
        LOGE("创建服务器套接字失败");
        return 1;
    }

    // 创建 epoll 实例，使用 epoll 统一管理所有 socket 的收发和连接状态
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        LOG_SYSERR("epoll_create1");
        close(server_fd);
        return 1;
    }

    // 将服务器套接字加入 epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    // 向 epoll 对象中添加感兴趣的事件，socket server_fd 的可读事件
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    // 主动连接远端服务器
    for (int i = 0; i < g_connections_len; i++) {
        if (g_connections[i].as_server == 0) {
            connect_to_server(i);
        }
    }

    // 启动连接管理线程
    pthread_t conn_manager_tid;
    pthread_create(&conn_manager_tid, NULL, connection_manager_thread, NULL);

    // 启动发送线程
    pthread_t send_tid;
    pthread_create(&send_tid, NULL, send_thread, NULL);

    // 启动获取待发送电文线程
    pthread_t get_sendmsg_tid;
    pthread_create(&get_sendmsg_tid, NULL, get_sendmsg_thread, NULL);

    // 主事件循环
    struct epoll_event events[MAX_EVENTS];
    LOGI("服务已启动，进入主循环...");

    while (running) {
        // 收集在 epoll 监控的事件中已经发生的事件，如果 epoll 中没有任何一个事件发生，则最多等待 1000ms
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_SYSERR("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            // 当发生事件的文件描述符为服务器套接字时，表示有新的连接请求
            if (fd == server_fd) {
                // 新的被动连接
                handle_new_connection(server_fd);
            } else {
                // 已有连接上的事件
                int conn_index = find_connection_by_socket(fd);
                if (conn_index != -1) {
                    // 表示对应的文件描述符可以读（包括对端SOCKET正常关闭）
                    if (events[i].events & EPOLLIN) {
                        LOGD("EPOLL 发现连接 %d 有数据可读，尝试读取数据", conn_index);
                        handle_client_data(conn_index);
                    }
                    // 表示对应的文件描述符可以写，此时尝试发送缓冲区的数据
                    if (events[i].events & EPOLLOUT) {
                        LOGD("EPOLL 发现连接 %d 可写，尝试发送缓冲区数据", conn_index);
                        pthread_mutex_lock(&connections_mutex);
                        bool success = send_buffered_data(conn_index);
                        pthread_mutex_unlock(&connections_mutex);
                        if (!success) {
                            handle_client_disconnect(conn_index);
                        }
                    }
                    // 连接关闭或错误
                    if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                        handle_client_disconnect(conn_index);
                    }
                }
            }
        }
    }

    // 清理
    LOGI("正在关闭...");

    // 唤醒可能在等待的发送线程
    pthread_mutex_lock(&send_queue_mutex);
    pthread_cond_broadcast(&send_queue_cv);
    pthread_mutex_unlock(&send_queue_mutex);

    pthread_cancel(conn_manager_tid);
    pthread_cancel(send_tid);
    // 取消并等待获取发送电文线程
    pthread_cancel(get_sendmsg_tid);
    pthread_join(conn_manager_tid, NULL);
    pthread_join(send_tid, NULL);
    pthread_join(get_sendmsg_tid, NULL);

    for (int i = 0; i < g_connections_len; i++) {
        cleanup_connection(i);
    }

    close(server_fd);
    close(epoll_fd);

    pthread_mutex_destroy(&connections_mutex);
    pthread_mutex_destroy(&send_queue_mutex);
    pthread_cond_destroy(&send_queue_cv);

    LOGI("关闭完成");
    return 0;
}