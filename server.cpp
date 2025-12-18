#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <memory>
#include "ThreadPool.h"
#include "Protocol.h"
#include "Connection.h"

struct Config {
    int worker_num = 4;
    int port = 8080;
    int keep_alive_timeout = 120000;
};

std::string ProcessBusiness(const std::string& request) {
    if (request.rfind("REVERSE:", 0) == 0) {
        std::string content = request.substr(8);
        std::reverse(content.begin(), content.end());
        return "REVERSE:" + content;
    } else {
        return "LEN:" + std::to_string(request.length());
    }
}

class Worker {
    int listen_fd;
    Config config;
    int epoll_fd;
    int wakeup_fd;
    std::unordered_map<int, std::shared_ptr<Connection>> connections;
    ThreadPool thread_pool;

public:
    Worker(int lfd, const Config& cfg) 
        : listen_fd(lfd), config(cfg), thread_pool(4) {
        wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd == -1) {
            perror("eventfd");
            exit(EXIT_FAILURE);
        }
    }

    void Run() {
        std::cout << "Worker process " << getpid() << " started." << std::endl;
        // 1. 初始化 epoll
        epoll_fd = epoll_create1(0);
        if (epoll_fd == -1) {
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }
        struct epoll_event ev, events[1024];

        // 2. 将监听 socket 加入 epoll
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
            perror("epoll_ctl: listen_fd");
            exit(EXIT_FAILURE);
        }

        // 3. 将 wakeup_fd 加入 epoll
        ev.events = EPOLLIN;
        ev.data.fd = wakeup_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wakeup_fd, &ev) == -1) {
            perror("epoll_ctl: wakeup_fd");
            exit(EXIT_FAILURE);
        }

        while (true) {
            int nfds = epoll_wait(epoll_fd, events, 1024, 1000);
            if (nfds == -1) {
                if (errno == EINTR) continue;
                perror("epoll_wait");
                break;
            }

            for (int i = 0; i < nfds; ++i) {
                int fd = events[i].data.fd;
                if (fd == listen_fd) {
                    AcceptConnection();
                } else if (fd == wakeup_fd) {
                    uint64_t val;
                    read(wakeup_fd, &val, sizeof(val));
                    for (auto& kv : connections) {
                        ProcessWriteQueue(kv.second);
                        HandleWrite(kv.second);
                    }
                } else {
                    auto it = connections.find(fd);
                    if (it == connections.end()) continue;
                    
                    std::shared_ptr<Connection> conn = it->second;
                    
                    if (events[i].events & EPOLLIN) {
                        if (!HandleRead(conn)) continue;
                        if (!ProcessReadBuffer(conn)) continue;
                        ProcessTasks(conn);
                    }
                    if (events[i].events & EPOLLOUT) {
                        HandleWrite(conn);
                    }
                }
            }
            CleanInactiveConnections();
        }
    }

private:
    void AcceptConnection() {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd > 0) {
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = client_fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                perror("epoll_ctl: client_fd");
                close(client_fd);
                return;
            }

            connections[client_fd] = std::make_shared<Connection>(client_fd);
            std::cout << "Worker " << getpid() << " accepted connection fd: " << client_fd << std::endl;
        }
    }

    bool HandleRead(std::shared_ptr<Connection> conn) {
        char buf[4096];
        while (true) {
            ssize_t n = read(conn->fd, buf, sizeof(buf));
            if (n > 0) {
                conn->byte_read_buf.append(buf, n);
                conn->last_active = time(nullptr);
                std::cout << "Worker " << getpid() << " read " << n << " bytes. Buffer size: " << conn->byte_read_buf.size() << std::endl;
            } else if (n == -1 && errno == EAGAIN) {
                break;
            } else {
                std::cout << "Worker " << getpid() << " read error or EOF. n=" << n << " errno=" << errno << std::endl;
                CloseConnection(conn);
                return false;
            }
        }
        return true;
    }

    void HandleWrite(std::shared_ptr<Connection> conn) {
        if (conn->byte_write_buf.empty()) return;
        
        ssize_t n = write(conn->fd, conn->byte_write_buf.data(), conn->byte_write_buf.size());
        if (n > 0) {
            conn->byte_write_buf.erase(0, n);
            conn->last_active = time(nullptr);
        }
        
        struct epoll_event ev;
        ev.data.fd = conn->fd;
        if (!conn->byte_write_buf.empty()) {
            ev.events = EPOLLIN | EPOLLOUT;
        } else {
            ev.events = EPOLLIN;
        }
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
    }

    void CloseConnection(std::shared_ptr<Connection> conn) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, nullptr);
        close(conn->fd);
        connections.erase(conn->fd);
    }

    void CleanInactiveConnections() {
        time_t now = time(nullptr);
        for (auto it = connections.begin(); it != connections.end(); ) {
            if (difftime(now, it->second->last_active) > config.keep_alive_timeout / 1000) {
                std::shared_ptr<Connection> conn = it->second;
                it = connections.erase(it);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, nullptr);
                close(conn->fd);
            } else {
                ++it;
            }
        }
    }

    bool ProcessReadBuffer(std::shared_ptr<Connection> conn) {
        std::string msg;
        int len;
        while (true) {
            len = Protocol::TryDecode(conn->byte_read_buf, msg);
            if (len > 0) {
                conn->byte_read_buf.erase(0, len);
                conn->msg_read_queue.push(msg);
                std::cout << "Worker " << getpid() << " received complete message: " << msg << std::endl;
            } else if (len == 0) {
                break;
            } else {
                std::cout << "Worker " << getpid() << " protocol error. Closing connection." << std::endl;
                CloseConnection(conn);
                return false;
            }
        }
        return true;
    }

    void ProcessTasks(std::shared_ptr<Connection> conn) {
        while (!conn->msg_read_queue.empty()) {
            std::string msg = conn->msg_read_queue.front();
            conn->msg_read_queue.pop();
            CreateTask(conn, msg);
        }
    }

    void CreateTask(std::shared_ptr<Connection> conn, std::string msg) {
        int wfd = wakeup_fd;
        auto task = [conn, msg, wfd]() {
            std::string response = ProcessBusiness(msg);
            {
                std::lock_guard<std::mutex> lock(conn->msg_write_mtx);
                conn->msg_write_queue.push(response);
            }
            uint64_t val = 1;
            write(wfd, &val, sizeof(val));
        };
        thread_pool.Push(task);
    }

    void ProcessWriteQueue(std::shared_ptr<Connection> conn) {
        std::lock_guard<std::mutex> lock(conn->msg_write_mtx);
        while (!conn->msg_write_queue.empty()) {
            std::string msg = conn->msg_write_queue.front();
            conn->msg_write_queue.pop();
            std::cout << "Worker " << getpid() << " sending message: " << msg << std::endl;
            Protocol::Encode(msg, conn->byte_write_buf);
        }
    }
};

void WorkerProcess(int listen_fd, const Config& config) {
    Worker worker(listen_fd, config);
    worker.Run();
}

int main() {
    Config config;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config.port);
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        return 1;
    }
    if (listen(listen_fd, 128) == -1) {
        perror("listen");
        return 1;
    }

    std::cout << "Server listening on port " << config.port << " with " << config.worker_num << " workers." << std::endl;

    std::vector<pid_t> workers;
    for (int i = 0; i < config.worker_num; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            WorkerProcess(listen_fd, config);
            return 0;
        } else if (pid > 0) {
            workers.push_back(pid);
        } else {
            perror("fork");
        }
    }

    while (wait(NULL) > 0);
    return 0;
}
