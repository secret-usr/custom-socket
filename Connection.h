#pragma once

#include <string>
#include <queue>
#include <mutex>
#include <ctime>

struct Connection {
    int fd;
    
    // 字节流缓冲
    std::string byte_read_buf;  // 接收到的原始字节
    std::string byte_write_buf; // 等待发送的原始字节

    // 报文缓冲
    std::queue<std::string> msg_read_queue; // 解析完成的报文体
    std::queue<std::string> msg_write_queue; // 待发送的报文体
    std::mutex msg_write_mtx; // msg_write_queue 互斥访问

    time_t last_active;

    Connection(int f) : fd(f) {
        last_active = time(nullptr);
    }
};
