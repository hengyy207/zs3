#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
快速测试 MyPlacement Web 服务器
在服务器运行的情况下执行此脚本
"""

import requests
import os
import sys

SERVER_URL = 'http://localhost:5000'

def main():
    print("测试 MyPlacement Web 服务器...")
    print(f"服务器地址: {SERVER_URL}")
    print()

    # 测试健康检查
    try:
        response = requests.get(f'{SERVER_URL}/health')
        if response.status_code == 200:
            print("✓ 服务器运行正常")
        else:
            print(f"✗ 服务器返回错误: {response.status_code}")
            return
    except Exception as e:
        print(f"✗ 无法连接到服务器: {e}")
        print()
        print("请先启动服务器:")
        print("  cd web && python3 server.py")
        return

    # 测试 thin1 数据
    print("\n测试 thin1 数据集...")
    # 数据目录由脚本位置推导(<工程根>/web/ 的上一级 + data/),换机器无需修改
    data_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'data')

    files = []
    for filename in ['thin1.aux', 'thin1.nodes', 'thin1.nets', 'thin1.pl', 'thin1.scl', 'thin1.wts']:
        filepath = os.path.join(data_dir, filename)
        if os.path.exists(filepath):
            files.append(('files', (filename, open(filepath, 'rb'))))
        else:
            print(f"✗ 文件不存在: {filepath}")
            return

    try:
        print("正在上传文件并运行分析...")
        response = requests.post(f'{SERVER_URL}/analyze', files=files)

        if response.status_code == 200:
            data = response.json()
            if data.get('success'):
                print("✓ 分析成功！")
                print(f"  - 图片大小: {len(data.get('image', '')) // 1024} KB")
                print(f"  - 单元数量: {data.get('data', {}).get('cell_count', '-')}")
                print(f"  - 网络数量: {data.get('data', {}).get('net_count', '-')}")
                print()
                print("✓ 所有测试通过！")
                print()
                print("现在可以在浏览器中访问: http://localhost:5000")
            else:
                print(f"✗ 分析失败: {data.get('error')}")
        else:
            print(f"✗ 请求失败: {response.status_code}")
    except Exception as e:
        print(f"✗ 测试失败: {e}")

if __name__ == '__main__':
    main()
