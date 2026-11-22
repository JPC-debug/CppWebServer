
#pragma once

#include "MySQLConnectionPool.h"

#include <string>
#include <optional>
#include <memory>
#include <stdexcept>

struct Product {
    std::string id;
    std::string name;
    std::string price;
};

// 管理当前线程的 MySQL 环境
class MySQLThreadContext {
public:
    MySQLThreadContext() {
        if (mysql_thread_init() != 0) {
            throw std::runtime_error(
                "MySQL thread initialization failed"
            );
        }
    }

    ~MySQLThreadContext() {
        mysql_thread_end();
    }

    MySQLThreadContext(const MySQLThreadContext&) = delete;
    MySQLThreadContext& operator=(const MySQLThreadContext&) = delete;
};

// 每个线程只初始化一次
inline void ensureMySQLThreadContext() {
    thread_local MySQLThreadContext context;
    (void)context;
}

class MySQLClient {
public:

    explicit MySQLClient(MySQLConnectionPool& pool)
        : pool_(pool) {
    }

    // 根据商品 ID 查询商品
    std::optional<Product> getProductById(long long id) {

        ensureMySQLThreadContext();

        if (id <= 0) {
            return std::nullopt;
        }

        // 1. 获取连接
        MYSQL* raw = pool_.getConnection();

        // 2. 使用 RAII 自动归还连接
        auto release = [this](MYSQL* conn) {
            pool_.releaseConnection(conn);
        };

        std::unique_ptr<MYSQL, decltype(release)> conn(
            raw, release
        );

        // 3. 构造 SQL
        std::string sql =
            "SELECT id, name, price FROM products WHERE id = "
            + std::to_string(id);

        // 4. 执行 SQL
        if (mysql_query(conn.get(), sql.c_str()) != 0) {
            throw std::runtime_error(mysql_error(conn.get()));
        }

        // 5. 获取结果
        std::unique_ptr<MYSQL_RES,
                        decltype(&mysql_free_result)> result(
            mysql_store_result(conn.get()),
            &mysql_free_result
        );

        if (!result) {
            throw std::runtime_error(mysql_error(conn.get()));
        }

        // 6. 读取第一行
        MYSQL_ROW row = mysql_fetch_row(result.get());

        if (!row) {
            return std::nullopt;
        }

        // 7. 封装商品对象
        Product product;

        product.id = row[0] ? row[0] : "";
        product.name = row[1] ? row[1] : "";
        product.price = row[2] ? row[2] : "";

        return product;
    }

private:
    MySQLConnectionPool& pool_;
};
