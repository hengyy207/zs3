# web —— 本地可视化前端

Flask 后端 + 单文件前端的本地 Web 应用，把 `myplace` 的分析过程与结果做成三页界面。总体的安装、编译与运行说明见**上级目录的 [`README.md`](../README.md)**，本文只讲这个模块自身。

## 启动

```bash
cd web
./start.sh              # 缺 flask 会自动 pip 安装
# 或
python3 server.py
```

浏览器访问 <http://localhost:5000>。

## 依赖与前置

- 已编译好的 `myplace`（工程 `build/myplace`，或 `out/build/<配置>/myplace`）。找不到时页面能打开，但点「开始分析」会报错。
- Python 3 与 Flask。

## 接口

| 方法   | 路径         | 说明                                                                 |
| ---- | ---------- | ------------------------------------------------------------------ |
| GET  | `/`        | 返回 `index.html`                                                    |
| GET  | `/health`  | 健康检查，返回 `{"status": "ok"}`                                         |
| POST | `/analyze` | 接收 multipart 文件（字段名 `files`）+ 表单字段 `mode`（`fast` / `full`），返回 JSON |

`/analyze` 返回结构：

```json
{
  "success": true,
  "mode": "快速分析(--fast)",
  "image": "<base64 BMP>",
  "gds": "<base64 GDSII>",
  "log": "<程序 stdout>",
  "data": { "cell_count": 0, "net_count": 0, "pin_count": 0, "row_count": 0,
            "hpwl_initial": 0, "hpwl_final": 0, "improvement": 0, "methods": [] }
}
```

## 路径与配置

所有路径都由脚本位置推导，工程整体拷到任意机器、任意目录都能直接用，没有写死的绝对路径。

| 项     | 默认                                         | 覆盖方式                 |
| ----- | ------------------------------------------ | -------------------- |
| 工程根   | `server.py` 的上一级目录                         | 环境变量 `MYPLACE_ROOT`  |
| 可执行文件 | `build/myplace` → `out/build/<配置>/myplace` | 环境变量 `MYPLACE_BIN`   |
| 端口    | 5000                                       | 环境变量 `MYPLACE_PORT`  |
| 监听地址  | `0.0.0.0`（Windows 浏览器访问 WSL 内的服务需要）        | 环境变量 `MYPLACE_HOST`  |
| 调试器   | 关闭                                         | `MYPLACE_DEBUG=1` 开启 |

## 输出目录隔离

每个请求在临时目录下建 `out/` 子目录，用 `--out` 传给 C++ 程序，请求结束随临时目录一并删除。因此多个标签页同时分析不会互相覆盖 `*.bmp` / `*.gds`。该能力依赖含 `--out` 参数的新版 `myplace`；旧版程序不认这个参数，会回落到工程根 `output/`（仍能工作，但没有隔离）。

## 超时

按模式区分（`server.py` 顶部的 `TIMEOUT_FAST` / `TIMEOUT_FULL`）：

| 模式           | 阈值    | adaptec1 实测 |
| ------------ | ----- | ----------- |
| 快速（`--fast`） | 240 秒 | 约 36 秒      |
| 完整           | 600 秒 | 约 56 秒      |

若快速分析仍超时，通常是 `build/myplace` 为 Debug 构建，用 `cmake --fresh` 重新配置成 Release。

## 自测

服务器运行后另开一个终端：

```bash
python3 test.py         # 完整自测：健康检查 + thin1 上传分析
python3 quick_test.py   # 轻量连通性自测
```

两个脚本的数据目录都按脚本位置推导（`../data`），换机器不用改。需要 `pip3 install requests`。

## 文件结构

```
web/
├── index.html      # 单文件前端：三页 + 样式 + 脚本，零外部依赖
├── server.py       # Flask 后端：上传、调用 myplace、解析日志、回传图片
├── start.sh        # 启动脚本（自动安装 flask）
├── test.py         # 接口自测
├── quick_test.py   # 快速连通性自测
└── README.md       # 本文件
```

前端不引用任何 CDN 或在线字体，离线可用；页面切换用 hash 路由（`#/data`、`#/run`、`#/result`）。
