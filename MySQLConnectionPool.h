
#pragma once

#include <mysql/mysql.h>

#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <stdexcept>
#include <chrono>
#include <unordered_set>
#include <memory>

class MySQLConnectionPool {
public:

    MySQLConnectionPool(
        size_t pool_size,
        const std::string& host,
        const std::string& user,
        const std::string& password,
        const std::string& database,
        unsigned int port = 3306
    )
        : host_(host),
          user_(user),
          password_(password),
          database_(database),
          port_(port)
    {
        if (pool_size == 0) {
            throw std::invalid_argument(
                "Pool size must be positive"
            );
        }

        try {
            for (size_t i = 0; i < pool_size; i++) {

                MYSQL* conn = createConnection(
                    host, user, password, database, port
                );

                try {
                    all_connections_.insert(conn);
                } catch (...) {
                    mysql_close(conn);
                    throw;
                }

                connections_.push(conn);
            }

        } catch (...) {
            closeAllConnections();
            throw;
        }
    }


    // 获取数据库连接
    MYSQL* getConnection() {

        MYSQL* conn = nullptr;

        {
            std::unique_lock<std::mutex> lock(mtx_);

            ++active_getters_;

            bool acquired = cv_.wait_for(
                lock,
                std::chrono::seconds(2),
                [this]() {
                    return shutting_down_ ||
                           !connections_.empty();
                }
            );

            if (!acquired) {
                --active_getters_;
                cv_.notify_all();

                throw std::runtime_error(
                    "MySQL connection pool timeout"
                );
            }

            if (shutting_down_) {
                --active_getters_;
                cv_.notify_all();

                throw std::runtime_error(
                    "MySQL connection pool is shutting down"
                );
            }

            conn = connections_.front();
            connections_.pop();

            bool inserted = false;

            try {
                inserted =
                    in_use_connections_.insert(conn).second;
            } catch (...) {
                connections_.push(conn);
                --active_getters_;
                cv_.notify_all();
                throw;
            }

            if (!inserted) {
                connections_.push(conn);
                --active_getters_;
                cv_.notify_all();

                throw std::logic_error(
                    "Connection ownership error"
                );
            }

            --active_getters_;
            cv_.notify_all();
        }

        // 检查连接健康状态
        if (mysql_ping(conn) == 0) {
            return conn;
        }

        // 连接失效，尝试重建
        try {

            std::unique_ptr<
                MYSQL,
                decltype(&mysql_close)
            > replacement(
                createConnection(
                    host_,
                    user_,
                    password_,
                    database_,
                    port_
                ),
                &mysql_close
            );

            {
                std::lock_guard<std::mutex> lock(mtx_);

                auto all_result =
                    all_connections_.insert(replacement.get());

                if (!all_result.second) {
                    throw std::logic_error(
                        "Duplicate connection pointer"
                    );
                }

                try {
                    auto use_result =
                        in_use_connections_.insert(
                            replacement.get()
                        );

                    if (!use_result.second) {
                        throw std::logic_error(
                            "Duplicate in-use connection"
                        );
                    }

                } catch (...) {
                    all_connections_.erase(
                        replacement.get()
                    );
                    throw;
                }

                // 原连接由新连接替换
                all_connections_.erase(conn);
                in_use_connections_.erase(conn);

                cv_.notify_all();
            }

            mysql_close(conn);

            return replacement.release();

        } catch (...) {

            std::lock_guard<std::mutex> lock(mtx_);

            in_use_connections_.erase(conn);

            if (shutting_down_) {

                all_connections_.erase(conn);
                mysql_close(conn);

            } else {

                // 保留连接名额，等待下次重试
                connections_.push(conn);
            }

            cv_.notify_all();

            throw;
        }
    }


    // 归还数据库连接
    void releaseConnection(MYSQL* conn) {

        if (conn == nullptr) {
            return;
        }

        std::lock_guard<std::mutex> lock(mtx_);

        // 只允许当前正在借出的连接归还
        auto it = in_use_connections_.find(conn);

        if (it == in_use_connections_.end()) {
            return;
        }

        in_use_connections_.erase(it);

        if (shutting_down_) {

            all_connections_.erase(conn);
            mysql_close(conn);

        } else {

            connections_.push(conn);
        }

        cv_.notify_all();
    }


    // 安全析构
    ~MySQLConnectionPool() {

        std::unique_lock<std::mutex> lock(mtx_);

        shutting_down_ = true;

        // 唤醒所有正在等待连接的线程
        cv_.notify_all();

        // 等待所有获取操作和借出的连接结束
        cv_.wait(lock, [this]() {
            return active_getters_ == 0 &&
                   in_use_connections_.empty();
        });

        // 所有连接均已归还，现在可以统一释放
        closeAllConnections();
    }


private:

    // 数据库配置
    std::string host_;
    std::string user_;
    std::string password_;
    std::string database_;
    unsigned int port_;


    // 空闲连接队列
    std::queue<MYSQL*> connections_;

    // 所有由连接池管理的连接
    std::unordered_set<MYSQL*> all_connections_;

    // 当前正在被借出的连接
    std::unordered_set<MYSQL*> in_use_connections_;


    // 线程同步
    std::mutex mtx_;
    std::condition_variable cv_;

    // 关闭状态
    bool shutting_down_ = false;

    // 正在等待或获取连接的线程数
    size_t active_getters_ = 0;


    // 创建数据库连接
    static MYSQL* createConnection(
        const std::string& host,
        const std::string& user,
        const std::string& password,
        const std::string& database,
        unsigned int port
    ) {

        MYSQL* conn = mysql_init(nullptr);

        if (conn == nullptr) {
            throw std::runtime_error("mysql_init failed");
        }

        unsigned int connect_timeout = 2;
        unsigned int io_timeout = 3;

        if (
            mysql_options(
                conn,
                MYSQL_OPT_CONNECT_TIMEOUT,
                &connect_timeout
            ) != 0 ||

            mysql_options(
                conn,
                MYSQL_OPT_READ_TIMEOUT,
                &io_timeout
            ) != 0 ||

            mysql_options(
                conn,
                MYSQL_OPT_WRITE_TIMEOUT,
                &io_timeout
            ) != 0
        ) {

            std::string error = mysql_error(conn);
            mysql_close(conn);

            throw std::runtime_error(
                "MySQL options failed: " + error
            );
        }

        if (
            mysql_real_connect(
                conn,
                host.c_str(),
                user.c_str(),
                password.c_str(),
                database.c_str(),
                port,
                nullptr,
                0
            ) == nullptr
        ) {

            std::string error = mysql_error(conn);
            mysql_close(conn);

            throw std::runtime_error(
                "MySQL connect failed: " + error
            );
        }

        return conn;
    }


    // 释放所有连接
    // 调用时必须保证没有其他线程访问连接池
    void closeAllConnections() {

        while (!connections_.empty()) {
            connections_.pop();
        }

        for (MYSQL* conn : all_connections_) {
            mysql_close(conn);
        }

        all_connections_.clear();
        in_use_connections_.clear();
    }

};
