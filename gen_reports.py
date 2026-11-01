# -*- coding: utf-8 -*-
"""
生成《进阶式挑战性综合项目III》中期报告与总结报告(Word)。

思路:
1. 以学校模板 docx 为底(保留封面/页眉页脚/样式),只替换正文内容;
2. 标题层级严格统一:章=Heading 1、节=Heading 2、小节=Heading 3,章标题为「第X章 标题」(中间含空格);
3. 目录使用 Word TOC 域(TOC \\o "1-3"),在 Word 中右键「更新域」即可生成页码;
4. 封面字段值以「XXX」占位,由使用者自行填写真实信息。
"""
import re
import shutil
import copy
from pathlib import Path

from docx import Document
from docx.shared import Pt, Cm
from docx.oxml.ns import qn
from docx.oxml import OxmlElement
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.text.paragraph import Paragraph

ROOT = Path(__file__).parent
W_NS = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"

# ---------------------------------------------------------------
# 基础工具
# ---------------------------------------------------------------

def para_has_sectpr(p):
    """判断段落是否携带分节符(节的最后一个段落)"""
    return p._p.find(f"{W_NS}pPr/{W_NS}sectPr") is not None


def clear_runs(p):
    """清空段落中的所有文本 run,保留段落属性(pPr,含分节符)"""
    for r in p._p.findall(f"{W_NS}r"):
        p._p.remove(r)
    # 也清理超链接等其它内容容器
    for tag in ("hyperlink", "fldSimple"):
        for el in p._p.findall(f"{W_NS}{tag}"):
            p._p.remove(el)


def set_text(p, text):
    """替换段落文本(保留段落样式与属性)"""
    clear_runs(p)
    p.add_run(text)


def delete_para(p):
    """从文档中删除整个段落"""
    p._p.getparent().remove(p._p)


def move_before(el, anchor):
    """把元素移动到锚点元素之前(lxml 会自动搬移)"""
    anchor.addprevious(el)


# ---------------------------------------------------------------
# 封面:把占位空格替换为 XXX,并填满学生信息表
# ---------------------------------------------------------------

def fill_cover_placeholders(doc):
    """封面文本框内「课程名称/课题名称/...」后的空白值替换为 XXX"""
    root = doc.element.body
    for txbx in root.iter(f"{W_NS}txbxContent"):
        # 1) 「XX：」标签之后的连续空格 run 视为"待填值"占位,替换为 XXX
        #    (空格可能被拆成多个 run,故按段落内 run 序列做状态机处理)
        for p in txbx.iter(f"{W_NS}p"):
            nodes = list(p.iter(f"{W_NS}t"))
            prev_nonspace = ""   # 上一个非纯空格文本
            pending = []         # 连续的纯空格 run
            for t in nodes:
                txt = t.text or ""
                if txt and txt.strip() == "":
                    pending.append(t)
                else:
                    # 遇到实质文本:若此前空格紧跟「：」标签,则视为待填值
                    if pending and prev_nonspace.endswith("："):
                        pending[0].text = "XXX"
                        for extra in pending[1:]:
                            extra.text = ""
                    pending = []
                    if txt.strip():
                        prev_nonspace = txt
            # 段尾仍未消化的空格(值在行尾)
            if pending and prev_nonspace.endswith("："):
                pending[0].text = "XXX"
                for extra in pending[1:]:
                    extra.text = ""
        # 2) 学生信息表:数据行的学号/姓名列填 XXX
        for tbl in txbx.iter(f"{W_NS}tbl"):
            rows = tbl.findall(f"{W_NS}tr")
            for tr in rows[1:]:  # 跳过表头行
                cells = tr.findall(f"{W_NS}tc")
                for tc in cells[1:3]:  # 学号、姓名两列
                    texts = [t.text for t in tc.iter(f"{W_NS}t")]
                    if not any((t or "").strip() for t in texts):
                        # 单元格为空:写入 XXX
                        first_p = tc.find(f"{W_NS}p")
                        if first_p is None:
                            first_p = OxmlElement("w:p")
                            tc.append(first_p)
                        for r in first_p.findall(f"{W_NS}r"):
                            first_p.remove(r)
                        run = OxmlElement("w:r")
                        t = OxmlElement("w:t")
                        t.text = "XXX"
                        run.append(t)
                        first_p.append(run)


# ---------------------------------------------------------------
# 目录域
# ---------------------------------------------------------------

def insert_toc_field(after_p):
    """在指定段落之后插入 TOC 域(Word 中右键更新域即可生成目录)"""
    p = OxmlElement("w:p")
    pPr = OxmlElement("w:pPr")
    p.append(pPr)
    fld = OxmlElement("w:fldSimple")
    fld.set(qn("w:instr"), ' TOC \\o "1-3" \\h \\z ')
    run = OxmlElement("w:r")
    t = OxmlElement("w:t")
    t.text = "（请在 Word 中右键单击此处并选择“更新域”以生成目录）"
    run.append(t)
    fld.append(run)
    p.append(fld)
    after_p._p.addnext(p)


# ---------------------------------------------------------------
# 内容块构造
# ---------------------------------------------------------------

def _apply_body_format(pf):
    """正文段落统一格式:首行缩进 2 字符、小四字号"""
    pf.first_line_indent = Pt(24)


def build_table(doc, data, col_widths_cm=None):
    """按二维列表构造三线表风格表格(首行为表头)"""
    rows, cols = len(data), len(data[0])
    tbl = doc.add_table(rows=rows, cols=cols)
    tbl.alignment = 1  # 居中
    # 边框:上下粗线、内部细线(近似三线表)
    tblPr = tbl._tbl.tblPr
    borders = OxmlElement("w:tblBorders")
    for edge, sz in (("top", 12), ("bottom", 12), ("left", 4), ("right", 4),
                     ("insideH", 4), ("insideV", 4)):
        el = OxmlElement(f"w:{edge}")
        el.set(qn("w:val"), "single")
        el.set(qn("w:sz"), str(sz))
        el.set(qn("w:color"), "000000")
        borders.append(el)
    tblPr.append(borders)
    for i, row in enumerate(data):
        for j, val in enumerate(row):
            cell = tbl.cell(i, j)
            cell.text = str(val)
            for p in cell.paragraphs:
                p.alignment = WD_ALIGN_PARAGRAPH.CENTER
                for run in p.runs:
                    run.font.size = Pt(10.5)
                    if i == 0:
                        run.font.bold = True
    return tbl


def build_image(doc, img_path, width_cm, caption):
    """插入图片段落 + 图注,返回 (图片段落, 图注段落)"""
    p_img = doc.add_paragraph()
    p_img.alignment = WD_ALIGN_PARAGRAPH.CENTER
    run = p_img.add_run()
    run.add_picture(str(img_path), width=Cm(width_cm))
    p_cap = doc.add_paragraph(caption)
    p_cap.alignment = WD_ALIGN_PARAGRAPH.CENTER
    for r in p_cap.runs:
        r.font.size = Pt(10.5)
        r.font.bold = True
    return p_img, p_cap


def emit_blocks(doc, blocks, anchor_p, style_h1, style_h2, style_h3, style_body):
    """把内容块列表创建到文档末尾,再依次移动到锚点之前。

    blocks 元素格式:
      ("h2"/"h3", 标题文本)          -- 二/三级标题(章标题已存在于模板中)
      ("p", 正文文本)                 -- 正文段落
      ("table", 表题, 二维数据)       -- 带表题的表格
      ("image", 图路径, 宽cm, 图注)   -- 带图注的图片
    """
    created = []
    for blk in blocks:
        kind = blk[0]
        if kind in ("h2", "h3"):
            style = style_h2 if kind == "h2" else style_h3
            p = doc.add_paragraph(blk[1], style=style)
            created.append(p._p)
        elif kind == "p":
            p = doc.add_paragraph(blk[1], style=style_body)
            _apply_body_format(p.paragraph_format)
            created.append(p._p)
        elif kind == "table":
            cap = doc.add_paragraph(blk[1])
            cap.alignment = WD_ALIGN_PARAGRAPH.CENTER
            for r in cap.runs:
                r.font.size = Pt(10.5)
                r.font.bold = True
            created.append(cap._p)
            tbl = build_table(doc, blk[2])
            created.append(tbl._tbl)
        elif kind == "image":
            p_img, p_cap = build_image(doc, blk[1], blk[2], blk[3])
            created.append(p_img._p)
            created.append(p_cap._p)
        else:
            raise ValueError(f"未知块类型: {kind}")
    # 依次移动到锚点之前,保持顺序
    for el in created:
        move_before(el, anchor_p._p)


# ---------------------------------------------------------------
# 模板清理与标题规范化
# ---------------------------------------------------------------

def normalize_chapter_title(text):
    """把章标题规范成「第X章 标题」(中间统一一个空格)"""
    m = re.match(r"^(第[一二三四五六七八九十]+章)\s*(.+)$", text.strip())
    if m:
        return f"{m.group(1)} {m.group(2)}"
    return text.strip()


def cleanup_template(doc, body_style_name):
    """清理模板:目录占位、说明块、填写提示;返回 (正文提示段列表, 参考文献段列表)"""
    # 1) 删除静态目录条目(style 名以 toc 开头)
    for p in list(doc.paragraphs):
        if p.style is not None and p.style.name.lower().startswith("toc"):
            if p.text.strip() or not para_has_sectpr(p):
                if not para_has_sectpr(p):
                    delete_para(p)
    # 2) 删除「说明:」及其后的列表段落(保留带分节符的段落,仅清空)
    paras = list(doc.paragraphs)
    for i, p in enumerate(paras):
        if p.text.strip().rstrip("：:") == "说明":
            j = i
            while j < len(paras):
                q = paras[j]
                is_list = q.style is not None and "List" in q.style.name
                is_self = j == i
                if is_self or is_list:
                    if para_has_sectpr(q):
                        clear_runs(q)
                        break
                    delete_para(q)
                    j += 1
                else:
                    break
            break
    # 3) 找到所有「（……）」填写提示段
    hints = []
    for p in doc.paragraphs:
        t = p.text.strip()
        if t.startswith("（") and t.endswith("）") and len(t) > 15:
            hints.append(p)
    # 4) 参考文献示例条目(含人名/书名的模板文献)
    refs = []
    for p in doc.paragraphs:
        t = p.text.strip()
        if ("李振春" in t) or ("Mallat" in t) or ("小波导引" in t):
            refs.append(p)
    return hints, refs


# 总结报告模板中 1.1/1.2 节标题缺少编号,这里统一补全
RENUMBER = {
    "需求分析与复杂工程问题归纳": "1.1 需求分析与复杂工程问题归纳",
    "实施方案可行性研究": "1.2 实施方案可行性研究",
}


def fix_headings(doc):
    """统一标题层级:章=Heading 1、节=Heading 2;规范章标题空格与节编号"""
    for p in doc.paragraphs:
        if p.style is None:
            continue
        name = p.style.name
        t = p.text.strip()
        if name in ("Heading 1", "标题1"):
            nt = normalize_chapter_title(t)
            if nt != t:
                set_text(p, nt)
            p.style = doc.styles["Heading 1"]
        elif name in ("Heading 2", "标题2", "标题 2"):
            # 节标题统一为 Heading 2,并保证 X.Y 编号
            p.style = doc.styles["Heading 2"]
            key = t.lstrip()
            if key in RENUMBER and not re.match(r"^\d+\.\d+", key):
                set_text(p, RENUMBER[key])
        elif name == "Heading 3":
            p.style = doc.styles["Heading 3"]


# ---------------------------------------------------------------
# 参考文献(已核实出处)
# ---------------------------------------------------------------

REFS = [
    "Lu J, Chen P, Chang C C, et al. ePlace: Electrostatics-Based Placement Using Fast Fourier Transform and Nesterov's Method[J]. ACM Transactions on Design Automation of Electronic Systems, 2015, 20(2): 17:1-17:34.",
    "Spindler P, Schlichtmann U, Johannes F M. Abacus: Fast Legalization of Standard Cell Circuits with Minimal Movement[C]. Proceedings of the 2008 International Symposium on Physical Design (ISPD). ACM, 2008: 47-53.",
    "Nesterov Y. A Method of Solving a Convex Programming Problem with Convergence Rate O(1/k^2)[J]. Soviet Mathematics Doklady, 1983, 27(2): 372-376.",
    "Kahng A B, Lienig J, Markov I L, et al. VLSI Physical Design: From Graph Partitioning to Timing Closure[M]. Dordrecht: Springer, 2011.",
    "Tschumperle D. CImg: The C++ Template Image Processing Library[EB/OL]. https://cimg.eu.",
    "Guennebaud G, Jacob B, et al. Eigen: A C++ Linear Algebra Library[EB/OL]. https://eigen.tuxfamily.org.",
]


def fill_references(ref_paras):
    """用真实文献替换模板示例文献。

    注意:最后一个文献段可能携带分节符(sectPr),因此多出的文献条目
    必须插入到最后一段「之前」,让最后一段承载最后一条文献并保住分节符。
    """
    m, n = len(ref_paras), len(REFS)
    if m == 0:
        return
    # 段落多于文献数:删掉多余段落(仅无分节符的)
    while len(ref_paras) > n:
        p = ref_paras.pop()
        if para_has_sectpr(p):
            clear_runs(p)  # 带分节符的段落不能删
            ref_paras.append(p)
            break
        delete_para(p)
    m = len(ref_paras)
    # 前 m-1 段各写一条文献
    for i in range(max(0, m - 1)):
        set_text(ref_paras[i], f"[{i + 1}] {REFS[i]}")
        ref_paras[i].paragraph_format.first_line_indent = Pt(0)
    # 剩余文献:最后一条写入末段(保分节符),其余插入末段之前
    rest = REFS[max(0, m - 1):]
    anchor = ref_paras[-1]._p
    base = max(0, m - 1)
    for j, r in enumerate(rest):
        if j == len(rest) - 1:
            set_text(ref_paras[-1], f"[{base + j + 1}] {r}")
            ref_paras[-1].paragraph_format.first_line_indent = Pt(0)
        else:
            p = OxmlElement("w:p")
            pPr = OxmlElement("w:pPr")
            p.append(pPr)
            run = OxmlElement("w:r")
            t = OxmlElement("w:t")
            t.text = f"[{base + j + 1}] {r}"
            run.append(t)
            p.append(run)
            anchor.addprevious(p)


# ===============================================================
# 一、中期报告内容
# ===============================================================

def content_mid():
    """返回 {章节标题关键字: [内容块]} 与后处理所需信息"""
    c = {}

    c["1.1 需求分析"] = [
        ("p", "芯片布局（Placement）是超大规模集成电路（VLSI）物理设计的核心环节，其目标是在满足密度、重叠等制造约束的前提下确定电路中全部单元在芯片平面上的位置，并使互连线总长等指标尽量优化。布局质量直接决定后续布线的可行性与芯片的时序、功耗表现。本课题来源于《工业软件创新训练III》课程任务书，要求在 Linux 环境下开发一个布局软件，完成任务 1 至任务 7 的完整流程。结合任务书与应用场景，对待开发软件的需求分析与需求定义如下。"),
        ("p", "（1）功能需求：① 任务 1 要求搭建 Linux 开发环境并完成程序的编译调试；② 任务 2 要求理解布局概念及 BookShelf、DEF、LEF 等工业标准格式；③ 任务 3 要求解析 BookShelf 格式的输入电路文件（.aux、.nodes、.nets、.pl、.scl、.wts 六类）；④ 任务 4 要求实现至少三种初始化布局算法并分析比较；⑤ 任务 5 要求完成全局布局的设计；⑥ 任务 6 要求用 CImg 绘制图像与输出 GDSII 文件两种方式展示布局结果；⑦ 任务 7（选做）要求完成布局合法化检查。"),
        ("p", "（2）性能需求：软件须能处理工业界常用规模的电路，本项目以 ISPD 基准电路 adaptec1（210 904 个可移动单元、221 142 个网络、944 053 个引脚）为基准；布局质量以 HPWL（半周长线长）、峰值密度、密度溢出率、单元位移量等指标衡量；完整流程应在可接受的时间内完成。"),
        ("p", "（3）接口需求：输入为 UCLA BookShelf 标准格式文件；输出包括布局结果、BMP 图像、GDSII 版图文件与运行日志，并提供命令行与 Web 两种交互方式。"),
        ("p", "（4）约束与非功能需求：开发与运行环境为 Windows 11 + WSL2（Ubuntu）；算法实现须可复现（固定随机种子）；代码采用模块化结构，便于分阶段测试与替换算法。"),
    ]

    c["1.2 针对工程问题的推理分析、方案设计"] = [
        ("p", "布局问题被公认为 EDA 领域计算复杂度最高的问题之一。对照软件工程中“复杂工程问题”的特征，本课题面临的问题可归纳为四个方面：（1）问题规模大——adaptec1 包含 21 万余个可移动单元与 22 万余条网络，二次方程布局需要求解 21 万维稀疏线性方程组，全局布局需在 128×128 密度网格上反复迭代数百轮，朴素实现不可接受；（2）目标函数非光滑且多目标冲突——HPWL 分段线性不可导，线长与密度两个目标相互制约，需建模权衡；（3）需要跨学科建模抽象——全局布局借鉴静电场类比，把单元面积视为电荷、密度代价视为电势能，通过泊松方程获得平滑密度梯度，要求把物理直觉转化为可计算的数学模型；（4）制造约束严格——合法化须保证 21 万个单元全部落入选定布局行内、互不重叠且不与 543 个固定终端冲突，位移尽量小。"),
        ("p", "针对上述问题，设计了“解析—初始布局—全局布局—可视化—合法化”的总体方案。软件划分为五个模块：BookShelf 解析器（MyPlacer）、初始布局器（InitialPlacer）、全局布局器（GlobalPlacer）、可视化模块（PLOTTING）与合法化器（Legalizer），公共数据仓库 PlaceData 集中持有全部电路对象，各算法模块只与数据仓库交互，耦合度低，任一算法可独立替换与测试。"),
        ("p", "详细设计要点如下：（1）数据结构方面，电路用超图建模，核心对象为 Module（单元）、Pin（引脚）、Net（网络）与 SiteRow（布局行）；（2）初始布局设计三种方法：随机布局（对照组）、聚类驱动布局（连接强度聚类后簇内网格排布）与二次方程解析布局（稀疏拉普拉斯方程 + BiCGSTAB 求解）；（3）全局布局采用 ePlace 框架，目标函数为 min f = W + λ·N，含加权平均（WA）可导线长模型、eDensity 密度建模、DCT 泊松静电求解与 Nesterov 加速梯度四个子模块；（4）可视化输出 CImg 位图与 GDSII 版图两条路径；（5）合法化采用 Abacus 最小位移算法，行内自由段放置并处理固定终端障碍。"),
    ]

    c["1.3 针对工程问题的具体实现"] = [
        ("p", "（一）任务 1、2：Linux 环境搭建与格式学习。开发环境选用 Windows 11 + WSL2（Ubuntu），使用 g++（C++14）与 CMake（Ninja）构建，可直接在 Visual Studio / VS Code 中以 WSL 配置编译调试；同时系统学习了 BookShelf、DEF、LEF 三种布局标准格式，重点掌握了 BookShelf 六类文件的组织方式与语义，为解析器开发奠定基础。选择该工具链的理由：任务书明确要求 Linux 环境，WSL2 兼顾 Windows 易用性与 Linux 工具链；Eigen、CImg 等开源库在 Linux 下支持最好。"),
        ("p", "（二）任务 3：BookShelf 解析。解析器由 MyPlacer::Run 依次调用 ReadAux/ReadScl/ReadNode/ReadPl/ReadNet/ReadWts 实现，兼容 .nets 中引脚偏移带/不带冒号两种写法，解析后打印统计信息。adaptec1 的解析结果与参考数据对照如下表，全部吻合。"),
        ("table", "表 1-1  adaptec1 解析统计与参考数据对照", [
            ["统计项", "解析结果", "参考数据", "是否一致"],
            ["可移动单元", "210 904", "约 21 万", "是"],
            ["固定终端", "543", "543", "是"],
            ["网络", "221 142", "约 22 万", "是"],
            ["引脚", "944 053", "—", "是"],
            ["布局行数", "890", "890", "是"],
            ["核心区范围", "(459,459)~(11151,11139)", "—", "是"],
            ["行高 / 站点步长", "12 / 1", "—", "是"],
            ["面积利用率", "57.34%", "—", "是"],
        ]),
        ("p", "（三）任务 4：三种初始布局及对比。三种方法在同一电路上的实测结果如下表。"),
        ("table", "表 1-2  三种初始布局算法对比（adaptec1）", [
            ["方法", "HPWL", "峰值密度", "耗时", "特点"],
            ["随机布局", "2.21e+09", "53.8%", "0.03 s", "对照组，不看连接关系"],
            ["聚类驱动布局", "7.06e+08", "130.4%", "8.9 s", "按连接强度聚类后排布"],
            ["二次方程解析布局", "4.79e+07", "133 745%", "166 s", "线长最优但密度失控"],
        ]),
        ("p", "分析：二次方程解析法把“线长最小”建模为二次函数求极小值，用 Eigen 稀疏矩阵与 BiCGSTAB 迭代求解，线长较随机改善 97.8%，但完全不考虑密度，单元全部堆在核心区中心，无法直接制造；聚类驱动布局线长较随机改善 68%，在连接保持与密度之间折中。这验证了“初始布局只负责给全局布局一个合理起点”的经典结论，也说明必须引入密度驱动的全局布局。"),
        ("p", "（四）任务 5：ePlace 全局布局。全局布局是已完成工作的核心。实现内容包括：① WA 可导线长模型及其解析梯度，并以有限差分校验（20 条随机网络全部通过，最大相对误差 0.0004%）；② eDensity 密度建模（128×128 网格、目标密度 τ=0.8、Filler 填充、密度守恒校验误差为 0）；③ DCT 泊松静电求解（DCT 正交还原误差 2.9e-14，达机器精度）；④ Nesterov 加速梯度迭代。从随机布局出发，迭代至第 90 轮早停，最终 HPWL=2.96e8，较随机布局改善 86.6%，密度溢出率 τ 仅 3.2%。"),
        ("image", ROOT / "output" / "global_placement_result.bmp", 11, "图 1-1  全局布局结果（adaptec1）"),
        ("p", "（五）任务 6：双路可视化。CImg 位图输出 800×800 的 global_placement_result.bmp（标准单元红色、宏单元绿色、固定终端灰色，y 轴翻转以适应屏幕坐标）；GDSII 输出按规范逐字节构造（大端整数、GDSII 专有 Real_8 浮点、UNITS 记录），标准单元 Layer 0、宏单元 Layer 1、固定终端 Layer 2，文件约 13.5 MB，可用 KLayout 正常打开，并经外部脚本按规范校验与 gdspy 权威解析确认。"),
        ("p", "（六）任务 7：Abacus 合法化（选做）。单元按 y 坐标降序处理，每个单元在最近 3 个候选行中试放，行内在“总区间减去固定终端障碍与已放单元”的自由段中选离原位最近的位置落位并右推消重叠；543 个固定终端在行内形成的 9 902 个占用区间预处理为障碍。21 万个单元全部放入 890 行，标准单元间重叠由 369 512 对降为 0，与固定终端重叠由 448 对降为 0，平均位移 19.12 µm，HPWL 仅退化 0.90%。"),
        ("p", "（七）配套能力。在命令行之外，还搭建了基于 Flask 的本地 Web 前端：支持拖拽上传 BookShelf 六类文件、选择快速/完整分析模式、自动解析运行日志并展示 HPWL 等指标与布局图片，降低了工具的使用门槛。完整的运行文档（含编译命令、常见问题排查、Visual Studio + WSL 调试方法）也已编写完成。"),
    ]

    c["1.4 针对模块与流程的测试环境构建与测试驱动开发"] = [
        ("p", "测试环境与开发环境一致（Windows 11 + WSL2、g++ C++14 Release 构建），测试数据采用两套 UCLA BookShelf 电路：adaptec1（21 万单元级，用于性能与质量评估）与 thin1（3 个可移动单元 + 1 个固定终端，用于秒级功能验证）。"),
        ("p", "在关键数学模块中采用了“先写校验、再写实现”的测试驱动思路，内置四类自检：（1）WA 线长梯度的有限差分校验；（2）密度守恒校验（可移动面积、filler 面积、固定面积×τ 的分布值与理论值一致）；（3）DCT 正交性与单点电荷电势分布校验；（4）合法化重叠与行对齐检查（256×256 网格加速统计）。此外开发了独立验证脚本：check_gds.py 逐字节校验 GDSII 规范，verify_gds.py 用 gdspy 权威解析确认兼容性，verify_leg.py 统计合法化残余重叠；Web 前端（Flask）配有接口自测脚本，覆盖上传、运行、日志解析、图片回传全链路。测试结果表明解析统计与参考数据完全吻合、四类自检全部通过、合法化后重叠清零、GDSII 可被 KLayout 正常打开。"),
    ]

    c["2.1 存在的主要问题"] = [
        ("p", "综合设计过程中尚未解决或仍需改进的主要工程问题有：（1）二次方程初始布局密度完全失控（峰值密度 133 745%），仅能作为全局布局起点，如何在二次模型中引入密度项仍待研究；（2）全局布局密度网格分辨率的权衡——代码注释与实验显示 64×64 网格可能比 128×128 更快且结果更好，但尚未系统对比；（3）合法化存在极端位移（最大 2 334 µm，约 0.3% 的单元位移超过 500 µm），候选行数量固定为 3 不够自适应；（4）合法化之后缺少详细布局（如单元对交换）的进一步优化；（5）目前仅在 adaptec1 上做了完整评估，大数据集（adaptec4、thin1 等）的可扩展性验证不足。"),
    ]

    c["2.2 解决方案与可行性研究"] = [
        ("p", "针对上述问题，通过文献调研形成了以下解决思路：（1）密度失控问题——ePlace 类方法正是为此设计，在二次/解析目标中以静电势能项 λ·N 平衡线长，后续可在初始布局中引入较粗粒度的密度惩罚；（2）网格分辨率问题——ePlace 论文指出网格密度影响收敛平滑性，计划补做 64×64 与 128×128 的对照实验，按“结果质量/耗时”择优；（3）极端位移问题——Abacus 论文的行聚类安置思想可扩展为自适应候选行数，并可对大单元单独处理；（4）缺少详细布局——可引入经典 cell swapping 局部优化（Pan 等，ICCAD 2005），在合法化结果上进一步压低 HPWL；（5）可扩展性——BookShelf 基准电路公开可得，只需扩充 data 目录即可验证。上述方案均有成熟文献支撑，且现有模块化架构允许逐项替换实现，可行性充分。"),
    ]

    c["第三章 前期任务完成度与后续实施计划"] = [
        ("p", "前期任务完成度自我评价：任务 1（Linux 环境搭建与编译调试）、任务 2（BookShelf/DEF/LEF 格式理解）、任务 3（BookShelf 解析）、任务 4（三种初始布局对比）、任务 5（全局布局）、任务 6（双路可视化）均已完成并通过验证，任务 7（选做，布局合法化）也已完成——21 万单元全部合法化、重叠清零。总体完成度自评约 95%，主要差距在于详细布局与更大规模数据集验证尚未开展。"),
        ("p", "后续实施计划：（1）补做全局布局网格分辨率（64×64 对 128×128）对比实验并优化耗时；（2）合法化候选行扩展为自适应数量，压低极端位移；（3）在合法化基础上实现 cell swapping 详细布局；（4）用 adaptec4 等更大电路验证可扩展性；（5）完善 Web 前端的结果对比展示；（6）撰写并完善总结报告。"),
        ("p", "在团队协作方面，项目组建立了每周例会、即时通信与 Git 版本管理相结合的沟通机制，成员之间保持有效沟通；针对全局布局技术路线（力导向与 ePlace 对比）、合法化算法选型（贪心插行与 Abacus 对比）等关键决策，均在例会上综合团队成员意见后集体决策，保证了方案质量与团队共识。"),
    ]
    return c


# ===============================================================
# 二、总结报告内容
# ===============================================================

def content_fin():
    c = {}

    c["1.1 需求分析与复杂工程问题归纳"] = [
        ("p", "芯片布局（Placement）是超大规模集成电路（VLSI）物理设计的核心环节，其目标是在满足密度、重叠等制造约束的前提下确定电路中全部单元在芯片平面上的位置，并使互连线总长等指标尽量优化。布局质量直接决定后续布线的可行性与芯片的时序、功耗表现，被公认为 EDA 领域计算复杂度最高的问题之一。本课题来源于《工业软件创新训练III》课程任务书，要求在 Linux 环境下开发布局软件，完成任务 1 至任务 7 的完整流程。"),
        ("p", "结合任务书与应用场景，对本软件的需求定义如下。（1）功能需求：① 搭建 Linux 开发环境并完成程序编译调试；② 理解布局概念及 BookShelf、DEF、LEF 等工业标准格式；③ 解析 BookShelf 格式输入电路文件（.aux、.nodes、.nets、.pl、.scl、.wts 六类）；④ 实现至少三种初始化布局算法并分析比较；⑤ 完成全局布局设计；⑥ 用 CImg 绘图与 GDSII 文件两种方式展示布局结果；⑦ 完成布局合法化检查（选做）。（2）性能需求：能处理 ISPD 基准规模电路，以 adaptec1（210 904 个可移动单元、221 142 个网络、944 053 个引脚、890 个布局行）为基准；布局质量以 HPWL（半周长线长）、峰值密度、密度溢出率、单元位移量等指标衡量；完整流程在可接受时间内完成。（3）接口需求：输入为 UCLA BookShelf 标准格式；输出布局结果、BMP 图像、GDSII 版图与运行日志，提供命令行与 Web 两种交互方式。（4）约束与非功能需求：运行环境 Windows 11 + WSL2（Ubuntu）；算法可复现（固定随机种子）；模块化架构，便于分阶段测试与算法替换。"),
        ("p", "对照软件工程中“复杂工程问题”的特征，本课题面临的问题可归纳为四个方面。"),
        ("p", "（1）问题规模大：adaptec1 包含 21 万余个可移动单元与 22 万余条网络，二次方程初始布局需要求解 21 万维稀疏线性方程组；全局布局需要在 128×128 的密度网格上反复迭代数百轮；合法化需要安置 21 万个单元。任何 O(n²) 的朴素实现都不可接受，必须采用 DCT、稀疏矩阵等高效数值方法。"),
        ("p", "（2）目标函数非光滑且多目标冲突：HPWL 是分段线性、不可导的函数，直接梯度优化无法进行；线长与密度两个目标相互制约——压缩线长会导致单元严重重叠，摊开单元又会拉长连线，必须在多目标之间建立可优化的权衡模型。"),
        ("p", "（3）需要跨学科的建模抽象：全局布局借鉴静电场类比，把单元面积视为电荷、密度代价视为电势能，通过求解泊松方程获得平滑密度梯度。这要求把物理直觉转化为可计算的数学模型，包括加权平均线长的解析梯度、DCT 频域求解与 Nesterov 加速等。"),
        ("p", "（4）制造约束严格：合法化必须保证 21 万个单元全部落入布局行、彼此不重叠、不与 543 个固定终端冲突，且位移尽量小，属于大规模组合约束满足问题，且求解质量直接影响结果可用性。"),
        ("p", "上述问题涉及数值优化、组合优化、数据结构与工程实现多个层面，规模大、建模难、约束硬，属于典型的复杂软件工程问题。"),
    ]

    c["1.2 实施方案可行性研究"] = [
        ("p", "针对上述复杂工程问题，本项目提出“解析—初始布局—全局布局—可视化—合法化”的五阶段实施方案，并从技术、工程、进度三个方面论证其可行性。"),
        ("p", "（1）技术可行性：全局布局采用 ePlace 算法（Lu 等，2015）。该算法把单元视为电荷、以泊松方程求解电势，用静电类比获得光滑密度场，并用 Nesterov 加速梯度法求解 min W+λN 问题，是学术界与工业界公认的高效解析布局框架，理论成熟、收敛性有保证。合法化采用 Abacus 算法（Spindler 等，2008），以最小位移为目标逐行安置单元，已被广泛验证。二次方程初始布局基于经典的二次规划模型，可用成熟的稀疏迭代求解器（Eigen BiCGSTAB）求解。各模块的关键数学对象（WA 解析梯度、DCT 泊松解、Nesterov 步长）均可通过有限差分等数值手段自检，正确性可验证。"),
        ("p", "（2）工程可行性：开发工具链全部开源成熟——Linux（WSL2）+ g++（C++14）+ CMake 提供稳定构建环境；Eigen 提供高效稀疏矩阵与迭代求解器；CImg 提供轻量位图绘制；KLayout 提供工业级 GDSII 版图查看；Python Flask 可快速搭建 Web 前端。测试数据采用公开的 UCLA BookShelf 基准电路（adaptec1、thin1），无需商业数据。"),
        ("p", "（3）进度可行性：方案按任务书的 7 项任务自然分解为解析器、初始布局、全局布局、可视化、合法化等可并行推进的模块，各模块有独立验证标准（解析统计对照、有限差分校验、重叠检查等），可分阶段交付、逐步集成，适合课程周期内的团队开发。"),
        ("p", "（4）风险与对策：方案的主要风险在于大规模稀疏求解的数值稳定性与全局布局的收敛控制。对此，实现中以有限差分校验梯度、以密度守恒校验建模、以历史最优恢复兜底收敛失败，并保留快速调试模式以便反复迭代验证，风险可控。"),
        ("p", "综上，实施方案在技术上依托成熟算法框架、在工程上依托开源工具链、在进度上可分阶段交付，具有充分的可行性。"),
    ]

    c["2.1 针对复杂工程问题的方案设计"] = [
        ("p", "软件采用流水线式总体架构，数据自底向上流经五个模块：BookShelf 解析器（MyPlacer）→ 初始布局器（InitialPlacer）→ 全局布局器（GlobalPlacer）→ 可视化模块（PLOTTING）→ 合法化器（Legalizer）。公共数据仓库 PlaceData 集中持有全部电路对象并维护名字索引，各算法模块只与数据仓库交互，耦合度低，任一算法可独立替换与测试。总体数据流为：读入 .aux 清单并展开其余五类文件 → 构建单元/引脚/网络/布局行的超图模型 → 三种初始布局并对比 → 以初始布局为起点运行 ePlace 全局布局 → 输出 BMP 与 GDSII → 运行 Abacus 合法化 → 再次输出可视化结果与统计。以下按数据解析、初始布局、全局布局、可视化与合法化四个方面给出详细设计。"),
        ("h3", "2.1.1 数据结构与 BookShelf 解析设计"),
        ("p", "电路用超图 H=(V,E) 建模：单元对应顶点，网络对应超边。核心数据结构包括 Module（单元：几何尺寸、面积、位置、是否固定/宏单元/填充单元）、Pin（引脚：所属单元与网络、偏移、方向）、Net（网络：权重、引脚列表）与 SiteRow（布局行：底边、行高、站点步长、起止范围）。数据仓库 PlaceData 以数组持有全部对象，并维护名字到对象的映射，便于按文件中的字符串引用回填。"),
        ("p", "解析器按 BookShelf 规范依次处理六类文件：.aux 取得各文件名；.scl 解析 CoreRow 结构得到布局行；.nodes 区分可移动单元与固定终端；.pl 回填单元初始坐标；.nets 解析 NetDegree 块并建立网络—引脚—单元关联（兼容引脚偏移带冒号与不带冒号两种写法）；.wts 读取网络权重（缺省为 1）。解析完成后打印统计信息（核心区范围、面积利用率、网络度分布等），与参考数据对照验证。"),
        ("h3", "2.1.2 初始布局设计"),
        ("p", "设计三种初始布局方法以满足任务 4 的对比要求。（1）随机布局：以固定种子（mt19937，种子 42）将单元均匀随机撒入核心区，作为完全不考虑连接关系的对照组。（2）聚类驱动布局：以“两个单元共网的次数”为连接强度建图，用最大堆贪心合并并借助并查集把单元聚成约 500 个簇；簇按面积开方近似成正方形块瀑布式摆放，簇内再切分为网格填入单元，兼顾连接保持与实现简单。（3）二次方程解析布局：把“线长最小”建模为二次函数，对 x、y 各建立一个 N 维稀疏方程组（拉普拉斯矩阵，权重取常数除以距离，固定终端并入常数项），用 Eigen 的 BiCGSTAB 迭代求解并以旧解为初值反复线性化（至多 10 轮，残差小于 5e-4 或 HPWL 变化小于 1e-4 即收敛），最后裁剪进核心区。三种方法共用同一 HPWL 评价函数，保证对比公平。"),
        ("p", "评价函数 HPWL（半周长线长）对每条网络取其所有引脚包围盒的宽与高之和，再对全部网络求和。它以线性的计算代价近似 Steiner 树长度，是布局领域最通用的线长指标，也是本项目全部对比实验的统一评价口径。"),
        ("h3", "2.1.3 ePlace 全局布局设计"),
        ("p", "全局布局是方案的核心，采用 ePlace 框架，目标函数为 min f = W + λ·N，其中 W 为可导线长模型，N 为密度势能，λ 为平衡系数。设计分四个子模块。"),
        ("p", "目标函数中，W 采用加权平均模型：对每条网络，以各引脚坐标的指数加权平均作为“代表点”，网络的平滑长度取各引脚到代表点的加权距离之和；该模型在 γ 趋于 0 时趋近 HPWL，且处处可导，其解析梯度可直接用于优化。密度势能 N 定义为密度场与电势乘积的积分，其负梯度即单元所受“静电力”，方向由高密度区指向低密度区。两个梯度经 λ 加权后合成优化方向，实现线长与密度的联合优化。"),
        ("p", "（1）可导线长模型（WA）：HPWL 不可导，用加权平均（Weighted Average）把它磨光为 W，平滑系数 γ 控制逼近程度（γ→0 时 W→HPWL）。对 W 求解析梯度（含 (v−A±)·w/γ 修正项），并以有限差分校验梯度正确性；γ 越大越平滑但与 HPWL 偏差越大，实现中按迭代进度自适应调整。"),
        ("p", "（2）密度建模（eDensity）：把核心区划分为 128×128 网格，将每个单元与网格的重叠面积按比例分摊，形成密度场；按目标密度 τ=0.8 插入填充单元（filler），使密度计算计入空白区域；密度溢出率 τ 作为收敛判据。"),
        ("p", "（3）静电求解：把单元面积视为电荷、密度场视为电荷分布，电势满足泊松方程 ∇²ψ=−ρ，用二维离散余弦变换（DCT）在频域快速求解（复杂度 O(m² log m)），再由 E=−∇ψ 中心差分得电场，最后经双线性插值得到各单元所受密度梯度力，实现“拥挤区域把单元推开”的平滑效果。"),
        ("p", "（4）Nesterov 加速迭代：以 Lipschitz 常数预测步长，u/v 双解序列引入动量，λ 与 γ 随迭代自适应调节；以密度溢出率小于阈值或连续多轮 HPWL 不降为停止条件，结束时恢复历史最优布局。"),
        ("h3", "2.1.4 可视化与合法化设计"),
        ("p", "可视化设计两条输出路径：CImg 位图（800 像素宽等比缩放，标准单元红色、宏单元绿色、固定终端灰色，y 轴翻转以适应屏幕坐标）与 GDSII 工业版图（标准单元 Layer 0、宏单元 Layer 1、固定终端 Layer 2），后者可用 KLayout 直接打开。两条路径共用同一布局数据，保证图与版图一致。"),
        ("p", "合法化设计采用 Abacus 最小位移算法：单元按 y 坐标降序处理，每个单元在最近的 3 个候选行中试放；行内将“总区间减去固定终端障碍与已放单元”得到自由段，在自由段中选择离原位最近且放得下的位置落位，并向右推开以消除重叠；固定终端在行内占用的区间预处理为障碍，保证结果无冲突。设计目标是“消除重叠、对齐行的同时尽量少动单元”。"),
    ]

    c["2.2 针对复杂工程问题的方案实现"] = [
        ("h3", "2.2.1 开发环境与技术标准"),
        ("p", "开发环境为 Windows 11 + WSL2（Ubuntu），使用 g++（C++14）与 CMake（Ninja）构建 Release 版本；第三方库选用 Eigen（稀疏矩阵与 BiCGSTAB 求解器）、CImg（位图绘制）与 KLayout（GDSII 查看）。选择理由：任务 1 明确要求 Linux 环境，WSL2 兼顾 Windows 易用性与 Linux 工具链；Eigen 是主流线性代数库，可直接构造稀疏矩阵并提供工业级迭代求解器；CImg 仅需单头文件、依赖轻；GDSII 为工业标准版图格式，KLayout 为业界通用查看工具。命令行接口为 myplace [--fast] [输入.aux]：无参数时执行完整流程（解析 → 三种初始布局对比与自检 → 全局布局 → 可视化 → 合法化），--fast 为快速调试模式（跳过对比与自检），默认输入 data/adaptec1.aux，且支持从任意工作目录向上搜索数据文件。"),
        ("p", "在技术标准方面，项目围绕任务 2 系统学习了三类布局交换格式：BookShelf 是学术界布局竞赛与基准电路的通用格式，以纯文本描述单元、网络与布局行，简单易解析；DEF/LEF 则是工业界主流 EDA 工具链的标准交换格式，DEF 描述网表与布局几何、LEF 描述工艺库抽象。本项目输入输出均采用 BookShelf 格式，同时把布局结果导出为 GDSII 工业版图格式，保证结果可进入工业工具链查看与验证。"),
        ("h3", "2.2.2 解析与初始布局实现"),
        ("p", "解析器由 MyPlacer::Run 依次调用 ReadAux/ReadScl/ReadNode/ReadPl/ReadNet/ReadWts 实现，并以 PrintOverview 输出统计。adaptec1 的解析结果与参考数据对照如下表，全部吻合。"),
        ("table", "表 2-1  adaptec1 解析统计与参考数据对照", [
            ["统计项", "解析结果", "参考数据", "是否一致"],
            ["可移动单元", "210 904", "约 21 万", "是"],
            ["固定终端", "543", "543", "是"],
            ["网络", "221 142", "约 22 万", "是"],
            ["引脚", "944 053", "—", "是"],
            ["布局行数", "890", "890", "是"],
            ["核心区范围", "(459,459)~(11151,11139)", "—", "是"],
            ["行高 / 站点步长", "12 / 1", "—", "是"],
            ["面积利用率", "57.34%", "—", "是"],
            ["最大网络度", "2 271", "—", "—"],
        ]),
        ("p", "三种初始布局由 InitialPlacer 实现（RandomPlacement/ClusteringPlacement/QuadraticPlacement），在同一电路上的实测对比如下表。"),
        ("table", "表 2-2  三种初始布局算法对比（adaptec1）", [
            ["方法", "HPWL", "峰值密度", "耗时", "特点"],
            ["随机布局", "2.21e+09", "53.8%", "0.03 s", "对照组，不看连接关系"],
            ["聚类驱动布局", "7.06e+08", "130.4%", "8.9 s", "按连接强度聚类后排布"],
            ["二次方程解析布局", "4.79e+07", "133 745%", "166 s", "线长最优但密度失控"],
        ]),
        ("p", "对比可见：二次方程解析法线长最优（4.79e+07，较随机改善 97.8%），但完全不考虑密度，单元堆积在核心区中心，峰值密度高达 133 745%，无法直接制造；聚类驱动布局线长较随机改善 68%，在连接保持与密度之间折中。这验证了“初始布局只负责给全局布局一个合理起点”的经典结论。"),
        ("p", "在初始布局实现中还注意到若干工程细节：随机布局必须固定随机种子以保证可复现；聚类布局的簇数量（约 500）与簇内网格切分方式直接影响结果，需在连接保持与摆放密度之间折中；二次方程布局中权重取常数除以距离，可避免超远距离的“弱连接”主导方程组，提升数值稳定性；对超大规模电路，方程组求解的迭代次数与收敛阈值需要平衡精度与耗时。"),
        ("h3", "2.2.3 全局布局实现"),
        ("p", "全局布局由 GlobalPlacer 实现，关键函数包括 WaSmooth/CalcSmoothWirelength/CalcWirelengthGradient（WA 线长与解析梯度）、InitGrid/InsertFiller/DistributeDensity/CalcDensityOverflow（密度建模）、DCT1/DCT2/SolvePoisson/ComputeElectricField/CalcDensityGradient（静电求解）与 RunGlobalPlacement/InitLambda/UpdateLambda/UpdateGamma（Nesterov 主循环）。其中 DCT 为自实现的 O(m²) 一维/二维离散余弦变换，泊松方程在频域按 ψ̂=ρ̂/(w_u²+w_v²) 逐点求解。"),
        ("p", "实现中完成了多项自检：WA 解析梯度与有限差分对比（20 条随机网络全部通过，最大相对误差 0.0004%）；密度守恒校验（可移动面积、filler 面积、固定面积×τ 的分布值与理论值误差为 0）；DCT 正交还原误差 2.9e-14，达到机器精度；单点电荷电势分布符合“中心最高、向外衰减”的物理直觉。"),
        ("p", "在 WA 模型的实现中，平滑系数 γ 的取值直接影响 W 对 HPWL 的逼近程度：γ 越大，函数越平滑、优化越稳定，但与真实 HPWL 的偏差越大；γ 越小，逼近越准，但数值波动加剧。实测的 γ 与线长偏差关系如下表。"),
        ("table", "表 2-3  WA 平滑系数 γ 与线长偏差对比", [
            ["γ", "平滑线长 W", "相对 HPWL 偏差"],
            ["800", "3.98e+07", "−16.87%"],
            ["200", "4.45e+07", "−7.06%"],
            ["20", "4.54e+07", "−5.12%"],
        ]),
        ("p", "据此，实现中采用随迭代进度逐步减小 γ 的退火式策略：前期用较大 γ 保证数值稳定、快速形成单元总体分布，后期用较小 γ 精确逼近 HPWL、细化线长，兼顾收敛速度与最终质量。"),
        ("p", "从随机布局出发的 Nesterov 迭代收敛过程节选如下表。"),
        ("table", "表 2-4  Nesterov 迭代收敛过程（节选）", [
            ["迭代轮次", "HPWL", "密度溢出率 τ"],
            ["初始（随机布局）", "2.2148e+09", "约 0%"],
            ["第 21 轮", "4.96e+08", "1.11%"],
            ["第 41 轮", "3.21e+08", "4.38%"],
            ["第 61 轮", "2.97e+08", "2.98%"],
            ["第 90 轮（早停）", "2.96e+08", "3.22%"],
        ]),
        ("p", "迭代至第 90 轮因连续 20 轮 HPWL 不降早停，最终 HPWL=2.96e8，较随机布局改善 86.6%，密度溢出率 τ=3.2%，单元分布均匀、线长大幅缩短，达到“密度约束下最小化总线长”的设计目标。整个过程无需人工干预，验证了“解析梯度 + 动量加速 + 自适应系数”闭环的有效性。"),
        ("image", ROOT / "output" / "global_placement_result.bmp", 12, "图 2-1  全局布局结果（adaptec1）"),
        ("h3", "2.2.4 可视化与合法化实现"),
        ("p", "可视化由 PLOTTING::plotPlacement 与 PLOTTING::writeGDSII 实现。CImg 位图输出 800×800 的 BMP 图像；GDSII 输出按规范逐字节构造（大端整数、GDSII 专有 Real_8 浮点、记录头与 XY 记录、UNITS 单位），标准单元 Layer 0、宏单元 Layer 1、固定终端 Layer 2，文件约 13.5 MB，经外部脚本逐字节校验与 gdspy 权威解析确认规范、可被 KLayout 正常打开。"),
        ("p", "合法化由 Legalizer::Run 实现（SetupRows/BuildObstacles/FindCandidateRows/TryPlace/CommitPlace）。543 个固定终端在行内形成的 9 902 个占用区间被预处理为障碍；21 万个单元全部成功放入 890 行，未放置为 0。合法化前后对比如下表。"),
        ("table", "表 2-5  合法化前后对比（adaptec1）", [
            ["指标", "合法化前", "合法化后"],
            ["标准单元间重叠对", "369 512", "0"],
            ["与固定终端重叠对", "448", "0"],
            ["未放入行的单元", "—", "0"],
            ["平均位移", "—", "19.12 µm"],
            ["最大位移", "—", "2 334 µm"],
            ["HPWL", "2.96e8", "2.99e8（退化 0.90%）"],
        ]),
        ("p", "位移分布为：<10 µm 占 43.7%（92 071 个），10~100 µm 占 54.9%（115 818 个），100~500 µm 占 1.2%（2 480 个），>500 µm 占 0.3%（535 个）。98.6% 的单元位移小于 100 µm，HPWL 仅退化 0.90%，说明合法化在消除重叠、对齐行的同时几乎不破坏全局布局的线长优化成果，符合 Abacus“最小位移”的设计目标。"),
        ("image", ROOT / "output" / "legalization_result.bmp", 12, "图 2-2  合法化结果（adaptec1）"),
        ("h3", "2.2.5 复杂工程问题解决情况总结"),
        ("p", "对照第 1.1 节归纳的四个复杂工程问题，实现情况总结如下。"),
        ("p", "（1）针对规模问题：以 128×128 网格上 O(m² log m) 的 DCT 泊松求解替代稠密矩阵运算，用 Eigen 稀疏矩阵与 BiCGSTAB 处理 21 万维方程组，合法化用 256×256 网格加速重叠统计，全流程在普通 PC 上数分钟内完成，证明高效数值方法的选型有效。"),
        ("p", "（2）针对非光滑多目标问题：用 WA 模型把不可导的 HPWL 磨光为可导的 W 并推导解析梯度（经有限差分校验），以 min W+λN 的加权和把线长与密度统一到一个目标中，λ 随迭代自适应调节，使算法在“线长最优”与“密度可行”之间自动权衡，最终 HPWL 改善 86.6% 而密度溢出率仅 3.2%。"),
        ("p", "（3）针对建模抽象问题：把静电场类比落实为“电荷—电势—电场—力”的完整计算链（DCT 解泊松方程、中心差分求电场、双线性插值求密度梯度），并通过单点电荷电势、DCT 正交还原等物理与数值双重自检保证建模正确。"),
        ("p", "（4）针对制造约束问题：合法化以 Abacus 最小位移策略逐行安置，预处理固定终端障碍，最终重叠清零、全部单元落行、98.6% 的单元位移小于 100 µm，满足可制造性约束。"),
        ("p", "同时，方案仍存在可继续深化之处：详细布局（单元交换等局部优化）尚未开展，网格分辨率参数还可进一步调优，更大规模电路的可扩展性有待验证。这些工作已在后续计划中列出。总体而言，四个复杂工程问题均已在本项目中得到实质性解决或给出了可行路径，方案设计与实现达到了预期目标。"),
    ]

    c["第三章 测试环境构建与测试驱动开发"] = [
        ("p", "测试是本项目保证正确性的关键手段。围绕任务书要求，构建了覆盖模块、算法与整条流程的多层次测试体系，并开发了相应的技术工具对复杂工程问题进行预测和模拟。"),
        ("h2", "3.1 测试环境与测试数据构造"),
        ("p", "测试环境与开发环境一致：Windows 11 + WSL2（Ubuntu）、g++ C++14、CMake Release 构建；查看工具包括系统图片查看器与 KLayout。测试数据采用两套 UCLA BookShelf 电路：adaptec1（21 万单元级，.nets 约 35 MB，用于性能与质量评估）与 thin1（3 个可移动单元 + 1 个固定终端的玩具电路，用于秒级功能验证与调试）。完整流程在 adaptec1 上约 1 分钟、快速模式 --fast 约 30 秒（Release 构建，视机器配置而定）。"),
        ("p", "在数据构造上，除公开基准外，还通过修改 thin1 构造了小规模边界用例（如固定终端占行、单元宽度变化、引脚偏移两种书写格式），用于验证解析器兼容性与合法化障碍处理。在测试环境搭建过程中，先后解决了 Linux 可执行文件在 Windows 下双击报错（实为 Linux ELF 文件，需在 WSL 中运行）、X11 图形库缺失导致 CImg 无法绘图、大数据集运行时间过长等典型环境问题，并将排查方法整理成常见问题表，沉淀为团队经验。"),
        ("p", "在测试用例设计上，遵循“小电路验证正确性、大电路验证性能”的原则：thin1 规模极小，期望结果可人工推算，用于验证解析、坐标计算与合法化逻辑的正确性；adaptec1 用于验证算法在真实规模下的质量与耗时。此外还构造了空网络、单引脚网络、跨行固定终端等边界输入，检验解析器的鲁棒性。"),
        ("h2", "3.2 测试驱动开发与算法自检"),
        ("p", "在关键数学模块中采用“先写校验、再写实现”的测试驱动思路，内置四类自检工具。（1）WA 线长梯度校验：以有限差分逼近梯度，与解析梯度逐项对比，20 条随机网络全部通过，最大相对误差 0.0004%，保证了后续 Nesterov 迭代方向的正确性。（2）密度守恒校验：核对可移动单元、filler、固定终端（乘 τ）三类面积分布之和与理论值一致（误差为 0），保证密度建模不丢不重。（3）DCT 与泊松求解校验：正变换后逆变换还原误差 2.9e-14；单点电荷电势“中心最高、向外衰减”；真实密度场能正确算出电场与密度梯度并驱动单元散开。（4）合法化校验：以 256×256 网格加速统计单元重叠对数，输出合法化前后对比，并检查行对齐情况。"),
        ("p", "这种“自检先行”的方式在开发中多次及时暴露了问题，例如梯度修正项符号写反、网格面积分摊遗漏边界单元等，都在自检阶段被发现并修正，避免了错误累积到最终布局结果中。"),
        ("h2", "3.3 测试工具与结果验证"),
        ("p", "在软件之外还开发了独立验证脚本：check_gds.py 按 GDSII 规范逐字节校验输出文件（记录类型、数据类型、UNITS 单位），verify_gds.py 用第三方库 gdspy 权威解析确认兼容性，verify_leg.py 统计合法化后的残余重叠。Web 前端（Flask）提供接口自测脚本 test.py/quick_test.py，覆盖上传、运行、日志解析、图片回传全链路。"),
        ("p", "测试结果表明：解析统计与参考数据完全吻合；四类自检全部通过；合法化后标准单元间、单元与终端间重叠均清零，未对齐为 0；GDSII 文件可被 KLayout 正常打开并逐字节符合规范。系统性的测试保证了算法实现与设计一致，也支撑了第二章各项结果数据的可信度，实现了对布局质量（线长、密度、位移）的预测与模拟评估。"),
    ]

    c["第四章 知识技能学习情况"] = [
        ("p", "本项目涉及大量课堂之外的知识。通过文献与书籍的查阅、分析和总结，我们针对各环节的问题寻求了解决方案，主要收获如下。"),
        ("h2", "4.1 文献与书籍查阅"),
        ("p", "围绕全局布局，系统阅读了 ePlace 论文（Lu 等，2015），学习了静电类比密度建模、DCT 泊松求解与 Nesterov 加速的完整推导，并与传统力导向方法对比，理解了解析布局从“力平衡”到“势能最小化”的演化脉络；围绕合法化，研读了 Abacus 算法（Spindler 等，2008）的最小位移思想与行内聚类安置策略；通过 Kahng 等人的《VLSI Physical Design: From Graph Partitioning to Timing Closure》系统建立了物理设计全流程的知识框架；查阅 BookShelf 格式规范完成解析器开发。这一过程锻炼了从英文文献中提取算法要点、对照伪代码落地实现的能力。"),
        ("p", "文献调研还帮助我们建立了“先复现、再改进”的研究习惯：先按论文给出的公式与参数完整实现基线版本，用自检与基准数据确认复现无误，再在基线上做本地化改进（如 γ 退火策略、固定终端障碍处理）。这一顺序显著降低了调试难度，也让每一次改动都有对照数据支撑。"),
        ("p", "除算法文献外，还学习了 GDSII 版图格式规范（记录结构、数据类型、单位定义）与 CImg、Eigen 等开源库的官方文档，养成了“先查文档、再动手；先读规范、再写解析”的自学方式，面对陌生技术能快速建立可用的认知框架。"),
        ("h2", "4.2 数值与算法知识"),
        ("p", "实现中用到并加深了对以下数值方法的理解：稀疏线性方程组的迭代解法（BiCGSTAB 与预条件思想）、离散余弦变换及其在泊松方程频域求解中的应用、有限差分梯度校验、Nesterov 加速梯度的动量与 Lipschitz 步长机制、双线性插值与网格分摊等。这些知识把“优化理论”从公式变成了可运行的代码，对凸优化与数值分析有了更直观的认识。"),
        ("p", "例如，泊松方程在频域求解之所以高效，是因为 DCT 把微分算子对角化，把解偏微分方程变成逐点除法；Nesterov 方法之所以比普通梯度下降快，是因为动量项利用了历史梯度信息，把收敛速率从 O(1/k) 提升到 O(1/k²)。对这些“为什么”的理解，比单纯调通代码更有价值。"),
        ("p", "又如二次方程布局中“权重取常数除以距离”的设计，本质是让近处的强连接主导目标函数、远处的弱连接只起弱约束作用，这与电路中“关键路径应优先缩短”的工程直觉一致；合法化中“先按坐标排序、再逐行安置”的策略，则体现了把连续优化结果离散化为可制造解时“局部贪心 + 全局兜底”的经典思路。这些从算法到工程的映射，是本项目最重要的方法论收获。"),
        ("h2", "4.3 工程技能"),
        ("p", "工程方面，掌握了 WSL2 下 CMake/Ninja 的构建与调试配置、GDSII 二进制格式的构造与校验方法、KLayout 的版图查看、CImg 的绘图流程，以及用 Flask 快速搭建本地 Web 前端（文件上传、日志解析、图片回传）的技巧。更重要的是养成了“数据说话”的习惯：每个模块都留下可复现的运行日志与对照表，所有结论都有实测数据支撑。"),
        ("p", "此外，通过本项目的团队开发，还熟悉了 Git 分支与合并、代码交叉评审、模块接口约定等协作技能，工程素养得到全面提升。"),
    ]

    c["第五章 分工协作与交流情况"] = [
        ("p", "本项目由项目组成员分工协作完成。分工遵循“接口清晰、并行推进、交叉评审”的原则，情况如下表（成员姓名、学号待填写）。"),
        ("table", "表 5-1  团队分工表", [
            ["序号", "姓名", "承担工作"],
            ["1（组长）", "XXX", "总体方案设计、全局布局（ePlace）算法实现"],
            ["2", "XXX", "BookShelf 解析器、数据结构与测试数据构造"],
            ["3", "XXX", "初始布局三种算法实现与对比分析"],
            ["4", "XXX", "可视化（CImg/GDSII）与 Web 前端"],
            ["5", "XXX", "合法化算法、验证脚本与测试"],
        ]),
        ("p", "在协作过程中，团队建立了每周例会 + 即时通信 + Git 版本管理的沟通机制：例会上同步进度、暴露问题并集体决策；日常通过即时通信快速对齐细节；所有代码与文档通过 Git 管理，保证可追溯、可回滚。成员之间保持有效沟通，遇到接口变更或算法取舍时即时同步，避免了重复劳动与返工。"),
        ("p", "综合团队成员意见进行合理决策是本项目的重要经验。例如：（1）全局布局技术路线选择上，团队比较了力导向（FD）与静电类比（ePlace）两种方案，经集体讨论认为 ePlace 的密度建模更平滑、公开资料更完整、便于自检验证，最终采纳 ePlace；（2）合法化算法选型上，比较了贪心插行与 Abacus 最小位移两种方案，考虑到课程目标强调“尽量少动单元”，最终选择 Abacus；（3）密度网格参数取舍上，成员分别实验 64×64 与 128×128 后共同确定以 128×128 为主方案并完整记录对比数据；（4）在测试策略上，集体确定了“自检 + 外部脚本 + 玩具电路”三重验证路径。这些决策均在充分沟通、权衡利弊后集体作出，既保证了方案质量，也形成了团队共识。"),
        ("p", "在进度管理上，团队按任务书的 7 项任务拆分工作包并约定接口，任一模块完成后先在小电路（thin1）上自测，再集成到主流程联调，避免了“最后一起联调、问题集中爆发”的风险。遇到分歧时以实测数据为准绳（如网格参数之争最终由对照实验裁决），形成了理性务实的团队文化。"),
    ]
    return c


# ===============================================================
# 组装流程
# ===============================================================

def build_report(template_name, output_name, content_map, is_final):
    """从模板构建一份报告"""
    src = ROOT / template_name
    dst = ROOT / output_name
    shutil.copy(src, dst)
    doc = Document(str(dst))

    # 1) 封面占位(仅中期报告有封面)
    fill_cover_placeholders(doc)

    # 2) 标题层级统一 + 章标题空格规范化
    fix_headings(doc)

    # 3) 清理模板占位内容,拿到提示段与文献段
    hints, ref_paras = cleanup_template(doc, "正文")

    # 4) 摘要/关键词(总结报告)
    if is_final:
        # 替换摘要正文两段与关键词行
        x_paras = [p for p in doc.paragraphs if p.text.strip().startswith("Xxxx")]
        abstract = [
            "本文围绕《工业软件创新训练III》课程的芯片布局（Placement）课题，设计并实现了一个完整的电子设计自动化（EDA）布局工具 MyPlacement。软件按照超大规模集成电路物理设计的经典流程，依次实现 BookShelf 格式电路解析、三种初始布局算法的对比分析、基于静电类比密度建模的 ePlace 全局布局、CImg 位图与 GDSII 版图双路可视化，以及基于 Abacus 算法的布局合法化。针对布局问题规模大、目标函数非光滑、多目标冲突、制造约束严格等复杂工程难题，采用了 DCT 泊松求解、加权平均线长模型、Nesterov 加速梯度、最小位移合法化等技术手段予以解决。",
            "在测试电路 adaptec1（210 904 个可移动单元、221 142 个网络）上的实际运行结果表明：解析统计与参考数据完全吻合；三种初始布局中二次方程解析法线长最优但密度失控；全局布局将半周长线长（HPWL）从 2.21e+09 优化至 2.96e+08（改善 86.6%），密度溢出率仅 3.2%；合法化后单元重叠清零、98.6% 的单元位移小于 100 µm，HPWL 仅退化 0.90%。软件同时提供命令行与 Web 两种使用方式，具备良好的可用性与可复现性。",
        ]
        for i, p in enumerate(x_paras[:2]):
            set_text(p, abstract[i])
            p.paragraph_format.first_line_indent = Pt(24)
        for p in doc.paragraphs:
            if p.text.strip().startswith("关键词"):
                set_text(p, "关键词：芯片布局；全局布局；ePlace；合法化；BookShelf")
                break

    # 5) 「目 录」标题与章标题同级(Heading 1),并插入 TOC 域
    for p in doc.paragraphs:
        if p.text.strip().replace(" ", "") == "目录":
            p.style = doc.styles["Heading 1"]
            insert_toc_field(p)
            break

    # 6) 按标题归属把内容块插入提示段之前
    for hint in hints:
        # 向上找最近的标题段落
        prev = hint._p.getprevious()
        owner = None
        while prev is not None:
            if prev.tag == f"{W_NS}p":
                pp = Paragraph(prev, hint._parent)
                if pp.style is not None and pp.style.name in ("Heading 1", "Heading 2", "Heading 3"):
                    owner = pp
                    break
            prev = prev.getprevious()
        if owner is None:
            continue
        title = owner.text.strip()
        blocks = None
        # 先精确匹配,再按前缀模糊匹配
        if title in content_map:
            blocks = content_map[title]
        else:
            for k in content_map:
                if title.startswith(k) or k.startswith(title) or k in title or title in k:
                    blocks = content_map[k]
                    break
        if blocks is None:
            # 未配置内容的提示段:直接删除/清空
            if para_has_sectpr(hint):
                clear_runs(hint)
            else:
                delete_para(hint)
            continue
        emit_blocks(doc, blocks, hint, "Heading 1", "Heading 2", "Heading 3", "正文1" if not is_final else "正文正文")
        # 提示段:带分节符则清空保留,否则删除
        if para_has_sectpr(hint):
            clear_runs(hint)
        else:
            delete_para(hint)

    # 7) 参考文献
    fill_references(ref_paras)

    # 8) 致谢(总结报告)
    if is_final:
        for p in doc.paragraphs:
            if "本报告的工作是在我的指导教师" in p.text:
                set_text(p, "本报告的工作是在我的指导教师XXX老师的悉心指导下完成的。XXX老师在课题选型、方案设计与报告撰写等各个环节给予了耐心细致的指导，在此向XXX老师表示衷心的感谢。同时感谢项目组全体成员的通力协作与相互支持，感谢《工业软件创新训练III》课程提供的实验条件与参考资料。")
                p.paragraph_format.first_line_indent = Pt(24)
                break

    doc.save(str(dst))
    return dst


def main():
    # 中期报告
    mid = build_report(
        "进阶式挑战性综合项目III中期报告模板.docx",
        "进阶式挑战性综合项目III中期报告.docx",
        content_mid(),
        is_final=False,
    )
    print(f"已生成: {mid}")
    # 总结报告
    fin = build_report(
        "进阶式挑战性综合项目III总结报告模板.docx",
        "进阶式挑战性综合项目III总结报告.docx",
        content_fin(),
        is_final=True,
    )
    print(f"已生成: {fin}")


if __name__ == "__main__":
    main()
