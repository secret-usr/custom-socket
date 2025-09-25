/**
 * test_client.cpp
 * Encoding: UTF-8
 * 
 * 多线程的TCP客户端测试程序，用于测试 socket_comm 程序。
 * - 创建多个客户端并发连接服务器。
 * - 每个客户端独立发送随机长度和内容的报文。
 * - 使用epoll进行I/O多路复用。
 * - 统计每个连接的收发包数和字节数。
 * 要求 socket_comm.cpp 在 process_received_message() 中实现消息回显并配置正确的客户端插槽
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
#include <time.h>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <atomic>

#include "../include/msghead.h"

// --- 配置 ---
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8002
#define NUM_CLIENTS 2          // 模拟的客户端数量
#define MAX_EVENTS 10
#define MAX_MESSAGE_BODY_SIZE 9959 // 测试用最大消息体，覆盖分包情况
#define MIN_MESSAGE_BODY_SIZE 1
#define SEND_INTERVAL_MS 500   // 每个客户端发送消息的平均间隔

// --- 全局变量 ---
static volatile bool g_running = true;

/**
 * @struct ConnectionStats
 * @brief 存储每个连接的统计信息
 */
struct ConnectionStats {
    std::atomic<long long> sent_packets;
    std::atomic<long long> sent_bytes;
    std::atomic<long long> received_packets;
    std::atomic<long long> received_bytes;
};

/**
 * @struct ClientInfo
 * @brief 存储每个客户端线程的信息
 */
struct ClientInfo {
    int id;
    int sock;
    ConnectionStats stats;
    // 接收缓冲
    char* recv_buffer;
    int received_len;
    int expected_body_len;
    bool header_received;
};

// 全局客户端信息数组
ClientInfo g_clients[NUM_CLIENTS];

/**
 * @brief 设置套接字为非阻塞模式
 * @param sock 要设置的套接字描述符
 */
void SetNonBlocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @brief 创建一个客户端套接字并连接到服务器
 * @param ip 服务器IP地址
 * @param port 服务器端口
 * @return int 成功则返回套接字描述符，失败返回-1
 */
int ConnectToServer(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }

    SetNonBlocking(sock);
    return sock;
}

/**
 * @brief 发送一个完整的带报文头的消息
 * @param client 指向客户端信息的指针
 * @param body 指向消息体的指针
 * @param body_len 消息体的长度
 * @return bool 成功发送返回true，失败返回false
 */
bool SendMessage(ClientInfo* client, const char* body, int body_len) {
    if (client->sock < 0) return false;

    int head_len = MsgHead::get_head_length();
    int total_len = head_len + body_len;
    char* message = new char[total_len];

    MsgHead msg_head = {};
    msg_head.random_fill(body_len);
    memcpy(message, &msg_head, head_len);
    memcpy(message + head_len, body, body_len);

    int bytes_sent = 0;
    while (bytes_sent < total_len) {
        int ret = send(client->sock, message + bytes_sent, total_len - bytes_sent, 0);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000); // 稍等后重试
                continue;
            }
            perror("send");
            delete[] message;
            return false;
        }
        bytes_sent += ret;
    }
    
    client->stats.sent_packets++;
    client->stats.sent_bytes += body_len; // 只统计业务数据
    printf("Client %d: Sent packet, body %d bytes.\n", client->id, body_len);

    delete[] message;
    return true;
}

/**
 * @brief 处理从服务器接收到的数据
 * @param client 指向客户端信息的指针
 */
void HandleReceive(ClientInfo* client) {
    int head_len = MsgHead::get_head_length();
    
    while (true) {
        int bytes_to_read;
        if (!client->header_received) {
            bytes_to_read = head_len - client->received_len;
        } else {
            bytes_to_read = client->expected_body_len - (client->received_len - head_len);
        }

        if (bytes_to_read <= 0) break; // 数据已完整或计算错误

        int ret = recv(client->sock, client->recv_buffer + client->received_len, bytes_to_read, 0);

        if (ret < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recv");
                g_running = false; // 发生错误，终止程序
            }
            break; // 缓冲区无数据可读
        }
        if (ret == 0) {
            printf("Client %d: Server closed connection.\n", client->id);
            g_running = false; // 服务器关闭，终止程序
            break;
        }

        client->received_len += ret;

        if (!client->header_received && client->received_len >= head_len) {
            MsgHead* mh = (MsgHead*)client->recv_buffer;
            client->expected_body_len = mh->get_body_length();
            client->header_received = true;
            if (client->expected_body_len > MAX_MESSAGE_BODY_SIZE * 2) { // 简单校验
                 printf("Client %d: Abnormal body length %d, closing.\n", client->id, client->expected_body_len);
                 g_running = false;
                 break;
            }
        }

        if (client->header_received && client->received_len >= head_len + client->expected_body_len) {
            // 收到完整消息
            int body_len = client->expected_body_len;
            client->stats.received_packets++;
            client->stats.received_bytes += body_len;
            printf("Client %d: Received packet, body %d bytes.\n", client->id, body_len);

            // 处理粘包：将剩余数据移到缓冲区头部
            int remaining_len = client->received_len - (head_len + body_len);
            if (remaining_len > 0) {
                memmove(client->recv_buffer, client->recv_buffer + head_len + body_len, remaining_len);
            }
            client->received_len = remaining_len;
            client->header_received = false;
            client->expected_body_len = 0;
            // 继续循环处理缓冲区中的下一个包
        }
    }
}

/**
 * @brief 客户端工作线程函数
 * @param arg 指向ClientInfo的指针
 * @return void* NULL
 */
void* ClientWorker(void* arg) {
    ClientInfo* client = (ClientInfo*)arg;
    
    // 初始化随机数种子
    srand(time(NULL) ^ client->id);

    while (g_running) {
        int body_len = (rand() % (MAX_MESSAGE_BODY_SIZE - MIN_MESSAGE_BODY_SIZE + 1)) + MIN_MESSAGE_BODY_SIZE;
        char* body = new char[body_len];
        // 生成随机内容
        for (int i = 0; i < body_len; ++i) {
            body[i] = rand() % 256;
        }

        if (!SendMessage(client, body, body_len)) {
            printf("Client %d: Failed to send message. Exiting.\n", client->id);
            delete[] body;
            break;
        }
        
        delete[] body;

        // 随机化发送间隔
        int sleep_ms = (rand() % (SEND_INTERVAL_MS / 2)) + (SEND_INTERVAL_MS / 2);
        usleep(sleep_ms * 1000);
    }
    return NULL;
}

/**
 * @brief 信号处理函数，用于优雅地关闭程序
 * @param sig 信号编号
 */
void SignalHandler(int sig) {
    printf("\nCaught signal %d, shutting down...\n", sig);
    g_running = false;
}

int main() {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    pthread_t threads[NUM_CLIENTS];
    int num_threads_created = 0;

    for (int i = 0; i < NUM_CLIENTS; ++i) {
        g_clients[i].id = i;
        g_clients[i].sock = ConnectToServer(SERVER_IP, SERVER_PORT);
        if (g_clients[i].sock < 0) {
            fprintf(stderr, "Failed to connect client %d\n", i);
            g_running = false;
            break;
        }
        printf("Client %d connected to %s:%d with socket %d.\n", i, SERVER_IP, SERVER_PORT, g_clients[i].sock);

        g_clients[i].stats.sent_packets = 0;
        g_clients[i].stats.sent_bytes = 0;
        g_clients[i].stats.received_packets = 0;
        g_clients[i].stats.received_bytes = 0;
        g_clients[i].recv_buffer = new char[MAX_MESSAGE_BODY_SIZE * 2]; // 足够大的接收缓冲
        g_clients[i].received_len = 0;
        g_clients[i].expected_body_len = 0;
        g_clients[i].header_received = false;

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET; // 边缘触发
        ev.data.ptr = &g_clients[i];
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_clients[i].sock, &ev) < 0) {
            perror("epoll_ctl");
            g_running = false;
            break;
        }

        if (pthread_create(&threads[i], NULL, ClientWorker, &g_clients[i]) != 0) {
            perror("pthread_create");
            g_running = false;
            break;
        }
        num_threads_created++;
    }

    struct epoll_event events[MAX_EVENTS];
    while (g_running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            ClientInfo* client = (ClientInfo*)events[i].data.ptr;
            if (events[i].events & EPOLLIN) {
                HandleReceive(client);
            }
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                printf("Client %d: Connection error or hang-up.\n", client->id);
                g_running = false;
            }
        }
    }

    // --- 清理 ---
    printf("Stopping client threads...\n");
    for (int i = 0; i < num_threads_created; ++i) {
        pthread_join(threads[i], NULL);
    }

    close(epoll_fd);
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        if (g_clients[i].sock >= 0) {
            close(g_clients[i].sock);
        }
        delete[] g_clients[i].recv_buffer;
    }

    // --- 打印统计信息 ---
    printf("\n--- Final Statistics ---\n");
    ConnectionStats total_stats = {0, 0, 0, 0};
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        printf("Client %d:\n", i);
        printf("  - Sent: %lld packets, %lld bytes\n", g_clients[i].stats.sent_packets.load(), g_clients[i].stats.sent_bytes.load());
        printf("  - Received: %lld packets, %lld bytes\n", g_clients[i].stats.received_packets.load(), g_clients[i].stats.received_bytes.load());
        total_stats.sent_packets += g_clients[i].stats.sent_packets;
        total_stats.sent_bytes += g_clients[i].stats.sent_bytes;
        total_stats.received_packets += g_clients[i].stats.received_packets;
        total_stats.received_bytes += g_clients[i].stats.received_bytes;
    }
    printf("--------------------------\n");
    printf("Total:\n");
    printf("  - Sent: %lld packets, %lld bytes\n", total_stats.sent_packets.load(), total_stats.sent_bytes.load());
    printf("  - Received: %lld packets, %lld bytes\n", total_stats.received_packets.load(), total_stats.received_bytes.load());
    printf("--------------------------\n");

    printf("Tester finished.\n");
    return 0;
}