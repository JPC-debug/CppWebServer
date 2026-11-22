
#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <memory>
#include <chrono>
#include <cstdlib>
#include <stdexcept>

#include "MySQLConnectionPool.h"

using namespace std;

int main() {
    const char* password = getenv("MYSQL_PASSWORD");

    if (!password) {
        cerr << "MYSQL_PASSWORD 未设置" << endl;
        return 1;
    }

    // 创建包含4个连接的连接池
    MySQLConnectionPool pool(
        4, "127.0.0.1", "webserver_app",
        password, "webserver_db"
    );

    mutex cout_mutex;
    vector<thread> workers;

    // 创建6个线程，模拟6个并发请求
    for (int i = 0; i < 6; i++) {
        workers.emplace_back([&pool, &cout_mutex, i]() {

            // 初始化当前线程的MySQL环境
            if (mysql_thread_init() != 0) {
                lock_guard<mutex> lock(cout_mutex);
                cerr << "线程初始化失败" << endl;
                return;
            }

            {
                try {
                    auto release = [&pool](MYSQL* p) {
                        pool.releaseConnection(p);
                    };

                    // 自动借用、归还连接
                    unique_ptr<MYSQL, decltype(release)> conn(
                        pool.getConnection(), release
                    );

                    int id = 1001 + i % 3;

                    string sql =
                        "SELECT name, price FROM products WHERE id = "
                        + to_string(id);

                    if (mysql_query(conn.get(), sql.c_str()) != 0) {
                        throw runtime_error(mysql_error(conn.get()));
                    }

                    // 自动释放查询结果
                    unique_ptr<MYSQL_RES,
                               decltype(&mysql_free_result)> result(
                        mysql_store_result(conn.get()),
                        &mysql_free_result
                    );

                    if (!result) {
                        throw runtime_error(mysql_error(conn.get()));
                    }

                    MYSQL_ROW row = mysql_fetch_row(result.get());

                    if (row) {
                        lock_guard<mutex> lock(cout_mutex);
                        cout << "线程 " << i
                             << " 查询商品 " << id
                             << "：" << row[0]
                             << "，价格：" << row[1]
                             << endl;
                    }

                    // 模拟线程持有连接进行处理
                    this_thread::sleep_for(
                        chrono::milliseconds(300)
                    );

                } catch (const exception& e) {
                    lock_guard<mutex> lock(cout_mutex);
                    cerr << "线程 " << i
                         << " 出错：" << e.what() << endl;
                }
            }

            mysql_thread_end();
        });
    }

    // 等待全部线程结束
    for (auto& t : workers) {
        t.join();
    }

    cout << "所有线程执行完成！" << endl;

    return 0;
}
