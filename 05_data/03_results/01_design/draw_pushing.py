"""推送几何概念图的可编辑绘图源；非正式场地工程图。

从2026-10-23已有绘图程序保留当前推送图部分，清除已停用的架构图函数。
依赖Pillow和Windows微软雅黑。默认输出同目录01_推送几何.png；
可用--output指定复核路径，避免覆盖正式结果。
"""
import argparse
from pathlib import Path
import math
from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parent
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



def pushing(output: Path):
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
    im.save(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUT / '01_推送几何.png')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    pushing(args.output)
    print('已生成：', args.output)
