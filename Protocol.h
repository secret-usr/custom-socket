#pragma once

#include <string>
#include <cstdio>
#include <stdexcept>

class Protocol {
public:
    // TryDecode 返回 0 代表不完整；-1 代表错误；>0 代表完整包的长度
    static int TryDecode(const std::string& buf, std::string& out_msg) {
        if (buf.size() < 4) return 0;
        try {
            int len = std::stoi(buf.substr(0, 4));
            if (buf.size() < 4 + (size_t)len) return 0;
            out_msg = buf.substr(4, len);
            return 4 + len;
        } catch (...) { return -1; }
    }

    static void Encode(const std::string& msg, std::string& out_buf) {
        char len_str[5];
        snprintf(len_str, 5, "%04lu", msg.size());
        out_buf.append(len_str);
        out_buf.append(msg);
    }
};
