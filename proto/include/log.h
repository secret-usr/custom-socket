#ifndef LOG_H_
#define LOG_H_

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <algorithm>

// ================ 日志宏定义 =================
enum LogLevel { LOG_ERR=0, LOG_WARN=1, LOG_INFO=2, LOG_DBG=3 };

// ================= 编译期日志等级控制 =========
// 优先级从低到高：ERR(0) < WARN(1) < INFO(2) < DBG(3)
// 用法：
//  1) 直接指定最大等级：  g++ -DLOG_LEVEL=LOG_INFO ...   -> 打印 E/W/I
//  2) 使用语义宏：        -DDEBUG / -DINFO / -DWARN / -DERROR
//     -DDEBUG 等价 LOG_LEVEL=LOG_DBG
//     -DINFO  等价 LOG_LEVEL=LOG_INFO
//     -DWARN  等价 LOG_LEVEL=LOG_WARN
//     -DERROR 等价 LOG_LEVEL=LOG_ERR
// 未传任何宏，则默认采用 LOG_INFO
#ifndef LOG_LEVEL
    #if defined(DEBUG)
        #define LOG_LEVEL LOG_DBG
    #elif defined(INFO)
        #define LOG_LEVEL LOG_INFO
    #elif defined(WARN)
        #define LOG_LEVEL LOG_WARN
    #elif defined(ERROR)
        #define LOG_LEVEL LOG_ERR
    #else
        #define LOG_LEVEL LOG_INFO
    #endif
#endif

// 判断某个级别是否启用，让编译器可做常量折叠，优化未启用分支
#define LOG_LEVEL_ENABLED(lvl) ((lvl) <= LOG_LEVEL)

static inline const char* LOG_TIMESTAMP() {
    static char buf[32];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    int ms = ts.tv_nsec / 1000000;
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    return buf;
}

#define LOG_BASE(lvl, lvlstr, fmt, ...)                                            \
    do {                                                                           \
        if (LOG_LEVEL_ENABLED(lvl)) {                                              \
            if ((lvl) == LOG_DBG) {                                                \
                fprintf(stderr, "[%s][%s][%s][T%lu][%s:%d] " fmt "\n",           \
                        LOG_TIMESTAMP(), lvlstr, __func__,                         \
                        (unsigned long)pthread_self(), __FILE__, __LINE__,         \
                        ##__VA_ARGS__);                                            \
            } else {                                                               \
                fprintf(stderr, "[%s][%s][%s] " fmt "\n",                        \
                        LOG_TIMESTAMP(), lvlstr, __func__, ##__VA_ARGS__);         \
            }                                                                      \
            fflush(stderr);                                                        \
        }                                                                          \
    } while(0)

#define LOGE(fmt, ...) LOG_BASE(LOG_ERR , "ERR", fmt, ##__VA_ARGS__)        // 错误
#define LOGW(fmt, ...) LOG_BASE(LOG_WARN, "WRN", fmt, ##__VA_ARGS__)        // 警告
#define LOGI(fmt, ...) LOG_BASE(LOG_INFO, "INF", fmt, ##__VA_ARGS__)        // 信息
#define LOGD(fmt, ...) LOG_BASE(LOG_DBG , "DBG", fmt, ##__VA_ARGS__)        // 调试信息
#define LOG_SYSERR(msg) LOGE("%s: (%d) %s", msg, errno, strerror(errno))    // 用于替换 perror()

// =============================================

// 十六进制转储，把 buf 的前 len 字节转换为十六进制字符串
// 用例：LOGI("HEX: %s", HEX_DUMP_N(buf, len, 32));
static inline const char* hex_dump_tls(const void* data, size_t len, size_t max_bytes) {
    static thread_local char bufs[4][3 * 128 + 8]; // 预览最多 128 字节
    static thread_local int idx = 0;
    char* out = bufs[idx];
    idx = (idx + 1) & 3;

    const unsigned char* p = static_cast<const unsigned char*>(data);
    size_t n = len < max_bytes ? len : max_bytes;
    if (n > 128) n = 128;

    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        if (pos + 3 >= sizeof(bufs[0])) break;
        pos += snprintf(out + pos, sizeof(bufs[0]) - pos, "%02X", p[i]);
        if (i + 1 < n && pos + 2 < sizeof(bufs[0])) {
            out[pos++] = ' ';
        }
    }
    if (n < len && pos + 5 < sizeof(bufs[0])) {
        strcpy(out + pos, " ...");
    } else {
        out[pos] = '\0';
    }
    return out;
}

// 便捷宏：预览全部（可能受到 128 字节截断）
#define HEX_DUMP(ptr, len)        hex_dump_tls((ptr), (len), (len))
// 便捷宏：手动限制预览长度
#define HEX_DUMP_N(ptr, len, max) hex_dump_tls((ptr), (len), (max))

#endif // LOG_H_