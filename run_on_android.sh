#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

# 小朋日记：在 Android 手机（Termux）上编译并启动后端。
# 运行成功后，手机浏览器打开 http://localhost:18080/ 即可，不依赖电脑/WSL。

prefix_dir="${PREFIX:-/data/data/com.termux/files/usr}"
cd "$(dirname "$0")"

if command -v clang++ >/dev/null 2>&1 && [ -x ./xiaopeng_server ]; then
  echo "[1/3] 已编译过，直接启动（改过代码请删除 xiaopeng_server 后重跑）。"
else
  echo "[1/3] 安装编译依赖（首次运行会较久）..."
  pkg update -y
  pkg install -y clang libasio libcurl libsqlite zlib

  echo "[2/3] 编译 xiaopeng_server..."
  "${CXX:-clang++}" -std=c++17 -O2 -I. -I"$prefix_dir/include" main.cpp \
    -o xiaopeng_server \
    -L"$prefix_dir/lib" -lsqlite3 -lcurl -lz -pthread
fi

echo "[3/3] 设置仅本机可访问（127.0.0.1）并启动..."
export XIAOPENG_BIND_ADDR="${XIAOPENG_BIND_ADDR:-127.0.0.1}"

if [ -z "${DEEPSEEK_API_KEY:-}" ]; then
  echo "提示：未设置 DEEPSEEK_API_KEY，AI 宠物分析暂不可用，写日记/保存不受影响。"
fi
echo "手机浏览器打开： http://localhost:18080/"
echo "按 Ctrl+C 停止。"
exec ./xiaopeng_server
