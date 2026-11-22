
#include <iostream>
#include <string>
#include <stdexcept>
#include <cstdlib>
#include <memory>

#include "MySQLConnectionPool.h"

using namespace std;

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

        // 使用代理端口 33306，连接池只有一个连接
        MySQLConnectionPool pool(
            1,
            "127.0.0.1",
            "webserver_app",
            password,
            "webserver_db",
            33306
        );

        // 自动归还连接
        auto release = [&pool](MYSQL* conn) {
            if (conn) {
                pool.releaseConnection(conn);
            }
        };

        using PooledConnection =
            unique_ptr<MYSQL, decltype(release)>;

        // 1. 获取初始连接
        PooledConnection conn(
            pool.getConnection(), release
        );

        unsigned long old_id = mysql_thread_id(conn.get());

        cout << "初始连接成功，ID: " << old_id << endl;

        cout << "\n请切换到 socat 终端，按 Ctrl+C 停止代理。" << endl;
        cout << "停止后回到这里，按 Enter 继续..." << endl;

        cin.get();

        // 2. 归还已经失效的连接
        conn.reset();

        cout << "\n开始测试重连失败..." << endl;

        // 3. 代理停止后，预期重连失败
        try {
            PooledConnection failed(
                pool.getConnection(), release
            );

            cerr << "测试失败：代理停止后仍然获取到了连接" << endl;
            return 1;

        } catch (const exception& e) {
            cout << "预期捕获重连异常: "
                 << e.what() << endl;
        }

        cout << "\n现在请重新启动 socat 代理。" << endl;
        cout << "看到 listening 后，回到这里按 Enter..." << endl;

        cin.get();

        // 4. 再次申请连接，验证自动恢复
        cout << "\n开始测试恢复..." << endl;

        PooledConnection recovered(
            pool.getConnection(), release
        );

        unsigned long new_id =
            mysql_thread_id(recovered.get());

        cout << "恢复后连接 ID: " << new_id << endl;

        // 5. 验证新连接可以正常执行 SQL
        if (mysql_query(recovered.get(), "SELECT 1") != 0) {
            throw runtime_error(mysql_error(recovered.get()));
        }

        MYSQL_RES* raw = mysql_store_result(recovered.get());

        if (!raw) {
            throw runtime_error(mysql_error(recovered.get()));
        }

        unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>
            result(raw, &mysql_free_result);

        MYSQL_ROW row = mysql_fetch_row(result.get());

        if (!row || !row[0] || string(row[0]) != "1") {
            throw runtime_error("SELECT 1 verification failed");
        }

        cout << "SELECT 1 执行成功" << endl;
        cout << "MySQL 故障恢复测试全部通过！" << endl;

    } catch (const exception& e) {
        cerr << "测试失败: " << e.what() << endl;
        return 1;
    }

    return 0;
}
