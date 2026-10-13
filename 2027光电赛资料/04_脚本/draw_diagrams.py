"""生成汇报用概念示意图；非正式场地工程图。依赖 Pillow。

用法：python draw_diagrams.py
输出固定到脚本上级的 03_图示，不向工作目录写临时文件。
"""
from pathlib import Path
import math
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "03_图示"
OUT.mkdir(parents=True, exist_ok=True)
FONT = Path("C:/Windows/Fonts/msyh.ttc")
BG, INK, MUTED = "#F5F7FB", "#16243B", "#59697F"
BLUE, GREEN, AMBER = "#246BFD", "#158675", "#B96B10"


def f(size):
    return ImageFont.truetype(str(FONT), size)


def text(d, xy, s, size=28, fill=INK):
    d.text(xy, s, font=f(size), fill=fill)


def arrow(d, points, fill=BLUE, width=5):
    d.line(points, fill=fill, width=width, joint="curve")
    a, b = points[-2], points[-1]
    t = math.atan2(b[1] - a[1], b[0] - a[0])
    h, w = 18, 10
    d.polygon([b,
               (b[0] - h * math.cos(t) + w * math.sin(t),
                b[1] - h * math.sin(t) - w * math.cos(t)),
               (b[0] - h * math.cos(t) - w * math.sin(t),
                b[1] - h * math.sin(t) + w * math.cos(t))], fill=fill)


def card(d, box, title, lines, accent=BLUE):
    x0, y0, x1, y1 = box
    d.rounded_rectangle(box, radius=20, fill="white", outline="#DDE4EF", width=2)
    d.rounded_rectangle((x0, y0, x0 + 9, y1), radius=4, fill=accent)
    text(d, (x0 + 26, y0 + 20), title, 32, accent)
    for i, line in enumerate(lines):
        text(d, (x0 + 26, y0 + 76 + i * 44), line, 27)


def architecture():
    im = Image.new("RGB", (1800, 1040), BG)
    d = ImageDraw.Draw(im)
    text(d, (70, 42), "推荐 A：车载视觉闭环 + 差速推送", 48)
    text(d, (72, 112), "设计建议｜Linux 负责理解与决策，STM32 负责实时控制｜所有相机与计算均在车上", 28, MUTED)
    card(d, (70, 215, 460, 464), "车载感知", ["广角巡扫：棋子 / 对手 / 门", "近场俯视：字样 / 推送偏差", "激光辅助测距 + 编码器 / IMU"])
    card(d, (510, 215, 900, 464), "Linux 主控", ["树莓派 5 原型，可迁移 CM5", "带置信度的局部地图", "目标选择 + 路径 / 速度规划"])
    card(d, (950, 215, 1340, 464), "STM32", ["轮速闭环 + 里程计", "心跳监测 + 故障停车", "读取接触 / 电流反馈"])
    card(d, (1390, 215, 1730, 464), "轻质机构", ["双轮差速 + 支撑轮", "开放式浅 V 推板", "顺应接触，允许退出"], GREEN)
    for a, b in [(460, 510), (900, 950), (1340, 1390)]:
        arrow(d, [(a + 7, 330), (b - 8, 330)])
    arrow(d, [(1530, 480), (1530, 529), (270, 529), (270, 478)], GREEN)
    text(d, (612, 480), "持续复核：棋子是否丢失、偏移、卡住，是否整体越过门线", 28, GREEN)
    card(d, (70, 595, 850, 827), "智能决策", ["按成功概率 × 估计分值 / 完整任务耗时排序", "看不清：靠近观察；对手接近：换目标 / 绕行", "加入目标切换滞回，避免反复犹豫"])
    card(d, (900, 595, 1730, 827), "闭环任务状态", ["巡扫 → 确认 → 到棋子背后 → 对准 → 推送 → 复核", "偏差增大 / 超时 / 堵转：退出，重定位，再决策", "地图过期或主控失联：STM32 停车"], GREEN)
    text(d, (74, 884), "工程优先级：单枚稳定推送 → 随机场完整得分 → 对抗决策 → 仿真学习增益", 32)
    text(d, (74, 944), "激光按原文“≤1 mW、II 类”预留并参与近场感知；正式细则发布后复核。", 26, MUTED)
    im.save(OUT / "01_推荐架构.png")


def pushing():
    im = Image.new("RGB", (1600, 1080), BG)
    d = ImageDraw.Draw(im)
    text(d, (65, 36), "推送几何：先到棋子背后，再沿目标方向送入门", 43)
    text(d, (67, 104), "概念示意｜场地 3 m × 2 m｜球门宽度、棋子和车辆大小未按比例绘制", 27, MUTED)
    x0, y0, x1, y1 = 220, 200, 1360, 960
    d.rectangle((x0, y0, x1, y1), fill="white", outline=INK, width=5)
    d.rectangle((x0 - 12, 478, x0 + 24, 716), fill="#DCE9FF", outline=BLUE, width=3)
    d.rectangle((x1 - 24, 478, x1 + 12, 716), fill="#FFE4E3", outline="#CF4949", width=3)
    text(d, (59, 530), "己方门", 30, BLUE)
    text(d, (60, 580), "门宽待定", 24, MUTED)
    text(d, (1390, 550), "对方门", 29, "#CF4949")
    text(d, (751, 158), "3 m", 27, MUTED)
    text(d, (1390, 850), "2 m", 27, MUTED)
    goal = (244, 600)
    piece = (760, 600)
    car = (918, 600)
    # 对手的不确定运动区域；非规则规定的禁入区。
    d.ellipse((665, 260, 980, 490), fill="#FBE9D0", outline="#E8BA71", width=2)
    d.rounded_rectangle((777, 313, 892, 412), radius=17, fill="#C28339")
    text(d, (723, 279), "对手 + 预测余量", 24, AMBER)
    arrow(d, [(760, 600), (goal[0] + 36, goal[1])], GREEN, 7)
    text(d, (392, 639), "② 沿线低速推送，持续修正偏差", 27, GREEN)
    d.ellipse((728, 568, 792, 632), fill="#FFF6D2", outline=AMBER, width=4)
    text(d, (744, 579), "车", 26, AMBER)
    text(d, (696, 684), "目标棋子", 26)
    d.rounded_rectangle((870, 556, 966, 644), radius=13, fill=BLUE)
    d.line([(878, 558), (856, 583), (856, 617), (878, 642)], fill=INK, width=7)
    text(d, (902, 577), "我", 26, "white")
    text(d, (892, 663), "对准位", 26, BLUE)
    arrow(d, [(1120, 816), (1140, 738), (1115, 670), (1035, 610), (978, 600)], BLUE)
    text(d, (994, 850), "① 绕行到背后", 27, BLUE)
    for px, py in [(502, 372), (500, 790), (1240, 340), (1240, 840)]:
        d.ellipse((px - 20, py - 20, px + 20, py + 20), fill="#E5E9F0", outline="#8C9BAE", width=2)
    text(d, (263, 895), "③ 整体越线后复核；最终得分以裁判判定为准", 25, GREEN)
    text(d, (67, 1008), "导航路径要考虑整车轮廓、围挡、其他棋子和对手；推送轨迹还要考虑棋子的运动。", 27, MUTED)
    im.save(OUT / "02_推送几何.png")


if __name__ == "__main__":
    architecture()
    pushing()
    print("已生成：", OUT)
