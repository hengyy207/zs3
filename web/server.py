# -*- coding: utf-8 -*-
"""
MyPlacement Web 服务器（第一版）
功能：接收 BookShelf 文件上传，调用 C++ 程序分析，返回 BMP 图片
使用：python server.py
访问：http://localhost:5000
"""

from flask import Flask, request, jsonify, send_from_directory
import re
import subprocess
import os
import tempfile
import base64
import shutil

app = Flask(__name__)


def locate_project_root():
    """自动定位工程根目录。

    本文件位于 <工程根>/web/server.py,上一级目录就是工程根。
    这样工程整体拷贝到任意电脑的任意路径都能直接运行,
    不再依赖写死的绝对路径。也可用环境变量 MYPLACE_ROOT 强制指定。
    """
    env = os.environ.get('MYPLACE_ROOT')
    if env:
        return env
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def locate_myplace(root):
    """在工程内自动查找已编译的 myplace 可执行文件(ELF)。

    优先顺序:环境变量 MYPLACE_BIN > 根目录 build/myplace >
    out/build/<配置名>/myplace(Windows + WSL 交叉编译的产物目录)。
    返回 None 表示还没编译出可执行文件。
    """
    env = os.environ.get('MYPLACE_BIN')
    if env and os.path.isfile(env):
        return env

    candidates = [os.path.join(root, 'build', 'myplace')]
    out_build = os.path.join(root, 'out', 'build')
    if os.path.isdir(out_build):
        for cfg in sorted(os.listdir(out_build)):
            p = os.path.join(out_build, cfg, 'myplace')
            if os.path.isfile(p):
                candidates.append(p)
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


# 工程根目录(自动推导,工程可放到任意位置)
PROJECT_ROOT = locate_project_root()
# C++ 可执行文件(自动探测编译产物)
MYPLACE_BIN = locate_myplace(PROJECT_ROOT)
# 兜底输出目录:仅在可执行文件不支持 --out 时才会命中(旧版本程序写死 output/)
OUTPUT_DIR = os.path.join(PROJECT_ROOT, 'output')


def first_existing(paths):
    """按顺序返回第一个存在的文件路径,全部不存在时返回 None"""
    for p in paths:
        if p and os.path.exists(p):
            return p
    return None

# 分模式超时(秒)。
# 两种模式的耗时差一个数量级,统一 300 秒会造成"完整模式必然踩线、机器稍慢就误杀":
#   fast: 跳过三种初始布局对比与算法自检,adaptec1 在 Release 下约 1 分钟;
#   full: 额外跑聚类驱动(约 9 秒)与二次方程解析(21 万单元约 2~3 分钟),
#         总耗时 5 分钟以上属于正常范围。
TIMEOUT_FAST = 240
TIMEOUT_FULL = 600


@app.route('/')
def index():
    """提供前端页面"""
    return send_from_directory(os.path.dirname(os.path.abspath(__file__)), 'index.html')


@app.route('/analyze', methods=['POST'])
def analyze():
    """接收文件上传，运行分析，返回结果"""
    # 模式与超时在 try 外解析:超时分支还要用到这两个变量,
    # 若只在 try 内赋值,一旦更早处抛异常,except 里就会 UnboundLocalError。
    mode = request.form.get('mode', 'fast')
    timeout_s = TIMEOUT_FULL if mode == 'full' else TIMEOUT_FAST
    try:
        # 1. 检查是否有文件上传
        if 'files' not in request.files:
            return jsonify({'error': '没有上传文件'}), 400

        files = request.files.getlist('files')
        if len(files) == 0:
            return jsonify({'error': '没有选择文件'}), 400

        # 2. 创建临时目录保存上传的文件
        with tempfile.TemporaryDirectory() as tmpdir:
            # 保存所有上传的文件
            uploaded_files = []
            for f in files:
                if f.filename:
                    filepath = os.path.join(tmpdir, f.filename)
                    f.save(filepath)
                    uploaded_files.append(f.filename)

            # 每个请求一个独立输出目录:并发分析时不会互相覆盖输出文件。
            # 目录位于本次请求的临时目录内,请求结束随 tmpdir 一起自动清理。
            out_dir = os.path.join(tmpdir, 'out')
            os.makedirs(out_dir, exist_ok=True)

            # 3. 找到 .aux 文件
            aux_files = [f for f in uploaded_files if f.endswith('.aux')]
            if len(aux_files) == 0:
                return jsonify({'error': '未找到 .aux 文件，请上传完整的 BookShelf 文件集'}), 400

            aux_file = aux_files[0]
            aux_path = os.path.join(tmpdir, aux_file)

            # 4. 检查是否上传了所有必需的文件
            required_exts = ['.aux', '.nodes', '.nets', '.pl', '.scl']
            missing = []
            for ext in required_exts:
                if not any(f.endswith(ext) for f in uploaded_files):
                    missing.append(ext)

            if missing:
                return jsonify({
                    'error': f'缺少必需文件: {", ".join(missing)}',
                    'uploaded': uploaded_files
                }), 400

            # 5. 检查 C++ 可执行文件是否已编译(迁移到新电脑常忘了这步)
            if not MYPLACE_BIN or not os.path.exists(MYPLACE_BIN):
                return jsonify({
                    'error': '未找到 myplace 可执行文件。请先在 WSL 里编译项目,'
                             '或用环境变量 MYPLACE_BIN 指定可执行文件路径。',
                    'project_root': PROJECT_ROOT
                }), 500

            # 6. 分析模式的参数映射（mode / timeout_s 已在函数入口解析）
            if mode == 'full':
                mode_label = '完整分析'
                mode_args = []          # 完整模式 = 不传 flag（C++ 默认全流程）
            else:
                mode = 'fast'
                mode_label = '快速分析(--fast)'
                mode_args = ['--fast']

            # 7. 调用 C++ 程序
            #    --out 指定本请求专属的输出目录(程序按此写 *.bmp / *.gds)
            cmd = [MYPLACE_BIN] + mode_args + ['--out', out_dir, aux_path]
            print(f"[{mode_label}] 运行(超时 {timeout_s}s): {' '.join(cmd)}")
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                cwd=PROJECT_ROOT,
                timeout=timeout_s
            )

            # 6. 检查程序是否成功运行
            if result.returncode != 0:
                return jsonify({
                    'error': '程序运行失败',
                    'stderr': result.stderr,
                    'stdout': result.stdout
                }), 500

            # 7. 读取生成的 BMP 图片（优先返回合法化后的图片）
            #    先查本请求的独立目录；后两个是旧二进制的兜底路径
            #    （不支持 --out 的程序仍写工程根 output/，此时退化为全局共享）。
            bmp_path = first_existing([
                os.path.join(out_dir, 'legalization_result.bmp'),
                os.path.join(out_dir, 'global_placement_result.bmp'),
                os.path.join(OUTPUT_DIR, 'legalization_result.bmp'),
                os.path.join(OUTPUT_DIR, 'global_placement_result.bmp'),
            ])

            if not bmp_path:
                return jsonify({
                    'error': '未生成结果图片。请确认 build/myplace 已用最新源码重新编译'
                             '（新版本通过 --out 写入独立输出目录）。',
                    'stdout': result.stdout
                }), 500

            with open(bmp_path, 'rb') as f:
                img_base64 = base64.b64encode(f.read()).decode('utf-8')

            # 8. 读取 GDS 文件（可选，优先返回合法化版本）
            gds_path = first_existing([
                os.path.join(out_dir, 'legalization_result.gds'),
                os.path.join(out_dir, 'global_placement_result.gds'),
                os.path.join(OUTPUT_DIR, 'legalization_result.gds'),
                os.path.join(OUTPUT_DIR, 'global_placement_result.gds'),
            ])
            gds_base64 = None
            if gds_path:
                with open(gds_path, 'rb') as f:
                    gds_base64 = base64.b64encode(f.read()).decode('utf-8')

            # 9. 解析日志，提取关键数据
            log_data = parse_log(result.stdout)

            # 10. 返回结果
            return jsonify({
                'success': True,
                'mode': mode_label,
                'image': img_base64,
                'gds': gds_base64,
                'log': result.stdout,
                'data': log_data,
                'files': uploaded_files
            })

    except subprocess.TimeoutExpired:
        # 给出按当前模式区分的提示，方便用户下一步操作
        hint = ''
        if mode == 'full':
            hint = '。请改用「快速分析」重试，或换用更小的数据集（如 data/thin1）'
        else:
            hint = '。当前已是快速分析仍超时，请换用更小的数据集（如 data/thin1），' \
                   '并确认 build/myplace 是 Release 构建'
        return jsonify({
            'error': f'分析超时（超过 {timeout_s} 秒）' + hint,
            'mode': mode,
            'timeout': timeout_s
        }), 500
    except Exception as e:
        return jsonify({'error': str(e)}), 500


def parse_log(log):
    """解析运行日志，提取关键数据"""
    data = {
        'cell_count': 0,
        'net_count': 0,
        'pin_count': 0,
        'row_count': 0,
        'core_area': 0,
        'hpwl_initial': 0,
        'hpwl_final': 0,
        'improvement': 0,
        'methods': []
    }

    lines = log.split('\n')
    for line in lines:
        # 解析单元数量
        if 'Cell #' in line:
            try:
                data['cell_count'] = int(line.split(':')[1].strip().split()[0])
            except:
                pass

        # 解析网络数量
        if 'Net #' in line:
            try:
                data['net_count'] = int(line.split(':')[1].strip().split()[0])
            except:
                pass

        # 解析引脚数量
        if 'Pin #' in line:
            try:
                data['pin_count'] = int(line.split(':')[1].strip())
            except:
                pass

        # 解析行数
        if 'Row Height/Num' in line:
            # 实际格式：Row Height/Num: 12 / 890  (site step 1.000000)
            # 行数在 "/" 之后；原先写死取 split(':')[2]，但该行只有两段，越界后
            # 异常被吞掉，导致界面上的行数一直是 0。
            try:
                seg = line.split(':', 1)[1]
                data['row_count'] = int(seg.split('/')[1].strip().split()[0])
            except (IndexError, ValueError):
                pass

        # 解析核心区域面积
        if 'Core Area' in line:
            try:
                data['core_area'] = float(line.split(':')[1].strip())
            except:
                pass

        # 解析初始 HPWL
        if '初始 HPWL' in line and '=' in line:
            try:
                data['hpwl_initial'] = float(line.split('=')[1].strip())
            except:
                pass

        # 解析最终 HPWL
        if '最终 HPWL' in line and '=' in line:
            try:
                data['hpwl_final'] = float(line.split('=')[1].strip().split()[0])
            except:
                pass

        # 解析改善百分比：兼容完整模式 "[任务5] 最终 HPWL = a   改善 = b%"，
        # 也兼容快速模式 "[fast] ...   改善 = b%   耗时 = x"，统一用正则定位。
        if '改善' in line and '%' in line:
            try:
                m = re.search(r'改善\s*=\s*([\d.]+)\s*%', line)
                if m:
                    data['improvement'] = float(m.group(1))
            except:
                pass

        # 解析表格格式的方法数据（格式：║ ①随机布局 │ 1.87e+04 │ 1.2% │ 0.000 s │ 对照组║）
        if '│' in line and ('①' in line or '②' in line or '③' in line or '④' in line):
            try:
                # 按 │ 分割列
                cols = [c.strip() for c in line.split('│') if c.strip()]
                if len(cols) >= 5:
                    # 第一列：方法名称（去掉 ║ 和序号）
                    method_name_raw = cols[0].replace('║', '').strip()
                    # 第二列：HPWL
                    hpwl_str = cols[1].strip()
                    # 第三列：峰值密度
                    density_str = cols[2].strip().replace('%', '')
                    # 第四列：耗时
                    time_str = cols[3].strip().replace('s', '').strip()
                    # 第五列：特点
                    feature_str = cols[4].replace('║', '').strip()

                    # 根据序号确定方法名称
                    if '①' in method_name_raw:
                        method_name = '随机布局'
                    elif '②' in method_name_raw:
                        method_name = '聚类驱动'
                    elif '③' in method_name_raw:
                        method_name = '二次方程'
                    elif '④' in method_name_raw:
                        method_name = 'Nesterov全局布局'
                    else:
                        continue

                    data['methods'].append({
                        'name': method_name,
                        'hpwl': float(hpwl_str),
                        'density': float(density_str),
                        'time': float(time_str),
                        'feature': feature_str
                    })
            except:
                pass

    return data


@app.route('/health')
def health():
    """健康检查接口"""
    return jsonify({'status': 'ok', 'message': 'MyPlacement Web 服务器运行中'})


if __name__ == '__main__':
    print("=" * 50)
    print("MyPlacement Web 服务器")
    print("=" * 50)
    print(f"项目目录: {PROJECT_ROOT}")
    print(f"可执行文件: {MYPLACE_BIN or '(未找到!)'}")
    print(f"输出目录: {OUTPUT_DIR}")
    if not MYPLACE_BIN:
        print("")
        print("提示: 还没找到编译好的 myplace,页面可打开,但点『开始分析』会报错。")
        print("      请先在 WSL 中编译,或用环境变量 MYPLACE_BIN 指定可执行文件。")
    # 端口可用环境变量覆盖:MYPLACE_PORT=5001 python3 server.py
    port = int(os.environ.get('MYPLACE_PORT', '5000'))
    # 监听地址默认 0.0.0.0:Windows 侧浏览器访问 WSL 内的服务需要它
    host = os.environ.get('MYPLACE_HOST', '0.0.0.0')
    # 调试模式默认关闭:Werkzeug 调试器会暴露交互式控制台,不适合分发给别人使用;
    # 本地开发需要热重载时设 MYPLACE_DEBUG=1 开启。
    debug = os.environ.get('MYPLACE_DEBUG', '0') == '1'

    print("=" * 50)
    print(f"启动服务器: http://localhost:{port}")
    print("=" * 50)

    app.run(host=host, port=port, debug=debug)
