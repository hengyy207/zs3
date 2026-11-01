#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
测试 MyPlacement Web 服务器
使用 thin1 数据集进行快速测试
"""

import requests
import os
import sys

# 服务器地址
SERVER_URL = 'http://localhost:5000'

def test_health():
    """测试健康检查接口"""
    print("测试健康检查接口...")
    try:
        response = requests.get(f'{SERVER_URL}/health')
        if response.status_code == 200:
            print("✓ 服务器运行正常")
            return True
        else:
            print(f"✗ 服务器返回错误: {response.status_code}")
            return False
    except Exception as e:
        print(f"✗ 无法连接到服务器: {e}")
        return False

def test_analyze():
    """测试分析接口（使用 thin1 数据）"""
    print("\n测试分析接口（使用 thin1 数据）...")

    # thin1 文件路径
    # 数据目录由脚本位置推导(<工程根>/web/ 的上一级 + data/),换机器无需修改
    data_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'data')
    files_to_upload = [
        'thin1.aux',
        'thin1.nodes',
        'thin1.nets',
        'thin1.pl',
        'thin1.scl',
        'thin1.wts'
    ]

    # 检查文件是否存在
    for filename in files_to_upload:
        filepath = os.path.join(data_dir, filename)
        if not os.path.exists(filepath):
            print(f"✗ 文件不存在: {filepath}")
            return False

    # 上传文件
    files = []
    for filename in files_to_upload:
        filepath = os.path.join(data_dir, filename)
        files.append(('files', (filename, open(filepath, 'rb'))))

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
                print(f"  - 初始 HPWL: {data.get('data', {}).get('hpwl_initial', '-')}")
                print(f"  - 最终 HPWL: {data.get('data', {}).get('hpwl_final', '-')}")
                return True
            else:
                print(f"✗ 分析失败: {data.get('error')}")
                return False
        else:
            print(f"✗ 请求失败: {response.status_code}")
            print(response.text)
            return False

    except Exception as e:
        print(f"✗ 测试失败: {e}")
        return False

def main():
    print("=" * 50)
    print("MyPlacement Web 服务器测试")
    print("=" * 50)

    # 测试健康检查
    if not test_health():
        print("\n请先启动服务器: python3 server.py")
        sys.exit(1)

    # 测试分析接口
    if test_analyze():
        print("\n" + "=" * 50)
        print("所有测试通过！")
        print("=" * 50)
        print("\n现在可以在浏览器中访问: http://localhost:5000")
    else:
        print("\n测试失败，请检查服务器日志")
        sys.exit(1)

if __name__ == '__main__':
    main()
