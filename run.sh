
#!/usr/bin/env bash

# 遇到错误立即退出
set -e

# 进入项目根目录
cd "$(dirname "$0")"

echo "========== C++ WebServer =========="

# 1. 加载 MySQL 环境变量
ENV_FILE="$HOME/.cppwebserver.env"

if [ -f "$ENV_FILE" ]; then
    source "$ENV_FILE"
else
    echo "错误：找不到 MySQL 配置文件"
    exit 1
fi

# 2. 检查密码
if [ -z "${MYSQL_PASSWORD:-}" ]; then
    echo "错误：MYSQL_PASSWORD 未设置"
    exit 1
fi

echo "[1/2] MySQL 配置加载成功"

# 3. 创建编译目录
mkdir -p build

# 4. 编译项目
echo "[2/2] 正在编译..."

g++ -std=c++17 main.cpp \
    -o build/server \
    $(pkg-config --cflags --libs redis++ mysqlclient) \
    -pthread

echo "编译成功！"

# 5. 启动服务器
echo "正在启动 WebServer..."

exec ./build/server
