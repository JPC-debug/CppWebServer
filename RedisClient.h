
#pragma once

#include <string>
#include <chrono>
#include <sw/redis++/redis++.h>

class RedisClient {
public:
    // 构造函数
    RedisClient()
        : redis_(makeConnectionOptions(), makePoolOptions()) {
    }

    // 写入数据（不设置过期时间）
    void set(const std::string& key,
            const std::string& value) {
        redis_.set(key, value);
    }

    // 写入数据（设置过期时间）
    void set(const std::string& key,
            const std::string& value,
            std::chrono::seconds ttl) {

        redis_.set(
            key,
            value,
            std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
        );
    }

    // 读取数据
    sw::redis::OptionalString get(
        const std::string& key) {
        return redis_.get(key);
    }

    // 删除数据
    long long del(const std::string& key) {
        return redis_.del(key);
    }

    // 判断 Key 是否存在
    long long exists(const std::string& key) {
        return redis_.exists(key);
    }

private:
    // Redis 连接配置
    static sw::redis::ConnectionOptions makeConnectionOptions() {
        sw::redis::ConnectionOptions options;

        options.host = "127.0.0.1";
        options.port = 6379;

        options.connect_timeout = std::chrono::milliseconds(1000);
        options.socket_timeout = std::chrono::milliseconds(1000);

        return options;
    }

    // 连接池配置
    static sw::redis::ConnectionPoolOptions makePoolOptions() {
        sw::redis::ConnectionPoolOptions options;

        options.size = 4;
        options.wait_timeout = std::chrono::milliseconds(1000);

        return options;
    }

    // Redis 对象
    sw::redis::Redis redis_;
};
