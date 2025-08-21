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

#define SERVER_PORT 8000
#define MAX_EVENTS 10
#define BUFFER_SIZE 8192
#define MAX_MESSAGE_SIZE 65535
#define RECONNECT_INTERVAL 5  // 秒

// 连接结构体
struct Commloop {
    int socket;          // 套接字描述符，初始化为 -1，表示无效连接
    char ip[20];         // 远端服务器的 IP 地址
    int port;            // 远端服务器的端口号，当 as_server == 1 时该字段为 0
    int as_server;       // 1 表示被动连接，0 表示主动连接
};

// 发送队列的消息结构
struct Message {
    char* data;
    int length;
    int target_index;  // 在 g_connections 数组中的目标下标
};

// 每个连接的发送缓冲链
struct SendBuffer {
    char* data;
    int total_length;
    int sent_bytes;
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
Commloop g_connections[4] = {
    {-1, "192.168.0.2", 0,    1},   // 192.168.0.2 的被动连接
    {-1, "192.168.0.2", 8001, 0},   // 主动连接到 192.168.0.2:8001
    {-1, "192.168.0.3", 0,    1},   // 192.168.0.3 的被动连接
    {-1, "192.168.0.3", 8002, 0}    // 主动连接到 192.168.0.3:8002
};

// 全局变量
static int server_fd = -1;
static int epoll_fd = -1;
static bool running = true;
static pthread_mutex_t connections_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t send_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// 发送队列与缓冲
static std::queue<Message> send_queue;
static SendBuffer* send_buffers[4] = {NULL, NULL, NULL, NULL};
static ReceiveBuffer receive_buffers[4];

// 函数声明
void signal_handler(int sig);
int create_server_socket();
int create_client_socket(const char* ip, int port);
void set_nonblocking(int sock);
void* connection_manager_thread(void* arg);
void* send_thread(void* arg);
int find_connection_by_socket(int socket);
int find_connection_by_ip_and_type(const char* ip, int as_server);
void handle_new_connection(int server_fd);
void handle_client_data(int conn_index);
void handle_client_disconnect(int conn_index);
bool connect_to_server(int conn_index);
void add_to_send_buffer(int conn_index, const char* data, int length);
bool send_buffered_data(int conn_index);
void process_received_message(int conn_index, const char* data, int length);
void cleanup_connection(int conn_index);

// 信号处理：优雅退出
void signal_handler(int sig) {
    printf("收到信号 %d，正在退出...\n", sig);
    running = false;
}

// 创建并配置服务器套接字
int create_server_socket() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(sock);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SERVER_PORT);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }

    if (listen(sock, SOMAXCONN) < 0) {
        perror("listen");
        close(sock);
        return -1;
    }

    set_nonblocking(sock);
    printf("服务器监听端口 %d\n", SERVER_PORT);
    return sock;
}

// 创建客户端套接字并连接服务器
int create_client_socket(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        printf("无效的 IP 地址: %s\n", ip);
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    set_nonblocking(sock);
    return sock;
}

// 设置非阻塞
void set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

// 通过套接字描述符查找连接下标
int find_connection_by_socket(int socket) {
    for (int i = 0; i < 4; i++) {
        if (g_connections[i].socket == socket) {
            return i;
        }
    }
    return -1;
}

// 通过 IP 与类型查找连接下标
int find_connection_by_ip_and_type(const char* ip, int as_server) {
    for (int i = 0; i < 4; i++) {
        if (strcmp(g_connections[i].ip, ip) == 0 && g_connections[i].as_server == as_server) {
            return i;
        }
    }
    return -1;
}

// 处理新的被动连接
void handle_new_connection(int server_fd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

    if (client_sock < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
        }
        return;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

    // 在 g_connections 中查找匹配的连接
    pthread_mutex_lock(&connections_mutex);
    int conn_index = find_connection_by_ip_and_type(client_ip, 1);

    if (conn_index == -1) {
        printf("拒绝来自未知 IP 的连接: %s\n", client_ip);
        close(client_sock);
        pthread_mutex_unlock(&connections_mutex);
        return;
    }

    // 如有已存在连接则关闭
    if (g_connections[conn_index].socket != -1) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, g_connections[conn_index].socket, NULL);
        close(g_connections[conn_index].socket);
    }

    g_connections[conn_index].socket = client_sock;
    set_nonblocking(client_sock);

    // 加入 epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = client_sock;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &ev);

    // 初始化接收缓冲
    memset(&receive_buffers[conn_index], 0, sizeof(ReceiveBuffer));

    printf("已接受来自 %s 的被动连接（index %d）\n", client_ip, conn_index);
    pthread_mutex_unlock(&connections_mutex);
}

// 处理连接上的数据
void handle_client_data(int conn_index) {
    ReceiveBuffer* rb = &receive_buffers[conn_index];
    int sock = g_connections[conn_index].socket;

    while (true) {
        int bytes_to_read;
        int read_offset;

        if (!rb->header_received) {
            // 读取消息头（2 字节长度）
            bytes_to_read = 2 - rb->received_bytes;
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

        if (!rb->header_received && rb->received_bytes >= 2) {
            // 头部读取完成，解析消息长度
            rb->expected_length = ntohs(*(uint16_t*)rb->data);
            rb->header_received = true;
            rb->received_bytes = 0; // 重置用于读取消息体

            if (rb->expected_length > MAX_MESSAGE_SIZE) {
                printf("消息过大（%d 字节），断开连接\n", rb->expected_length);
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
    pthread_mutex_lock(&connections_mutex);
    printf("连接 %d 已断开\n", conn_index);
    cleanup_connection(conn_index);
    pthread_mutex_unlock(&connections_mutex);
}

// 主动连接远端服务器（用于主动连接项）
bool connect_to_server(int conn_index) {
    if (g_connections[conn_index].as_server == 1) {
        return false; // 非主动连接
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
    memset(&receive_buffers[conn_index], 0, sizeof(ReceiveBuffer));

    printf("已连接到 %s:%d（index %d）\n",
           g_connections[conn_index].ip, g_connections[conn_index].port, conn_index);
    pthread_mutex_unlock(&connections_mutex);

    return true;
}

// 将数据加入发送缓冲链
void add_to_send_buffer(int conn_index, const char* data, int length) {
    SendBuffer* new_buffer = (SendBuffer*)malloc(sizeof(SendBuffer));
    new_buffer->total_length = length + 2; // 包含 2 字节长度头
    new_buffer->data = (char*)malloc(new_buffer->total_length);
    new_buffer->sent_bytes = 0;
    new_buffer->next = NULL;

    // 写入长度头
    uint16_t msg_len = htons(length);
    memcpy(new_buffer->data, &msg_len, 2);
    memcpy(new_buffer->data + 2, data, length);

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

// 发送缓冲链中的数据（处理部分发送）
bool send_buffered_data(int conn_index) {
    int sock = g_connections[conn_index].socket;
    if (sock == -1) return false;

    while (send_buffers[conn_index] != NULL) {
        SendBuffer* buffer = send_buffers[conn_index];
        int remaining = buffer->total_length - buffer->sent_bytes;

        int sent = send(sock, buffer->data + buffer->sent_bytes, remaining, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true; // 会阻塞，稍后再试
            } else {
                return false; // 发生错误
            }
        }

        buffer->sent_bytes += sent;

        if (buffer->sent_bytes >= buffer->total_length) {
            // 当前缓冲已全部发送，移除
            send_buffers[conn_index] = buffer->next;
            free(buffer->data);
            free(buffer);
        }
    }

    return true;
}

// 处理收到的完整消息
void process_received_message(int conn_index, const char* data, int length) {
    printf("收到来自连接 %d 的消息: %.*s\n", conn_index, length, data);
    // 在此加入你的业务处理逻辑
}

// 清理连接（从 epoll 移除、关闭、清空缓冲）
void cleanup_connection(int conn_index) {
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
}

// 连接管理线程——负责主动连接的重连
void* connection_manager_thread(void* arg) {
    while (running) {
        sleep(RECONNECT_INTERVAL);

        pthread_mutex_lock(&connections_mutex);
        for (int i = 0; i < 4; i++) {
            if (g_connections[i].as_server == 0 && g_connections[i].socket == -1) {
                // 尝试重连主动连接
                printf("尝试重连到 %s:%d\n",
                       g_connections[i].ip, g_connections[i].port);
                connect_to_server(i);
            }
        }
        pthread_mutex_unlock(&connections_mutex);
    }
    return NULL;
}

// 发送线程——消费发送队列
void* send_thread(void* arg) {
    while (running) {
        pthread_mutex_lock(&send_queue_mutex);
        if (!send_queue.empty()) {
            Message msg = send_queue.front();
            send_queue.pop();
            pthread_mutex_unlock(&send_queue_mutex);

            pthread_mutex_lock(&connections_mutex);
            if (g_connections[msg.target_index].socket != -1) {
                add_to_send_buffer(msg.target_index, msg.data, msg.length);
            }
            pthread_mutex_unlock(&connections_mutex);

            free(msg.data);
        } else {
            pthread_mutex_unlock(&send_queue_mutex);
            usleep(1000); // 1 毫秒
        }
    }
    return NULL;
}

// 主函数
int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // 初始化接收缓冲
    memset(receive_buffers, 0, sizeof(receive_buffers));

    // 创建服务器套接字
    server_fd = create_server_socket();
    if (server_fd < 0) {
        fprintf(stderr, "创建服务器套接字失败\n");
        return 1;
    }

    // 创建 epoll 实例
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(server_fd);
        return 1;
    }

    // 将服务器套接字加入 epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    // 主动连接远端服务器
    for (int i = 0; i < 4; i++) {
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

    // 主事件循环
    struct epoll_event events[MAX_EVENTS];
    printf("服务器已启动，进入主循环...\n");

    while (running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                // 新的被动连接
                handle_new_connection(server_fd);
            } else {
                // 已有连接上的事件
                int conn_index = find_connection_by_socket(fd);
                if (conn_index != -1) {
                    if (events[i].events & EPOLLIN) {
                        handle_client_data(conn_index);
                    }
                    if (events[i].events & EPOLLOUT) {
                        pthread_mutex_lock(&connections_mutex);
                        if (!send_buffered_data(conn_index)) {
                            handle_client_disconnect(conn_index);
                        }
                        pthread_mutex_unlock(&connections_mutex);
                    }
                    if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                        handle_client_disconnect(conn_index);
                    }
                }
            }
        }
    }

    // 清理
    printf("正在关闭...\n");

    pthread_cancel(conn_manager_tid);
    pthread_cancel(send_tid);
    pthread_join(conn_manager_tid, NULL);
    pthread_join(send_tid, NULL);

    for (int i = 0; i < 4; i++) {
        cleanup_connection(i);
    }

    close(server_fd);
    close(epoll_fd);

    pthread_mutex_destroy(&connections_mutex);
    pthread_mutex_destroy(&send_queue_mutex);

    printf("关闭完成\n");
    return 0;
}