"""四点透视变换：默认校正实拍棋盘，也可用 --points 校正自己的平面照片。

最小示例：python 02_programs/01_host/01_perspective/perspective_demo.py
点序：目标平面的左上、右上、右下、左下（须一一对应，不能交叉）。
本例的四角是检测网格的四角，不是整张棋盘的外边界。
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[3]
DEFAULT_INPUT = ROOT / "05_data/01_raw/01_perspective/20261023_perspective_input_01.jpg"
DEFAULT_OUTPUT = ROOT / "05_data/03_results/02_perspective"
DEFAULT_REPORT = DEFAULT_OUTPUT / "20261023_透视变换结果.json"
PREFIX = "20261023_perspective"
SAMPLE_SHA256 = "04bdd78b3b507fe0551aa7062a3ffd94846a147ee2bea7fb38082c720bdbe231"
SAMPLE_SOURCE = "https://raw.githubusercontent.com/opencv/opencv/4.13.0/samples/data/left12.jpg"


def read_image(path: Path) -> np.ndarray:
    """imdecode/fromfile 兼容 Windows 中文路径；输入文件保持原样。"""
    image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"无法读取图像：{path}")
    return image


def write_image(path: Path, image: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ok, encoded = cv2.imencode(path.suffix, image)
    if not ok:
        raise ValueError(f"图像编码失败：{path}")
    encoded.tofile(path)


def rectify(
    image: np.ndarray, points: np.ndarray, size: tuple[int, int]
) -> tuple[np.ndarray, np.ndarray]:
    """由四组对应点求 H，将输入平面映射到指定宽、高的矩形。

    size=(width, height)，而 image.shape 的前两项是 (height, width)。
    输出比例须根据目标平面的实际长宽比指定，四点本身不提供真实尺度。
    """
    source = np.asarray(points, dtype=np.float32)
    if source.shape != (4, 2) or not np.isfinite(source).all():
        raise ValueError("需要四个有限的二维坐标。")
    height, width = image.shape[:2]
    if np.any(source < 0) or np.any(source > [width - 1, height - 1]):
        raise ValueError("对应点必须位于输入图像内。")
    if not cv2.isContourConvex(source) or abs(cv2.contourArea(source)) < 1:
        raise ValueError("四点须围成非退化的凸四边形，并按边界顺序排列。")
    out_width, out_height = size
    if min(out_width, out_height) < 2:
        raise ValueError("输出宽、高必须至少为 2 像素。")

    # 核心算法：四个原图点 -> 目标矩形四角 -> H -> 透视图像重采样。
    target = np.float32(
        [[0, 0], [out_width - 1, 0],
         [out_width - 1, out_height - 1], [0, out_height - 1]]
    )
    matrix = cv2.getPerspectiveTransform(source, target)
    birdview = cv2.warpPerspective(image, matrix, size, flags=cv2.INTER_LINEAR)
    return birdview, matrix


def detect_board(image: np.ndarray, pattern: tuple[int, int]) -> np.ndarray:
    """返回棋盘内角点，按检测网格逐行排列；方向由检测器决定。"""
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    ok, corners = cv2.findChessboardCorners(
        gray, pattern, cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE
    )
    if not ok:
        raise ValueError(f"未检测到 {pattern[0]}×{pattern[1]} 内角点，可用 --points 指定四点。")
    cv2.cornerSubPix(
        gray, corners, (11, 11), (-1, -1),
        (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001),
    )
    return corners.reshape(-1, 2)


def check_grid(
    corners: np.ndarray, matrix: np.ndarray, pattern: tuple[int, int],
    size: tuple[int, int], fitted_indices: list[int],
) -> dict:
    """检查未参与 H 求解的角点，残差单位为输出像素，不是真实毫米误差。"""
    columns, rows = pattern
    x, y = np.meshgrid(np.arange(columns), np.arange(rows))
    expected = np.column_stack((
        x.ravel() * (size[0] - 1) / (columns - 1),
        y.ravel() * (size[1] - 1) / (rows - 1),
    ))
    observed = cv2.perspectiveTransform(corners[:, None, :], matrix)[:, 0, :]
    keep = [i for i in range(len(corners)) if i not in fitted_indices]
    errors = np.linalg.norm(observed[keep] - expected[keep], axis=1)
    return {
        "description": "同图中未参与四点拟合的内角点；非独立测试集、非毫米精度",
        "count": len(keep),
        "rmse_output_px": float(np.sqrt(np.mean(errors ** 2))),
        "mean_output_px": float(np.mean(errors)),
        "max_output_px": float(np.max(errors)),
        "pixels_per_grid_x": (size[0] - 1) / (columns - 1),
        "pixels_per_grid_y": (size[1] - 1) / (rows - 1),
        "points": [
            {"index": i, "expected": expected[i].tolist(),
             "observed": observed[i].tolist(), "error_px": float(error)}
            for i, error in zip(keep, errors)
        ],
    }


def annotate(image: np.ndarray, points: np.ndarray) -> np.ndarray:
    marked = image.copy()
    polygon = np.rint(points).astype(np.int32)
    cv2.polylines(marked, [polygon], True, (0, 196, 255), 2, cv2.LINE_AA)
    for index, (x, y) in enumerate(polygon, start=1):
        cv2.circle(marked, (int(x), int(y)), 6, (0, 196, 255), -1)
        position = (int(x + 9), int(y - 9))
        cv2.putText(marked, str(index), position, cv2.FONT_HERSHEY_SIMPLEX,
                    0.75, (0, 0, 0), 4, cv2.LINE_AA)
        cv2.putText(marked, str(index), position, cv2.FONT_HERSHEY_SIMPLEX,
                    0.75, (0, 230, 255), 2, cv2.LINE_AA)
    return marked


def font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    # Windows 默认字体；Linux 可安装 Noto CJK，或通过 --font 明确指定。
    candidates = [
        "C:/Windows/Fonts/msyh.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/System/Library/Fonts/PingFang.ttc",
    ]
    if FONT_PATH:
        candidates.insert(0, FONT_PATH)
    for candidate in candidates:
        if Path(candidate).is_file():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


FONT_PATH: str | None = None


def comparison(
    original: np.ndarray, result: np.ndarray, check: dict | None, source_label: str
) -> Image.Image:
    """比较图保持两侧各自宽高比；左侧明确显示的是原图标注副本。"""
    canvas = Image.new("RGB", (1600, 1010), "#f3f6fa")
    draw = ImageDraw.Draw(canvas)
    draw.text((48, 30), "Perspective 透视变换：斜视平面 → 俯视平面", font=font(35), fill="#142b47")
    explanation = "其他内角点检查误差" if check else "输出比例由 --size 指定"
    draw.text((48, 86), f"{source_label}  |  四点确定映射，{explanation}", font=font(22), fill="#53657a")
    for left, label, image in [
        (48, "原图 + 四点标注", original),
        (824, "变换后：矩形俯视图", result),
    ]:
        draw.rounded_rectangle((left, 142, left + 728, 884), radius=16, fill="white")
        draw.text((left + 24, 162), label, font=font(27), fill="#142b47")
        panel = Image.fromarray(cv2.cvtColor(image, cv2.COLOR_BGR2RGB))
        panel.thumbnail((680, 632), Image.Resampling.LANCZOS)
        canvas.paste(panel, (left + (728 - panel.width) // 2, 222 + (632 - panel.height) // 2))
    if check:
        status = (f"{check['count']} 个未参与拟合的检查点  |  "
                  f"RMSE {check['rmse_output_px']:.2f} px  |  "
                  f"每格 {check['pixels_per_grid_x']:.0f} × {check['pixels_per_grid_y']:.0f} px")
    else:
        status = "手动四点模式：未提供额外检查点，因此不报告几何精度。"
    draw.text((48, 909), status, font=font(24), fill="#12695c")
    draw.text((48, 957), "只校正所选共面区域；本例未去镜头畸变，不代表项目相机的定位精度。", font=font(21), fill="#53657a")
    return canvas


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--report", type=Path, help="JSON 结果路径；自定义输出目录时默认保存为该目录 metrics.json")
    parser.add_argument("--pattern", type=int, nargs=2, default=(6, 9), metavar=("COLS", "ROWS"))
    parser.add_argument("--pixels-per-square", type=int, default=100)
    parser.add_argument("--points", type=float, nargs=8, metavar="COORD", help="四点 x1 y1 x2 y2 x3 y3 x4 y4")
    parser.add_argument("--size", type=int, nargs=2, metavar=("WIDTH", "HEIGHT"))
    parser.add_argument("--font", help="比较图使用的中文字体路径")
    args = parser.parse_args()
    if min(args.pattern) < 3 or args.pixels_per_square < 1:
        parser.error("棋盘每个方向至少需要 3 个内角点，每格像素数须为正。")
    if args.points and not args.size:
        parser.error("--points 模式须用 --size 指定符合实际平面比例的输出尺寸。")
    return args


def main() -> None:
    global FONT_PATH
    args = parse_args()
    FONT_PATH = args.font
    image = read_image(args.input)
    columns, rows = args.pattern
    check = None
    corners = None
    size = tuple(args.size or (
        (columns - 1) * args.pixels_per_square + 1,
        (rows - 1) * args.pixels_per_square + 1,
    ))
    if args.points:
        source = np.float32(args.points).reshape(4, 2)
        mode = "manual_four_points"
    else:
        corners = detect_board(image, tuple(args.pattern))
        fitted = [0, columns - 1, columns * rows - 1, columns * (rows - 1)]
        source = corners[fitted]
        mode = "chessboard_four_points"
    birdview, matrix = rectify(image, source, size)
    if corners is not None:
        check = check_grid(corners, matrix, tuple(args.pattern), size, fitted)

    annotated = annotate(image, source)
    outputs = {
        "annotated": args.output_dir / f"{PREFIX}_annotated_01.png",
        "birdview": args.output_dir / f"{PREFIX}_birdview_01.png",
        "comparison": args.output_dir / f"{PREFIX}_comparison_01.png",
    }
    report = args.report or (
        DEFAULT_REPORT if args.output_dir == DEFAULT_OUTPUT else args.output_dir / "metrics.json"
    )
    destinations = [path.resolve() for path in [*outputs.values(), report]]
    if args.input.resolve() in destinations or len(set(destinations)) != len(destinations):
        raise ValueError("输出路径不能覆盖输入文件，图像与报告也必须使用不同路径。")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    input_hash = hashlib.sha256(args.input.read_bytes()).hexdigest()
    source_label = "OpenCV 官方实拍棋盘样图" if input_hash == SAMPLE_SHA256 else "输入平面照片"
    write_image(outputs["annotated"], annotated)
    write_image(outputs["birdview"], birdview)
    comparison(annotated, birdview, check, source_label).save(outputs["comparison"])

    def stored_path(path: Path) -> str:
        absolute = path.resolve()
        return absolute.relative_to(ROOT).as_posix() if absolute.is_relative_to(ROOT) else str(absolute)

    metadata = {
        "mode": mode, "input": stored_path(args.input),
        "input_sha256": input_hash,
        "source_url": SAMPLE_SOURCE if input_hash == SAMPLE_SHA256 else None,
        "opencv": cv2.__version__, "numpy": np.__version__,
        "input_size_wh": [image.shape[1], image.shape[0]],
        "output_size_wh": list(size), "source_points": source.tolist(),
        "homography_source_to_output": matrix.tolist(), "lens_undistorted": False,
        "validation": check, "outputs": {key: stored_path(path) for key, path in outputs.items()},
    }
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n",
                      encoding="utf-8", newline="\n")
    print("输入四点（按输出矩形的左上、右上、右下、左下对应）：")
    print(source)
    print("原图 → 俯视图的 H：")
    print(matrix)
    if check:
        print(f"{check['count']} 个非拟合检查点：RMSE={check['rmse_output_px']:.3f} 输出像素，"
              f"最大误差={check['max_output_px']:.3f} 输出像素")
    for label, path in outputs.items():
        print(f"{label}: {path}")
    print(f"report: {report}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, cv2.error) as error:
        raise SystemExit(f"错误：{error}") from error
