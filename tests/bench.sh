
#!/usr/bin/env bash

set -euo pipefail

# 并发请求数：默认 100
N=${1:-100}

# 服务器地址
URL="http://127.0.0.1:8080"

# 创建独立测试目录
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

echo "================================="
echo "C++ WebServer Benchmark"
echo "Concurrent Requests: $N"
echo "================================="

START=$(date +%s%N)

# 同时发送 N 个请求
for ((i=1; i<=N; i++)); do
(
    # 交替查询两个商品
    if ((i % 2 == 0)); then
        id=1002
        expected="MacBook"
    else
        id=1001
        expected="iPhone"
    fi

    body="$DIR/body_$i"
    result_file="$DIR/result_$i"

    # 发送 HTTP 请求
    if result=$(curl -sS \
        --connect-timeout 3 \
        --max-time 10 \
        -o "$body" \
        -w "%{http_code} %{time_total}" \
        "$URL/product?id=$id" 2>/dev/null); then

        code=${result%% *}
        elapsed=${result#* }

    else
        code=000
        elapsed=0
    fi

    # 检查状态码和商品内容
    if [[ "$code" == "200" ]] &&
       grep -Fqx "Product Name: $expected" "$body"; then

        echo "PASS $elapsed" > "$result_file"

    else
        echo "FAIL $elapsed" > "$result_file"
        echo "FAIL request $i: HTTP $code"
    fi

) &
done

# 等待全部请求结束
wait

END=$(date +%s%N)

DURATION=$(awk "BEGIN {
    printf \"%.6f\", ($END - $START) / 1000000000
}")

# 使用 Python 统计测试结果
python3 - "$DIR" "$N" "$DURATION" <<'PY'
import sys
import math
from pathlib import Path

directory = Path(sys.argv[1])
total = int(sys.argv[2])
duration = float(sys.argv[3])

times = []
failed = 0

for i in range(1, total + 1):
    path = directory / f"result_{i}"

    if not path.exists():
        failed += 1
        continue

    parts = path.read_text().split()

    if len(parts) == 2 and parts[0] == "PASS":
        times.append(float(parts[1]))
    else:
        failed += 1

times.sort()
success = len(times)

print()
print("========== Benchmark Result ==========")
print(f"Total requests: {total}")
print(f"Success: {success}")
print(f"Failed: {failed}")
print(f"Success rate: {success / total * 100:.2f}%")
print(f"Total time: {duration:.3f} s")

if success:
    avg = sum(times) / success
    p95_index = math.ceil(success * 0.95) - 1

    print(f"Average latency: {avg * 1000:.2f} ms")
    print(f"P95 latency: {times[p95_index] * 1000:.2f} ms")
    print(f"Max latency: {times[-1] * 1000:.2f} ms")

if duration > 0:
    print(f"Throughput: {success / duration:.2f} requests/s")

print("======================================")
PY
