"""把教学动画和项目样图打包成离线网页，同时生成聊天内展示用片段。"""

from __future__ import annotations

import argparse
import base64
import json
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fragment", type=Path, help="可选：另存可直接展示的HTML片段")
    args = parser.parse_args()
    report = json.loads((ROOT / "05_data/03_results/02_perspective/20261023_透视变换结果.json").read_text(encoding="utf-8"))
    photo = ROOT / report["input"]
    data = {"source_points": report["source_points"], "image": "data:image/jpeg;base64," + base64.b64encode(photo.read_bytes()).decode("ascii")}
    fragment = (HERE / "perspective-transform.html").read_text(encoding="utf-8").replace("__PROJECT_SAMPLE__", json.dumps(data, ensure_ascii=False))
    if "__PROJECT_SAMPLE__" in fragment or len(fragment.encode("utf-8")) >= 1_000_000:
        raise ValueError("动画数据未正确嵌入或文件超出大小限制")
    if args.fragment:
        args.fragment.parent.mkdir(parents=True, exist_ok=True)
        args.fragment.write_text(fragment, encoding="utf-8")
    # 网页外壳只提供主题与基础控件样式；动画逻辑始终来自同一个片段。
    shell = '''<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><meta name="color-scheme" content="light dark"><title>透视变换原理动画</title>
<style>
:root { color-scheme:light dark; --font-size-base:16px; --background:light-dark(#ffffff,#17191c); --foreground:light-dark(#202428,#e8eaed); --muted-foreground:light-dark(#59616a,#aeb6bf); --muted:light-dark(#e8ecf0,#30363e); --border:light-dark(#c4cbd3,#505964); --primary:var(--foreground); --primary-foreground:var(--background); --viz-series-1:light-dark(#147d85,#65cbd3); --viz-series-2:light-dark(#a05e2c,#eab280); }
* { box-sizing:border-box; } body { margin:0; background:var(--background); color:var(--foreground); font:400 16px/1.5 "Microsoft YaHei",sans-serif; } main { max-width:850px; margin:auto; padding:24px; } h2 { margin:0; font-size:20px; font-weight:500; } .text-small { font-size:14px; } .tabular-nums { font-variant-numeric:tabular-nums; } .btn { font:inherit; border:0; border-radius:7px; padding:8px 16px; background:var(--muted); color:var(--foreground); } .btn-primary { background:var(--primary); color:var(--primary-foreground); } .form-range { width:100%; accent-color:var(--foreground); } .reading { margin-top:24px; color:var(--muted-foreground); } .reading p { margin:12px 0; } a { color:var(--foreground); } @media(max-width:450px) { main { padding:14px; } .btn { min-height:44px; } input { min-height:44px; } }
</style></head><body><main>
__FRAGMENT__
<div class="reading"><p>这段动画对应项目的OpenCV透视变换：四组共面对应点确定坐标换算关系，再让输出像素回原图找位置、插值取色。播放中的渐变是教学过程，实际算法直接计算最终结果。</p><p>它只能拉正选定平面；不能补出遮挡内容，也不能直接把地面与棋子顶面都还原。矩形长宽比需要已知几何支持。</p><p>末段照片来自OpenCV官方实拍样图；四点围住5×8个内角点方格间距，并非整张棋盘边缘。页面内校正图由浏览器实际计算，未做镜头去畸变。<a href="https://docs.opencv.org/4.13.0/d9/dab/tutorial_homography.html">原理来源</a> · <a href="https://docs.opencv.org/4.13.0/da/d54/group__imgproc__transform.html">像素取样来源</a> · <a href="https://github.com/opencv/opencv/blob/4.13.0/LICENSE">样图许可</a></p></div>
</main></body></html>'''
    (HERE / "index.html").write_text(shell.replace("__FRAGMENT__", fragment), encoding="utf-8")
    print("已生成离线网页：" + str(HERE / "index.html"))
    if args.fragment:
        print("已生成展示片段：" + str(args.fragment.resolve()))


if __name__ == "__main__":
    main()
