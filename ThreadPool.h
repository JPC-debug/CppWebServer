
#pragma once

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>
#include <stdexcept>
#include <utility>

class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex mtx;
    std::condition_variable cv;

    bool stop = false;

public:
    explicit ThreadPool(size_t num_threads) {
        if (num_threads == 0) {
            throw std::invalid_argument(
                "Thread count must be positive"
            );
        }

        for (size_t i = 0; i < num_threads; i++) {
            workers.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock(mtx);

                        cv.wait(lock, [this]() {
                            return stop || !tasks.empty();
                        });

                        if (stop && tasks.empty()) {
                            return;
                        }

                        task = std::move(tasks.front());
                        tasks.pop();
                    }

                    try {
                        task();
                    }
                    catch (const std::exception& e) {
                        std::cerr
                            << "[ThreadPool] Task exception: "
                            << e.what() << std::endl;
                    }
                    catch (...) {
                        std::cerr
                            << "[ThreadPool] Unknown task exception!"
                            << std::endl;
                    }
                }
            });
        }
    }

    // 提交任务
    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mtx);

            if (stop) {
                throw std::runtime_error("ThreadPool stopped");
            }

            tasks.push(std::move(task));
        }

        cv.notify_one();
    }

    // 主动停止线程池并等待所有工作线程退出
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop = true;
        }

        cv.notify_all();

        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    ~ThreadPool() {
        shutdown();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
};
