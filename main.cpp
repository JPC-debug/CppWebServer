#include <iostream>
#include <string>
#include <cstdio>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sstream>
#include <sys/epoll.h>
#include <cerrno>
#include <fcntl.h>
#include <unordered_map>
#include "HttpHandler.h"
#include "ThreadPool.h"

#include <mutex>
#include <queue>
#include <cstdint>
#include <sys/eventfd.h>
#include <unordered_set>

#include <exception>
#include <utility>

#include "MySQLClient.h"
#include <cstdlib>
#include <stdexcept>

#include "Logger.h"

#include <atomic>
#include <signal.h>
#include <sys/signalfd.h>

using namespace std;

class MySQLLibraryGuard {
public:
    MySQLLibraryGuard() {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            throw runtime_error("MySQL library initialization failed");
        }
        cout << "MySQL library initialized!" << endl;
    }

    ~MySQLLibraryGuard() {
        mysql_library_end();
        cout << "MySQL library ended!" << endl;
    }

    MySQLLibraryGuard(const MySQLLibraryGuard&) = delete;
    MySQLLibraryGuard& operator=(const MySQLLibraryGuard&) = delete;
};

bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        return false;
    }

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

struct HttpResult {
    int client_fd;
    uint64_t connection_id;
    std::string response;
};

// 工作线程任务计数器
struct ActiveTaskGuard {
    std::atomic<size_t>& counter;

    explicit ActiveTaskGuard(std::atomic<size_t>& c)
        : counter(c) {}

    ~ActiveTaskGuard() {
        counter.fetch_sub(1, std::memory_order_release);
    }
};

// 工作线程与主线程共享的结果队列
std::queue<HttpResult> result_queue;

// 保护结果队列的互斥锁
std::mutex result_mutex;

int main() {

    // 阻塞 SIGINT 和 SIGTERM
    sigset_t signal_mask;

    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGTERM);

    if (sigprocmask(
            SIG_BLOCK,
            &signal_mask,
            nullptr
        ) == -1) {

        perror("sigprocmask");
        return 1;
    }

    // 1. 创建 TCP Socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return 1;
    }

    // 允许服务器重启后尽快重新绑定端口
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
               &opt, sizeof(opt));

    // 2. 配置服务器地址
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(8080);

    // 3. 绑定 IP 和端口
    if (bind(server_fd, (sockaddr*)&address,
             sizeof(address)) == -1) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    // 4. 监听客户端连接
    if (listen(server_fd, 128) == -1) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    cout << "Server listening on 127.0.0.1:8080" << endl;

if (!setNonBlocking(server_fd)) {
    perror("setNonBlocking");
    close(server_fd);
    return 1;
}

// 创建 epoll 实例
int epoll_fd = epoll_create1(0);

if (epoll_fd == -1) {
    perror("epoll_create1");
    close(server_fd);
    return 1;
}

// 创建 eventfd
int result_event_fd = eventfd(
    0,
    EFD_NONBLOCK | EFD_CLOEXEC
);

if (result_event_fd == -1) {
    perror("eventfd");
    close(epoll_fd);
    close(server_fd);
    return 1;
}

// 将 eventfd 注册到 epoll
epoll_event result_event{};

result_event.events = EPOLLIN;
result_event.data.fd = result_event_fd;

if (epoll_ctl(
        epoll_fd,
        EPOLL_CTL_ADD,
        result_event_fd,
        &result_event
    ) == -1) {

    perror("epoll_ctl eventfd");

    close(result_event_fd);
    close(epoll_fd);
    close(server_fd);

    return 1;
}

cout << "eventfd initialized!" << endl;

// 创建 signalfd
int signal_fd = signalfd(
    -1,
    &signal_mask,
    SFD_NONBLOCK | SFD_CLOEXEC
);

if (signal_fd == -1) {
    perror("signalfd");
    close(result_event_fd);
    close(epoll_fd);
    close(server_fd);
    return 1;
}

// 注册到 epoll
epoll_event signal_event{};

signal_event.events = EPOLLIN;
signal_event.data.fd = signal_fd;

if (epoll_ctl(
        epoll_fd,
        EPOLL_CTL_ADD,
        signal_fd,
        &signal_event
    ) == -1) {

    perror("epoll_ctl signal_fd");

    close(signal_fd);
    close(result_event_fd);
    close(epoll_fd);
    close(server_fd);

    return 1;
}

cout << "Signal handling initialized!" << endl;

// 创建事件对象
epoll_event event{};

// 关注可读事件
event.events = EPOLLIN;

// 保存服务器 Socket 描述符
event.data.fd = server_fd;

// 将服务器 Socket 注册到 epoll
if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
              server_fd, &event) == -1) {
    perror("epoll_ctl");
    close(signal_fd);
    close(result_event_fd);
    close(epoll_fd);
    close(server_fd);
    return 1;
}

cout << "epoll initialized!" << endl;

epoll_event events[64];
unordered_map<int, string> request_buffers;
// 每个客户端待发送的 HTTP 响应
unordered_map<int, string> response_buffers;

// 每个客户端已经发送的字节数
unordered_map<int, size_t> send_offsets;
// 每个客户端对应的唯一连接编号
unordered_map<int, uint64_t> connection_ids;

// 下一个连接编号
uint64_t next_connection_id = 1;

// 正在由工作线程处理的客户端
unordered_set<int> processing_fds;

// 1. 初始化 MySQL 全局库
MySQLLibraryGuard mysql_library;

// 2. 获取数据库密码
const char* mysql_password = getenv("MYSQL_PASSWORD");

if (mysql_password == nullptr) {
    cerr << "MYSQL_PASSWORD is not set!" << endl;
    return 1;
}

// 3. 创建 MySQL 连接池
MySQLConnectionPool mysql_pool(
    4,
    "127.0.0.1",
    "webserver_app",
    mysql_password,
    "webserver_db"
);

// 4. 创建 MySQL 客户端
MySQLClient mysql_client(mysql_pool);

// 5. 创建 Redis 客户端
RedisClient redis_client;

// 记录正在执行的 HTTP 任务数量
std::atomic<size_t> active_tasks{0};

// 是否正在关闭服务器
bool shutting_down = false;

ThreadPool pool(4);

cout << "ThreadPool initialized!" << endl;

while (true) {
    // 1. 等待就绪事件
    int count = epoll_wait(epoll_fd, events, 64, -1);

    if (count == -1) {
        if (errno == EINTR) continue;
        perror("epoll_wait");
        break;
    }

// 优先处理关闭信号，避免同一批事件中继续接受新连接
bool received_signal = false;

for (int i = 0; i < count; i++) {

    if (events[i].data.fd != signal_fd) {
        continue;
    }

    signalfd_siginfo info{};

    while (true) {

        ssize_t n = read(
            signal_fd,
            &info,
            sizeof(info)
        );

        if (n == sizeof(info)) {
            received_signal = true;
            continue;
        }

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n == -1 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        if (n == -1) {
            perror("read signalfd");
        }

        break;
    }
}

if (received_signal && !shutting_down) {

    shutting_down = true;

    logInfo("Shutdown signal received!");

    // 停止接受新的客户端连接
    epoll_ctl(
        epoll_fd,
        EPOLL_CTL_DEL,
        server_fd,
        nullptr
    );

    logInfo("Stopped accepting new clients!");
}

    // 2. 遍历所有就绪事件
    for (int i = 0; i < count; i++) {
        int fd = events[i].data.fd;
        if (fd == signal_fd) {
            continue;
        }
        // 情况一：有新的客户端连接
        if (fd == server_fd) {

            if (shutting_down) {
                continue;
            }
            while (true) {
                int client_fd = accept(server_fd, nullptr, nullptr);

                if (client_fd == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    if (errno == EINTR)
                        continue;

                    perror("accept");
                    break;
                }

                // 将客户端设置为非阻塞
                if (!setNonBlocking(client_fd)) {
                    close(client_fd);
                    continue;
                }

                // 将客户端加入 epoll
                epoll_event client_event{};
                client_event.events = EPOLLIN | EPOLLRDHUP;
                client_event.data.fd = client_fd;

                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
                              client_fd, &client_event) == -1) {
                    perror("epoll_ctl client");
                    close(client_fd);
                    continue;
                }
                // 为新客户端分配唯一连接编号
                connection_ids[client_fd] = next_connection_id++;
                logInfo("New client registered: ", client_fd);
            }
        }

// 情况二：工作线程完成任务，eventfd 通知主线程
        else if (fd == result_event_fd) {

            uint64_t value;

            // 读取 eventfd 通知
            while (true) {

                ssize_t n = read(
                    result_event_fd,
                    &value,
                    sizeof(value)
                );

                if (n == sizeof(value)) {
                    break;
                }

                if (n == -1 && errno == EINTR) {
                    continue;
                }

                if (n == -1 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }

                if (n == -1) {
                    perror("read eventfd");
                }

                break;
            }

            // 取出所有已经完成的 HTTP 任务
            queue<HttpResult> completed;

            {
                lock_guard<mutex> lock(result_mutex);
                completed.swap(result_queue);
            }

            // 逐个处理工作线程返回的结果
            while (!completed.empty()) {

                HttpResult result = std::move(completed.front());
                completed.pop();

                int client_fd = result.client_fd;

                // 检查客户端是否仍然存在，以及连接编号是否匹配
                auto it = connection_ids.find(client_fd);

                if (it == connection_ids.end() ||
                    it->second != result.connection_id) {

                    logInfo("Discarding stale result: ", client_fd);
                    continue;
                }

                // 工作线程已经处理完毕
                processing_fds.erase(client_fd);

                // 保存 HTTP 响应
                response_buffers[client_fd] =
                    std::move(result.response);

                send_offsets[client_fd] = 0;

                // 通知 epoll：客户端 Socket 现在可以发送数据
                epoll_event write_event{};

                write_event.events = EPOLLOUT | EPOLLRDHUP;
                write_event.data.fd = client_fd;

                if (epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_MOD,
                        client_fd,
                        &write_event
                    ) == -1) {

                    perror("epoll_ctl result");

                    epoll_ctl(
                        epoll_fd,
                        EPOLL_CTL_DEL,
                        client_fd,
                        nullptr
                    );

                    close(client_fd);

                    request_buffers.erase(client_fd);
                    response_buffers.erase(client_fd);
                    send_offsets.erase(client_fd);
                    connection_ids.erase(client_fd);
                    processing_fds.erase(client_fd);
                }

            }
        }

        // 情况二：已有客户端产生事件
        else {
            bool should_close = false;
            bool request_complete = false;

            string request;


            // 收到关闭信号后，不再处理新的客户端请求
            if (shutting_down &&
                !processing_fds.count(fd) &&
                !response_buffers.count(fd)) {

                should_close = true;
            }


            // 1. 客户端可读
            if (!shutting_down && (events[i].events & EPOLLIN)) {
                char buffer[4096];

                // 非阻塞模式：读取当前能够读取的数据
                while (true) {
                    ssize_t n = recv(fd, buffer, sizeof(buffer), 0);

                    if (n > 0) {
                        // 追加到这个客户端自己的缓冲区
                        request_buffers[fd].append(buffer, n);

                        // 判断 HTTP 请求头是否接收完整
                        if (request_buffers[fd].find("\r\n\r\n")
                            != string::npos) {

                            request_complete = true;
                            request = request_buffers[fd];
                            break;
                        }

                        // 防止请求头无限增长
                        if (request_buffers[fd].size() > 8192) {
                            cout << "Request header too large!" << endl;
                            should_close = true;
                            break;
                        }
                    }
                    else if (n == 0) {
                        // 客户端已经关闭连接
                        should_close = true;
                        break;
                    }
                    else if (errno == EINTR) {
                        continue;
                    }
                    else if (errno == EAGAIN ||
                            errno == EWOULDBLOCK) {
                        // 暂时没有数据，等待下一次 epoll 通知
                        break;
                    }
                    else {
                        perror("recv");
                        should_close = true;
                        break;
                    }
                }
            }



            // 2. 只有请求完整，才提交给线程池
            if (request_complete && !should_close) {

                // 获取当前客户端的唯一连接编号
                auto id_it = connection_ids.find(fd);

                if (id_it == connection_ids.end()) {
                    should_close = true;
                }
                else {

                    uint64_t connection_id = id_it->second;

                    // 标记当前客户端正在被工作线程处理
                    processing_fds.insert(fd);

                    // 暂时关闭 EPOLLIN
                    epoll_event processing_event{};
                    processing_event.events = EPOLLRDHUP;
                    processing_event.data.fd = fd;

                    if (epoll_ctl(
                            epoll_fd,
                            EPOLL_CTL_MOD,
                            fd,
                            &processing_event
                        ) == -1) {

                        perror("epoll_ctl processing");

                        processing_fds.erase(fd);
                        should_close = true;
                    }
                    else {

                        // 增加正在执行的任务数量
                        active_tasks.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );

                        try {

                            pool.enqueue([
                                request,
                                fd,
                                connection_id,
                                result_event_fd,
                                &redis_client,
                                &mysql_client,
                                &active_tasks
                            ]() {

                                // 任务结束时自动减少计数
                                ActiveTaskGuard task_guard(active_tasks);

                                // 1. 处理 HTTP 请求
                                string response;

                                try {
                                    response = buildHttpResponse(
                                        request,
                                        redis_client,
                                        mysql_client
                                    );
                                }
                                catch (const std::exception& e) {

                                    cerr << "[HTTP] Task failed: "
                                         << e.what() << endl;

                                    const string body =
                                        "Internal Server Error";

                                    response =
                                        "HTTP/1.1 500 Internal Server Error\r\n"
                                        "Content-Length: " +
                                        to_string(body.size()) +
                                        "\r\nConnection: close\r\n\r\n" +
                                        body;
                                }
                                catch (...) {

                                    const string body =
                                        "Internal Server Error";

                                    response =
                                        "HTTP/1.1 500 Internal Server Error\r\n"
                                        "Content-Length: " +
                                        to_string(body.size()) +
                                        "\r\nConnection: close\r\n\r\n" +
                                        body;
                                }

                                // 2. 封装处理结果
                                HttpResult result{
                                    fd,
                                    connection_id,
                                    std::move(response)
                                };

                                // 3. 放入结果队列
                                {
                                    lock_guard<mutex> lock(result_mutex);
                                    result_queue.push(std::move(result));
                                }

                                // 4. 通知主线程
                                uint64_t one = 1;

                                while (true) {

                                    ssize_t n = write(
                                        result_event_fd,
                                        &one,
                                        sizeof(one)
                                    );

                                    if (n == sizeof(one)) {
                                        break;
                                    }

                                    if (n == -1 && errno == EINTR) {
                                        continue;
                                    }

                                    if (n == -1 &&
                                        (errno == EAGAIN ||
                                         errno == EWOULDBLOCK)) {
                                        break;
                                    }

                                    perror("write eventfd");
                                    break;
                                }
                            });

                            logInfo(
                                "Client ",
                                fd,
                                " submitted to thread pool!"
                            );

                            request_buffers.erase(fd);
                        }
                        catch (const std::exception& e) {

                            active_tasks.fetch_sub(
                                1,
                                std::memory_order_release
                            );

                            cerr << "ThreadPool enqueue failed: "
                                 << e.what() << endl;

                            processing_fds.erase(fd);
                            should_close = true;
                        }
                    }
                }
            }



            // 处理 Socket 可写事件
            if (!should_close &&
                (events[i].events & EPOLLOUT) &&
                response_buffers.count(fd)) {

                string &response = response_buffers[fd];
                size_t &offset = send_offsets[fd];

                while (offset < response.size()) {

                    ssize_t n = send(
                        fd,
                        response.data() + offset,
                        response.size() - offset,
                        MSG_NOSIGNAL
                    );

                    if (n > 0) {
                        offset += static_cast<size_t>(n);
                    }
                    else if (n == -1 && errno == EINTR) {
                        continue;
                    }
                    else if (n == -1 &&
                            (errno == EAGAIN ||
                            errno == EWOULDBLOCK)) {
                        cout << "Send buffer full! "
                             << "Already sent: " << offset
                             << " bytes" << endl;
                        break;
                    }
                    else {
                        should_close = true;
                        break;
                    }
                }

                // 全部发送完成
                if (offset == response.size()) {
                    logInfo("Response sent completely!");
                    should_close = true;
                }
            }
            // 3. 处理连接关闭或错误事件
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                should_close = true;
            }

            if ((events[i].events & EPOLLRDHUP) &&
                !response_buffers.count(fd) &&
                !processing_fds.count(fd)) {

                should_close = true;
            }

            // 4. 清理客户端资源
            if (should_close) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);

                // 删除客户端对应的缓冲区
                request_buffers.erase(fd);
                response_buffers.erase(fd);
                send_offsets.erase(fd);
                connection_ids.erase(fd);
                processing_fds.erase(fd);

                logInfo("Client disconnected: ", fd);
            }
        }
    }


    // 检查是否可以安全退出主循环
    if (shutting_down) {

        bool queue_empty = false;

        {
            lock_guard<mutex> lock(result_mutex);
            queue_empty = result_queue.empty();
        }

        // 所有工作任务和待发送响应均已处理完成
        if (
            active_tasks.load(std::memory_order_acquire) == 0 &&
            processing_fds.empty() &&
            response_buffers.empty() &&
            queue_empty
        ) {

            logInfo("All requests completed!");

            break;
        }
    }

}


    // 1. 停止线程池，等待所有工作线程退出
    pool.shutdown();

    logInfo("ThreadPool stopped!");

    // 2. 清理残留结果队列
    {
        lock_guard<mutex> lock(result_mutex);

        while (!result_queue.empty()) {
            result_queue.pop();
        }
    }

    // 3. 关闭所有剩余客户端 Socket
    for (const auto& entry : connection_ids) {

        int client_fd = entry.first;

        epoll_ctl(
            epoll_fd,
            EPOLL_CTL_DEL,
            client_fd,
            nullptr
        );

        close(client_fd);
    }

    // 4. 清理客户端容器
    connection_ids.clear();
    request_buffers.clear();
    response_buffers.clear();
    send_offsets.clear();
    processing_fds.clear();

    // 5. 关闭服务器监听 Socket
    close(server_fd);

    // 6. 关闭信号描述符
    close(signal_fd);

    // 7. 关闭 eventfd
    close(result_event_fd);

    // 8. 最后关闭 epoll
    close(epoll_fd);

    logInfo("WebServer shutdown complete!");

    return 0;
}
