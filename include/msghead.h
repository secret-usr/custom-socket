#ifndef MSGHEAD_H_
#define MSGHEAD_H_

#include <stdint.h>

// 电文头的定义
struct MsgHead
{
    // 电文头的定义可根据实际需求扩展
    uint16_t msglen;
    
    // 以下一些辅助函数，适用于不同的电文头定义
    // 获取电文体的大小
    int get_body_length()
    {
        return msglen - sizeof(MsgHead);
    }
    // 获取电文头的大小
    static constexpr int get_head_length()
    {
        return sizeof(MsgHead);
    }
};

#endif // MSGHEAD_H_