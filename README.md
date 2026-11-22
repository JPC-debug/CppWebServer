# C++ High-Performance WebServer

基于 C++17 和 Linux 的轻量级 HTTP WebServer，采用非阻塞 I/O、多路复用、线程池及 Redis/MySQL 实现并发请求处理与数据缓存。

## 项目介绍

本项目使用 Linux Socket API 构建 HTTP 服务器，采用 epoll 实现 I/O 多路复用，通过线程池处理客户端请求，并结合 MySQL 连接池和 Redis 缓存实现商品数据查询。

在基础 HTTP 服务之外，重点实现了数据库连接池的并发安全、超时控制、连接唤醒、断线重连及故障恢复机制。

## 技术栈

- **语言：** C++17
- **操作系统：** Linux (Ubuntu)
- **网络编程：** Socket、epoll、非阻塞 I/O
- **并发编程：** std::thread、std::mutex、std::condition_variable
- **线程通信：** eventfd
- **数据库：** MySQL、Redis
- **资源管理：** RAII、智能指针
- **构建工具：** GCC、pkg-config、Bash

## 核心功能

### 1. 网络 I/O 与并发处理

- 使用 epoll 实现 I/O 多路复用。
- 支持 ET/LT 两种触发模式。
- 采用非阻塞 Socket 处理客户端连接。
- 使用线程池分发 HTTP 请求，避免主线程执行耗时业务逻辑。
- 使用 eventfd 实现工作线程与主线程之间的事件通知。

### 2. MySQL 连接池

- 预先创建并管理数据库连接，减少频繁创建连接的开销。
- 使用互斥锁和条件变量保证多线程访问安全。
- 实现连接获取超时控制。
- 支持连接提前归还后的等待线程唤醒。
- 通过 mysql_ping 检查连接健康状态。
- 实现失效连接重建与断线恢复。
- 使用 RAII 管理数据库连接资源。

### 3. Redis 缓存

- 使用 Redis 缓存商品查询结果。
- 实现缓存命中与未命中处理。
- 设置缓存过期时间。
- 使用基于商品 Key 的互斥锁和双重检查机制，减少缓存击穿时的重复数据库查询。
- Redis 不可用时支持降级查询 MySQL。

### 4. 服务稳定性

- 实现日志记录。
- 支持 SIGINT 等信号触发的优雅关闭。
- 停止接收新请求后等待已提交任务完成。
- 完成线程池与数据库资源的有序释放。

## HTTP 接口

| 接口 | 功能 |
|---|---|
| GET / | 首页响应 |
| GET /hello | 基础 HTTP 响应 |
| GET /large | 大数据量响应测试 |
| GET /cache | Redis 缓存读取 |
| GET /product?id=1001 | 商品查询，结合 Redis 与 MySQL |

## 项目测试

项目包含以下测试程序：

- MySQL 连接池基础测试
- 多线程连接池测试
- 连接池获取超时测试
- 连接提前归还与等待线程唤醒测试
- MySQL 断线重连测试
- MySQL 连接失败与恢复测试
- HTTP 服务与缓存功能测试

已完成连接池超时、提前唤醒、断线重连和故障恢复等测试，相关测试均通过。

## 运行环境

需要安装：

- GCC / G++
- MySQL Client 开发库
- Redis++ 与 hiredis
- Redis 服务
- MySQL 服务
- pkg-config

## 配置与运行

项目通过环境变量读取 MySQL 密码。

在用户主目录创建配置文件：

```bash
nano ~/.cppwebserver.env
```

配置：

```bash
export MYSQL_PASSWORD="your_password"
```

请将 `your_password` 替换为自己的数据库密码。

启动服务：

```bash
./run.sh
```

服务器默认监听：

```text
127.0.0.1:8080
```

## 项目结构

```text
CppWebServer/
├── main.cpp
├── HttpHandler.h
├── ThreadPool.h
├── MySQLClient.h
├── MySQLConnectionPool.h
├── RedisClient.h
├── Logger.h
├── run.sh
├── tests/
│   ├── bench.sh
│   ├── mysql_pool_test.cpp
│   ├── mysql_pool_multithread_test.cpp
│   ├── test_mysql_pool_timeout.cpp
│   ├── test_mysql_pool_wakeup.cpp
│   ├── test_mysql_reconnect.cpp
│   └── test_mysql_reconnect_failure.cpp
└── .gitignore
```

## 后续优化方向

- 完善 HTTP 请求解析与协议处理。
- 增加连接管理与超时回收机制。
- 补充自动化压力测试及性能指标。
- 完善配置管理与异常处理。
