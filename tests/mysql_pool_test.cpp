#include <memory>
#include <iostream>
#include <cstdlib>
#include "MySQLConnectionPool.h"

using namespace std;

int main() {
    const char* password = getenv("MYSQL_PASSWORD");

    if (password == nullptr) {
        cerr << "请先设置 MYSQL_PASSWORD 环境变量" << endl;
        return 1;
    }

    try {
        // 1. 创建包含4个连接的连接池
        MySQLConnectionPool pool(
            4,
            "127.0.0.1",
            "webserver_app",
            password,
            "webserver_db"
        );

        cout << "MySQL连接池创建成功！" << endl;

        auto release = [&pool](MYSQL* p) {
            pool.releaseConnection(p);
        };

        std::unique_ptr<MYSQL, decltype(release)> conn(
            pool.getConnection(),
            release
        );

        cout << "成功获取MySQL连接！" << endl;

        // 3. 执行查询
        const char* sql =
            "SELECT id, name, price FROM products WHERE id = 1001";

        if (mysql_query(conn.get(), sql) != 0) {
            cerr << mysql_error(conn.get()) << endl;
            pool.releaseConnection(conn.get());
            return 1;
        }

        MYSQL_RES* result = mysql_store_result(conn.get());

        if (result == nullptr) {
            cerr << mysql_error(conn.get()) << endl;
            pool.releaseConnection(conn.get());
            return 1;
        }

        MYSQL_ROW row = mysql_fetch_row(result);

        if (row != nullptr) {
            cout << "商品ID：" << row[0] << endl;
            cout << "商品名称：" << row[1] << endl;
            cout << "商品价格：" << row[2] << endl;
        }

        // 4. 释放查询结果
        mysql_free_result(result);

        // 5. 归还连接池
        //pool.releaseConnection(conn.get());

        //cout << "连接已归还连接池！" << endl;

    } catch (const exception& e) {
        cerr << "发生错误：" << e.what() << endl;
        return 1;
    }

    return 0;
}
