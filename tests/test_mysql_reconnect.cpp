
#include <iostream>
#include <memory>
#include <string>
#include <stdexcept>
#include <cstdlib>

#include "MySQLConnectionPool.h"

using namespace std;

// RAII 管理 MySQL 全局库
class MySQLLibraryGuard {
public:
    MySQLLibraryGuard() {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            throw runtime_error("MySQL library init failed");
        }
    }

    ~MySQLLibraryGuard() {
        mysql_library_end();
    }
};

int main() {
    const char* password = getenv("MYSQL_PASSWORD");

    if (!password) {
        cerr << "MYSQL_PASSWORD 未设置" << endl;
        return 1;
    }

    try {
        MySQLLibraryGuard library;

        // 只创建一个数据库连接
        MySQLConnectionPool pool(
            1, "127.0.0.1", "webserver_app",
            password, "webserver_db"
        );

        // 使用 RAII 自动归还借出的连接
        auto release = [&pool](MYSQL* p) {
            pool.releaseConnection(p);
        };

        unique_ptr<MYSQL, decltype(release)> conn(
            pool.getConnection(), release
        );

        unsigned long old_id = mysql_thread_id(conn.get());

        cout << "原连接 ID: " << old_id << endl;

        // 另建一个测试连接，用来断开原连接
        unique_ptr<MYSQL, decltype(&mysql_close)> killer(
            mysql_init(nullptr), &mysql_close
        );

        if (!killer) {
            throw runtime_error("mysql_init failed");
        }

        if (!mysql_real_connect(
                killer.get(),
                "127.0.0.1",
                "webserver_app",
                password,
                "webserver_db",
                3306,
                nullptr,
                0
            )) {
            throw runtime_error(mysql_error(killer.get()));
        }

        // 只终止本测试程序创建的原连接
        string sql = "KILL CONNECTION " + to_string(old_id);

        if (mysql_query(killer.get(), sql.c_str()) != 0) {
            throw runtime_error(mysql_error(killer.get()));
        }

        cout << "已主动断开原连接" << endl;

        // 将失效连接归还连接池
        conn.reset();

        cout << "重新申请连接..." << endl;

        // getConnection 内部应检测失效并重建
        unique_ptr<MYSQL, decltype(release)> recovered(
            pool.getConnection(), release
        );

        unsigned long new_id = mysql_thread_id(recovered.get());

        cout << "恢复后连接 ID: " << new_id << endl;

        // 验证重建后的连接能够执行 SQL
        if (mysql_query(recovered.get(), "SELECT 1") != 0) {
            throw runtime_error(mysql_error(recovered.get()));
        }

        unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(
            mysql_store_result(recovered.get()),
            &mysql_free_result
        );

        if (!result) {
            throw runtime_error(mysql_error(recovered.get()));
        }

        MYSQL_ROW row = mysql_fetch_row(result.get());

        if (!row || !row[0] || string(row[0]) != "1") {
            throw runtime_error("SELECT 1 result incorrect");
        }

        if (new_id == old_id) {
            throw runtime_error("Connection ID did not change");
        }

        cout << "SELECT 1 执行成功" << endl;
        cout << "MySQL 断线恢复测试通过！" << endl;

    } catch (const exception& e) {
        cerr << "测试失败: " << e.what() << endl;
        return 1;
    }

    return 0;
}
