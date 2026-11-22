
#pragma once

#include <string>
#include <sstream>
#include <charconv>
#include <chrono>
#include <exception>
#include <iostream>

#include "RedisClient.h"
#include "MySQLClient.h"

#include <mutex>
#include <memory>
#include <unordered_map>

#include "Logger.h"

// 管理不同商品对应的互斥锁
class ProductKeyLockManager {
public:

    // 根据商品 Key 获取对应的锁
    std::shared_ptr<std::mutex> getLock(
        const std::string& key
    ) {

        std::lock_guard<std::mutex> guard(map_mutex_);

        // 获取当前 Key 对应的弱引用
        auto& weak = locks_[key];

        // 尝试获取已有的互斥锁
        auto mutex_ptr = weak.lock();

        // 如果锁已经不存在，则重新创建
        if (!mutex_ptr) {
            mutex_ptr = std::make_shared<std::mutex>();
            weak = mutex_ptr;
        }

        // 定期清理已经失效的锁记录
        if (locks_.size() > 1024) {

            for (auto it = locks_.begin();
                 it != locks_.end();) {

                if (it->second.expired()) {
                    it = locks_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        return mutex_ptr;
    }

private:

    // 保护锁管理表
    std::mutex map_mutex_;

    // 商品 Key -> 互斥锁弱引用
    std::unordered_map<
        std::string,
        std::weak_ptr<std::mutex>
    > locks_;
};

// 获取全局商品锁管理器
inline ProductKeyLockManager& getProductLockManager() {

    static ProductKeyLockManager manager;

    return manager;
}


// 从 URL 中解析商品 ID
inline bool parseProductId(
    const std::string& target,
    long long& id
) {
    size_t query_pos = target.find('?');

    if (query_pos == std::string::npos) {
        return false;
    }

    std::string query = target.substr(query_pos + 1);

    size_t id_pos = query.find("id=");

    if (id_pos == std::string::npos ||
        (id_pos != 0 && query[id_pos - 1] != '&')) {
        return false;
    }

    size_t start = id_pos + 3;
    size_t end = query.find('&', start);

    if (end == std::string::npos) {
        end = query.size();
    }

    if (start == end) {
        return false;
    }

    auto result = std::from_chars(
        query.data() + start,
        query.data() + end,
        id
    );

    return result.ec == std::errc{} &&
           result.ptr == query.data() + end &&
           id > 0;
}

inline std::string buildHttpResponse(
    const std::string& request,
    RedisClient& redis,
    MySQLClient& mysql
) {

    std::istringstream iss(request);

    std::string method, target, version;
    std::string path;
    std::string status, body;

    if (!(iss >> method >> target >> version)) {
        status = "400 Bad Request";
        body = "Bad Request";
    }
    else if (method != "GET") {
        status = "405 Method Not Allowed";
        body = "Method Not Allowed";
    }
    else {

        // 分离 URL 路径和查询参数
        size_t query_pos = target.find('?');

        path = target.substr(0, query_pos);

        if (path == "/") {
            status = "200 OK";
            body = "Hello, C++ WebServer!";
        }
        else if (path == "/hello") {
            status = "200 OK";
            body = "Welcome to my WebServer!";
        }
        else if (path == "/large") {
            status = "200 OK";
            body = std::string(8 * 1024 * 1024, 'A');
        }
        else if (path == "/cache") {

            try {
                auto value = redis.get("webserver:message");

                status = "200 OK";

                if (value) {
                    body = *value;
                } else {
                    body = "Cache Miss: key does not exist";
                }
            }
            catch (const std::exception& e) {
                status = "503 Service Unavailable";
                body = "Redis unavailable";
            }
        }
        else if (path == "/product") {

            long long id = 0;

            if (!parseProductId(target, id)) {
                status = "400 Bad Request";
                body = "Invalid product ID";
            }
            else {

                // Redis 缓存 Key
                std::string cache_key =
                    "product:" + std::to_string(id);

                // 缓存未命中时，从 MySQL 查询
                auto loadFromMySQL = [&]() {

                    try {
                        logInfo("[MySQL] ORIGIN QUERY: ", cache_key);

                        auto product = mysql.getProductById(id);

                        if (!product) {
                            status = "404 Not Found";
                            body = "Product not found";
                            return;
                        }

                        status = "200 OK";

                        body =
                            "Product ID: " + product->id +
                            "\nProduct Name: " + product->name +
                            "\nProduct Price: " + product->price;

                        // 将数据库查询结果写入 Redis
                        try {
                            redis.set(
                                cache_key,
                                body,
                                std::chrono::seconds(60)
                            );
                        }
                        catch (const std::exception& e) {
                            std::cerr
                                << "[Redis] Cache write failed: "
                                << e.what() << std::endl;
                        }

                    }
                    catch (const std::exception& e) {
                        std::cerr
                            << "[MySQL] Query failed: "
                            << e.what() << std::endl;

                        status = "503 Service Unavailable";
                        body = "MySQL unavailable";
                    }
                };


                try {

                    // 1. 第一次查询 Redis
                    auto cached = redis.get(cache_key);

                    if (cached) {

                        // 2. 缓存命中，不需要加锁
                        status = "200 OK";
                        body = *cached;

                        logInfo("[Cache] HIT: ", cache_key);
                    }
                    else {
                        logInfo("[Cache] MISS: ", cache_key);

                        // 3. 获取当前商品对应的互斥锁
                        auto key_mutex =
                            getProductLockManager().getLock(cache_key);

                        // 4. 加锁，避免多个线程同时回源
                        std::unique_lock<std::mutex> key_lock(
                            *key_mutex
                        );

                        // 5. 第二次查询 Redis（双重检查）
                        auto cached_after_lock = redis.get(cache_key);

                        if (cached_after_lock) {

                            // 其他线程已经完成了 MySQL 查询和缓存写入
                            status = "200 OK";
                            body = *cached_after_lock;

                            logInfo("[Cache] HIT AFTER WAIT: ", cache_key);
                        }
                        else {

                            // 6. 仍然没有缓存，才查询 MySQL
                            loadFromMySQL();
                        }
                    }

                }
                catch (const std::exception& e) {

                    // Redis 不可用时，降级为直接查询 MySQL
                    std::cerr
                        << "[Redis] Read failed: "
                        << e.what() << std::endl;

                    loadFromMySQL();
                }

            }
        }
        else {
            status = "404 Not Found";
            body = "404 Not Found";
        }
    }

    std::string response =
        "HTTP/1.1 " + status + "\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body;

    return response;
}
