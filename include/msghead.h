#ifndef MSGHEAD_H_
#define MSGHEAD_H_

#include <stdint.h>
#include <arpa/inet.h> // For ntohs

// 电文头的定义
struct MsgHead
{
    // 电文头的定义可根据实际需求扩展
    // 电文长度，不包括电文头，以网络字节序存储
    uint16_t msglen;
    
    // 以下一些辅助函数，根据不同电文头的定义进行扩展
    // 获取电文体的大小
    int get_body_length()
    {
        return ntohs(msglen);
    }
    // 获取电文头的大小
    static constexpr int get_head_length()
    {
        return sizeof(MsgHead);
    }
};

#endif // MSGHEAD_H_