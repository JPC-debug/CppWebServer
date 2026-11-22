
#include <iostream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <exception>

#include "MySQLConnectionPool.h"

using namespace std;

int main() {

    const char* password = getenv("MYSQL_PASSWORD");

    if (!password) {
        cerr << "MYSQL_PASSWORD 未设置" << endl;
        return 1;
    }

    // 初始化 MySQL 客户端库
    if (mysql_library_init(0, nullptr, nullptr) != 0) {
        cerr << "MySQL library init failed" << endl;
        return 1;
    }

    bool timeout_occurred = false;

    try {
        {
            // 连接池只有1个连接
            MySQLConnectionPool pool(
                1,
                "127.0.0.1",
                "webserver_app",
                password,
                "webserver_db"
            );

            // 主线程借走唯一连接
            MYSQL* conn = pool.getConnection();

            cout << "主线程成功获取唯一连接" << endl;

            // 创建第二个线程
            thread worker([&]() {

                cout << "线程B开始申请连接..." << endl;

                auto start = chrono::steady_clock::now();

                try {
                    MYSQL* other = pool.getConnection();

                    auto end = chrono::steady_clock::now();

                    auto ms = chrono::duration_cast<
                        chrono::milliseconds
                    >(end - start).count();

                    cout << "线程B获取连接成功" << endl;
                    cout << "等待时间: " << ms << " ms" << endl;

                    pool.releaseConnection(other);

                } catch (const exception& e) {

                    timeout_occurred = true;

                    auto end = chrono::steady_clock::now();

                    auto ms = chrono::duration_cast<
                        chrono::milliseconds
                    >(end - start).count();

                    cout << "捕获异常: " << e.what() << endl;
                    cout << "等待时间: " << ms << " ms" << endl;
                }
            });

                // 主线程等待500ms
                this_thread::sleep_for(
                    chrono::milliseconds(500)
                );

                // 提前归还连接
                pool.releaseConnection(conn);

                cout << "主线程提前归还连接" << endl;

                // 最后等待线程B结束
                worker.join();
                        }

    } catch (const exception& e) {
        cerr << "测试失败: " << e.what() << endl;
        mysql_library_end();
        return 1;
    }

    mysql_library_end();

    if (timeout_occurred) {
        cerr << "测试失败：不应该发生超时" << endl;
        return 1;
    }

    cout << "连接池提前唤醒测试通过！" << endl;

    return 0;
}
