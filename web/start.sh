#!/bin/bash

# MyPlacement Web 服务器启动脚本

echo "=========================================="
echo "  MyPlacement 芯片布局分析工具"
echo "=========================================="

# 检查 Python 是否安装
if ! command -v python3 &> /dev/null; then
    echo "错误: 未找到 Python3"
    exit 1
fi

# 检查 Flask 是否安装
if ! python3 -c "import flask" &> /dev/null; then
    echo "正在安装 Flask..."
    pip3 install flask
fi

# 进入 web 目录
cd "$(dirname "$0")"

# 启动服务器
echo ""
echo "启动 Web 服务器..."
echo "请在浏览器中访问: http://localhost:5000"
echo "按 Ctrl+C 停止服务器"
echo ""

python3 server.py
