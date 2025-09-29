#ifndef MSGHEAD_H_
#define MSGHEAD_H_

#include <stdint.h>
#include <arpa/inet.h> // For ntohs
#include <time.h>      // For time() and strftime()

// 电文头的定义
struct MsgHead
{
    // 电文头的定义可根据实际需求扩展
    // 1 电文长度   整个电文的长度，十进制字符串，包括电文头、电文内容、结束符  C4  "1024", "0066"
    char length[4];
    // 2 电文号     用户定义的电文标识，用于区分同一种用途的电文，不能含有空格或串结束符    C4  "AA01", "kknn" 
    char msgid[4];
    // 3 日期   电文发送的日期，八位字符串，依次表示年、月、日  C8  YYYYMMDD
    char date[8];
    // 4 时间   电文发送的时间，六位字符串，依次表示时、分、秒，二十四小时制  C6  HHMMSS
    char time[6];
    // 5 发送端DC   发送端主机的描述码，二位字符串，不能含有空格或串结束符  C2  "L4"
    char senddc[2];
    // 6 接收端DC   接收端主机的描述码，二位字符串，不能含有空格或串结束符  C2  "L3"
    char recvdc[2];
    // 7 序列号     该字段用于区分在相同时间段内传输的不同电文，应用可以通过该字段加上时间日期来唯一确定某条电文。该字段为十进制字符，可以不连续 C6
    char seqno[6];
    // 8 保留域     该字段为保留字段，暂未使用，置空 C8
    char spare[8];
    

    // 以下一些成员函数，根据不同电文头的定义进行扩展

    // 获取电文体的大小
    int get_body_length()
    {
        return string_to_int(length) - get_head_length();
    }
    // 获取电文头的大小
    static constexpr int get_head_length()
    {
        return sizeof(MsgHead);
    }
    // 随机填充符合条件的电文头，用于测试，参数为电文体长度
    void random_fill(int body_length)
    {
        char temp_string[16];
        // 填充 length 字段
        int total_length = get_head_length() + body_length;
        total_length = total_length > 9999 ? 9999 : total_length;  // 限制最大值为 9999
        snprintf(temp_string, sizeof(temp_string), "%04d", total_length);
        memcpy(length, temp_string, sizeof(length));
        // 填充 msgid 字段
        memcpy(msgid, "TEST", sizeof(msgid));
        // 使用当前系统时间填充 date 和 time 字段
        time_t now = ::time(nullptr);
        struct tm* tstruct = localtime(&now);
        strftime(temp_string, sizeof(date) + 1, "%Y%m%d", tstruct);
        memcpy(date, temp_string, sizeof(date));
        strftime(temp_string, sizeof(time) + 1, "%H%M%S", tstruct);
        memcpy(time, temp_string, sizeof(time));
        // 填充 senddc 字段
        memcpy(senddc, "L3", sizeof(senddc));
        // 填充 recvdc 字段
        memcpy(recvdc, "L2", sizeof(recvdc));
        // 填充 seqno 字段
        memcpy(seqno, "114514", sizeof(seqno));
        // 填充 spare 字段
        memcpy(spare, "        ", sizeof(spare));
    }
    // 指示字段 length 是否包括电文头
    static constexpr bool includes_header() {
        return true;
    }
    // 指示字段 length 是否包括结束符
    static constexpr bool includes_terminator() {
        return true;
    }

    
    // 以下为辅助函数，用于成员函数的实现

    // 字符组成的十进制字符串转换为对应的数字，输入字符串仅包含 '0'-'9'，长度为4
    // 例如："1024" -> 1024
    static inline int string_to_int(const char* str) {
        int num = 0;
        for (int i = 0; i < 4; ++i) {
            num = num * 10 + (str[i] - '0');
        }
        return num;
    }
};

#endif // MSGHEAD_H_