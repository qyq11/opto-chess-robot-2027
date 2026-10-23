"""低机位象棋图 -> 四个参考点的 Perspective 棋面二维校正 -> 同流程 OCR 对照。

默认样例使用 AI 生成图。四个棋面参考点人工对应到演示用目标矩形，
假定这些点位于同一平面；不代表真实尺寸标定、真实 3D 俯视或自动定位。
此程序不读取人工字符答案；校正图由 OpenCV 的单应变换实际计算。
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import time

import cv2
import numpy as np
from PIL import Image, ImageDraw
from rapidocr import RapidOCR

from recognize_pieces import (
    DEFAULT_IMAGES, DEFAULT_MODEL, MODEL_SHA256, ROOT, TYPE_NAMES,
    classify_text, crop_region, get_font, ink_color, read_image,
    relative_path, rgb,
)


DEFAULT_CONFIG = ROOT / "05_data/01_raw/02_xiangqi/generated_oblique_scene.json"
DEFAULT_OUTPUT = ROOT / "05_data/03_results/03_xiangqi/generated_oblique_predictions.json"
DEFAULT_PREFIX = "20261023_xiangqi_oblique_"


def project_path(path: Path | str) -> Path:
    candidate = Path(path)
    return (candidate if candidate.is_absolute() else ROOT / candidate).resolve()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def points_array(value: object, name: str) -> np.ndarray:
    try:
        points = np.asarray(value, dtype=np.float32)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{name} 必须是四个二维点。") from error
    if points.shape != (4, 2) or not np.isfinite(points).all():
        raise ValueError(f"{name} 必须是四个有限二维点。")
    contour = points.reshape(-1, 1, 2)
    if not cv2.isContourConvex(contour) or abs(cv2.contourArea(contour)) < 1:
        raise ValueError(f"{name} 必须按四边形的边界顺序排列，且不能重复或共线。")
    return points


def positive_size(value: object, name: str) -> tuple[int, int]:
    if (not isinstance(value, list) or len(value) != 2
            or any(type(v) is not int or v < 2 for v in value)):
        raise ValueError(f"{name} 必须是两个至少为 2 的整数 [宽,高]。")
    return value[0], value[1]


def load_config(path: Path) -> dict:
    config = json.loads(path.read_text(encoding="utf-8"))
    fields = {"image", "synthetic", "source_centers", "target_centers", "output_size",
              "geometry_assumption", "pieces"}
    if not isinstance(config, dict) or set(config) != fields:
        raise ValueError("配置字段必须为 image、synthetic、source_centers、target_centers、"
                         "output_size、geometry_assumption、pieces；不得混入字符答案。")
    if not isinstance(config["image"], str) or type(config["synthetic"]) is not bool:
        raise ValueError("image 必须为图像路径字符串，synthetic 必须为布尔值。")
    if not isinstance(config["geometry_assumption"], str) or not config["geometry_assumption"].strip():
        raise ValueError("须明确写出 geometry_assumption 几何假设。")
    positive_size(config["output_size"], "output_size")
    points_array(config["source_centers"], "source_centers")
    points_array(config["target_centers"], "target_centers")
    pieces = config["pieces"]
    if not isinstance(pieces, list) or not pieces:
        raise ValueError("pieces 必须是非空 ROI 清单。")
    ids = []
    for piece in pieces:
        if not isinstance(piece, dict) or set(piece) != {
                "id", "source_bbox", "target_center", "target_crop_size"}:
            raise ValueError("每枚棋子只允许 id、source_bbox、target_center、target_crop_size 字段。")
        if not isinstance(piece["id"], str) or not piece["id"]:
            raise ValueError("棋子 id 必须是非空字符串。")
        ids.append(piece["id"])
        center = np.asarray(piece["target_center"], dtype=float)
        if center.shape != (2,) or not np.isfinite(center).all():
            raise ValueError("每枚棋子的 target_center 必须是有限二维点。")
        positive_size(piece["target_crop_size"], "target_crop_size")
    if len(set(ids)) != len(ids):
        raise ValueError("棋子 id 不可重复。")
    return config


def target_box(piece: dict, image: np.ndarray) -> list[int]:
    width, height = positive_size(piece["target_crop_size"], "target_crop_size")
    cx, cy = piece["target_center"]
    x1, y1 = round(cx - width / 2), round(cy - height / 2)
    box = [x1, y1, x1 + width, y1 + height]
    crop_region(image, box)  # 越界时拒绝，避免悄悄改变前后 ROI。
    return box


def recognize_four_rotations(engine: RapidOCR, crop: np.ndarray, threshold: float) -> dict:
    """原比例 ROI 的四个直角方向；前后完全相同，不按棋字答案挑候选。"""
    attempts = []
    for k in range(4):
        view = np.ascontiguousarray(np.rot90(crop, k))
        result = engine(view, use_det=False, use_cls=False, use_rec=True)
        texts = [str(v) for v in (result.txts or [])]
        scores = [float(v) for v in (result.scores or [])]
        text = texts[0] if texts else ""
        score = scores[0] if scores else 0.0
        attempts.append({"rotation_ccw_deg": 90 * k, "input_size": [view.shape[1], view.shape[0]],
                         "raw_text": text, "score": score, "all_raw_texts": texts,
                         "all_scores": scores})
    selected = max(attempts, key=lambda attempt: attempt["score"])
    character, status = classify_text(selected["raw_text"], selected["score"], threshold)
    ink, evidence = ink_color(crop)
    piece_class = (f"{ink}_{TYPE_NAMES[character]}"
                   if character != "unknown" and ink != "unknown" else "unknown")
    return {"raw_text": selected["raw_text"], "score": selected["score"],
            "character": character, "status": status, "ink": ink,
            "piece_class": piece_class, "ink_evidence": evidence,
            "selected_rotation_ccw_deg": selected["rotation_ccw_deg"], "attempts": attempts}


def image_panel(canvas: Image.Image, image: np.ndarray | Image.Image,
                bounds: tuple[int, int, int, int]) -> None:
    picture = rgb(image) if isinstance(image, np.ndarray) else image.copy()
    x, y, width, height = bounds
    picture.thumbnail((width, height), Image.Resampling.LANCZOS)
    canvas.paste(picture, (x + (width - picture.width) // 2,
                           y + (height - picture.height) // 2))


def annotated_points(image: np.ndarray, points: np.ndarray, title: str,
                     source: bool, pieces: list[dict] | None = None) -> Image.Image:
    picture = rgb(image)
    draw = ImageDraw.Draw(picture)
    scale = max(1, picture.width / 1400)
    width = max(2, round(3 * scale))
    radius = round(8 * scale)
    font = get_font(round(23 * scale))
    positions = [tuple(float(v) for v in point) for point in points]
    draw.line([*positions, positions[0]], fill="#1fd2c4", width=width)
    for index, (x, y) in enumerate(positions, 1):
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill="#ffcb55", outline="#17324f", width=width)
        label = f"P{index} ({x:.0f},{y:.0f})"
        tx = min(max(8, x + 14 * scale), picture.width - 240 * scale)
        ty = max(6, y - 42 * scale)
        box = draw.textbbox((tx, ty), label, font=font)
        draw.rectangle(box, fill="white")
        draw.text((tx, ty), label, font=font, fill="#17324f")
    if pieces:
        for piece in pieces:
            box = piece["source_bbox"] if source else target_box(piece, image)
            draw.rectangle(box, outline="#f09851", width=max(1, width - 1))
    band = round(64 * scale)
    canvas = Image.new("RGB", (picture.width, picture.height + band), "white")
    canvas.paste(picture, (0, band))
    ImageDraw.Draw(canvas).text((round(16 * scale), round(13 * scale)), title,
                               font=font, fill="#17324f")
    return canvas


def make_comparison(image: np.ndarray, rectified: np.ndarray, source_points: np.ndarray,
                    target_points: np.ndarray, synthetic: bool) -> Image.Image:
    canvas = Image.new("RGB", (1800, 1010), "#eef3f8")
    draw = ImageDraw.Draw(canvas)
    draw.text((36, 22), "低机位象棋图 → Perspective 棋面二维校正", font=get_font(38), fill="#17324f")
    source_kind = "AI 生成输入 / 非真实相机实测" if synthetic else "自定义输入 / 来源见配置记录"
    draw.text((36, 83), source_kind, font=get_font(24), fill="#a15a23")
    for x, title in ((28, "01  原始斜视图"), (916, "02  棋面校正（非真实 3D 俯视）")):
        draw.rounded_rectangle((x, 136, x + 856, 880), radius=18, fill="white")
        draw.text((x + 22, 154), title, font=get_font(30), fill="#17324f")
    image_panel(canvas, image, (48, 210, 816, 650))
    image_panel(canvas, rectified, (936, 210, 816, 650))
    draw.text((36, 904), "四个棋面参考点（假定同平面）→ 演示目标矩形；单应矩阵 H 由四点计算。",
              font=get_font(25), fill="#17324f")
    draw.text((36, 949), "侧壁非共面会拉伸；生成图无物理标定。遮挡与丢失的像素无法恢复。",
              font=get_font(23), fill="#526579")
    return canvas


def prediction_text(record: dict) -> str:
    character = record["character"] if record["character"] != "unknown" else "未知"
    raw = record["raw_text"] or "空"
    return f"分类：{character}   OCR：{raw[:8]}   分数：{record['score']:.3f}"


def make_piece_comparison(records: list[dict], source_crops: list[np.ndarray],
                          target_crops: list[np.ndarray], threshold: float,
                          synthetic: bool) -> Image.Image:
    row_height = 270
    canvas = Image.new("RGB", (1500, 200 + row_height * len(records)), "#eef3f8")
    draw = ImageDraw.Draw(canvas)
    draw.text((30, 20), "棋面逐子对比：校正前 / 校正后使用相同 OCR 流程", font=get_font(31), fill="#17324f")
    draw.text((30, 73), f"原比例 ROI · 4 个直角方向 · 最高原始分数后精确归一 · 阈值 {threshold:.2f} · 未读取字符答案",
              font=get_font(21), fill="#526579")
    draw.text((95, 122), "原始 ROI", font=get_font(24), fill="#17324f")
    draw.text((555, 122), "Perspective 校正 ROI", font=get_font(24), fill="#17324f")
    draw.text((940, 122), "本地 OCR 实际输出", font=get_font(24), fill="#17324f")
    for index, (record, before, after) in enumerate(zip(records, source_crops, target_crops)):
        y = 170 + row_height * index
        draw.rounded_rectangle((20, y, 1480, y + row_height - 14), radius=12, fill="white")
        draw.text((35, y + 8), record["id"], font=get_font(21), fill="#526579")
        image_panel(canvas, before, (62, y + 26, 350, 206))
        image_panel(canvas, after, (510, y + 26, 350, 206))
        draw.text((910, y + 32), "校正前", font=get_font(24), fill="#526579")
        draw.text((910, y + 73), prediction_text(record["before"]), font=get_font(22), fill="#17324f")
        draw.text((910, y + 130), "校正后", font=get_font(24), fill="#116c56")
        draw.text((910, y + 171), prediction_text(record["after"]), font=get_font(22), fill="#17324f")
        draw.text((910, y + 213), f"字色：{record['after']['ink']}；分数不是正确概率", font=get_font(18), fill="#526579")
    suffix = "AI 生成场景演示；不报告真实比赛识别精度。" if synthetic else "自定义输入；需独立标注后才能评估识别精度。"
    draw.text((30, canvas.height - 29), f"展示等比缩放；{suffix}", font=get_font(18), fill="#526579")
    return canvas


def write_bgr(path: Path, image: np.ndarray) -> None:
    ok, encoded = cv2.imencode(path.suffix, image)
    if not ok:
        raise ValueError(f"无法编码图像：{path}")
    encoded.tofile(path)


def guard_outputs(destinations: list[Path], protected: set[Path], overwrite: bool) -> None:
    resolved = [path.resolve() for path in destinations]
    if len(set(resolved)) != len(resolved) or any(path in protected for path in resolved):
        raise ValueError("输出必须彼此不同，且不得覆盖输入图、配置、源码或模型。")
    if any(not path.is_relative_to(ROOT) for path in resolved):
        raise ValueError("所有输出须在当前项目目录内。")
    existing = [relative_path(path) for path in resolved if path.exists()]
    if existing and not overwrite:
        raise ValueError("输出已存在；请换输出路径，或确认重跑后使用 --overwrite：" + ", ".join(existing))
    if any(path.exists() and not path.is_file() for path in resolved):
        raise ValueError("输出目标不能是文件夹。")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--images-dir", type=Path, default=DEFAULT_IMAGES)
    parser.add_argument("--prefix", default=DEFAULT_PREFIX)
    parser.add_argument("--threshold", type=float, default=0.70)
    parser.add_argument("--overwrite", action="store_true", help="明确允许重跑覆盖本程序生成的结果；不覆盖输入")
    args = parser.parse_args()
    if not 0 <= args.threshold <= 1:
        parser.error("threshold 必须介于 0 和 1 之间。")
    if not args.prefix or Path(args.prefix).name != args.prefix or any(v in args.prefix for v in "\\/:*?\"<>|"):
        parser.error("prefix 只能是文件名前缀，不能含路径或非法字符。")
    config_path, model_path = project_path(args.config), project_path(args.model)
    output_path, images_dir = project_path(args.output), project_path(args.images_dir)
    config = load_config(config_path)
    image_path = project_path(config["image"])
    image = read_image(image_path)
    src = points_array(config["source_centers"], "source_centers")
    dst = points_array(config["target_centers"], "target_centers")
    output_size = positive_size(config["output_size"], "output_size")
    for name, points, width, height in (
            ("source_centers", src, image.shape[1], image.shape[0]),
            ("target_centers", dst, *output_size)):
        if np.any(points < 0) or np.any(points[:, 0] >= width) or np.any(points[:, 1] >= height):
            raise ValueError(f"{name} 有点坐标超出图像范围。")
    if sha256(model_path) != MODEL_SHA256:
        raise ValueError("OCR 模型 SHA-256 与 05_data/02_models/01_xiangqi_ocr/metadata.json 不符。")
    names = {"source_points": "points_01.png", "rectified": "rectified_01.png",
             "target_points": "points_02.png", "comparison": "comparison_01.png",
             "piece_comparison": "recognition_01.png"}
    outputs = {name: images_dir / (args.prefix + suffix) for name, suffix in names.items()}
    protected = {image_path, config_path, model_path, Path(__file__).resolve(),
                 ROOT / "02_programs/01_host/02_xiangqi/recognize_pieces.py"}
    guard_outputs([output_path, *outputs.values()], protected, args.overwrite)

    # 真正的 Perspective：仅根据共面对应点求 3×3 H，再重采样原图。
    homography = cv2.getPerspectiveTransform(src, dst)
    if not np.isfinite(homography).all() or abs(np.linalg.det(homography)) < 1e-12:
        raise ValueError("无法从该四点得到非退化单应矩阵。")
    rectified = cv2.warpPerspective(image, homography, output_size, flags=cv2.INTER_CUBIC,
                                    borderMode=cv2.BORDER_CONSTANT, borderValue=(242, 242, 242))
    mapped = cv2.perspectiveTransform(src.reshape(1, -1, 2), homography)[0]
    source_crops, target_crops = [], []
    for piece in config["pieces"]:
        source_crops.append(crop_region(image, piece["source_bbox"]))
        target_crops.append(crop_region(rectified, target_box(piece, rectified)))

    started = time.perf_counter()
    engine = RapidOCR(params={
        "Global.use_det": False, "Global.use_cls": False, "Global.log_level": "warning",
        "Global.model_root_dir": str(model_path.parent), "Rec.model_path": str(model_path),
        "EngineConfig.onnxruntime.intra_op_num_threads": 2,
        "EngineConfig.onnxruntime.inter_op_num_threads": 1,
    })
    records = []
    for piece, source_crop, target_crop in zip(config["pieces"], source_crops, target_crops):
        before = recognize_four_rotations(engine, source_crop, args.threshold)
        after = recognize_four_rotations(engine, target_crop, args.threshold)
        records.append({**piece, "target_bbox": target_box(piece, rectified),
                        "before": before, "after": after})
        print(f"{piece['id']}: before={before['raw_text']!r} ({before['score']:.5f}, {before['character']}); "
              f"after={after['raw_text']!r} ({after['score']:.5f}, {after['character']})", flush=True)
    elapsed = time.perf_counter() - started
    # 所有输入与目标检查、变换和识别完成后，才创建输出目录与文件。
    images_dir.mkdir(parents=True, exist_ok=True)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    write_bgr(outputs["rectified"], rectified)
    annotated_points(image, src, "原始斜视图：人工选取 4 个棋面参考点（假定同平面）", True,
                     config["pieces"]).save(outputs["source_points"])
    annotated_points(rectified, dst, "棋面二维校正：四参考点映射至演示目标矩形；橙框为 OCR ROI", False,
                     config["pieces"]).save(outputs["target_points"])
    make_comparison(image, rectified, src, dst, config["synthetic"]).save(outputs["comparison"])
    make_piece_comparison(records, source_crops, target_crops, args.threshold,
                          config["synthetic"]).save(outputs["piece_comparison"])
    report = {
        "schema_version": 1, "synthetic": config["synthetic"],
        "config_file": relative_path(config_path), "config_sha256": sha256(config_path),
        "source_image": relative_path(image_path), "source_image_sha256": sha256(image_path),
        "source_image_size": [image.shape[1], image.shape[0]],
        "model_file": relative_path(model_path), "model_sha256": sha256(model_path),
        "geometry_assumption": config["geometry_assumption"],
        "calibration_mode": "manual_four_reference_points_to_demonstration_rectangle_not_metric_calibration",
        "source_centers": src.tolist(), "target_centers": dst.tolist(),
        "output_size": list(output_size), "homography_source_to_target": homography.tolist(),
        "operation": "cv2.getPerspectiveTransform(src,dst) -> cv2.warpPerspective(image,H,output_size)",
        "interpolation": "INTER_CUBIC", "border_mode": "BORDER_CONSTANT", "border_bgr": [242, 242, 242],
        "fitted_control_point_rmse_pixels": float(np.sqrt(np.mean(np.sum((mapped - dst) ** 2, axis=1)))),
        "control_point_rmse_note": "四个拟合控制点的回代误差，仅检验映射计算，不是独立校正精度。",
        "roi_mode": "manual_source_bbox_and_configured_target_crops",
        "ground_truth_used_for_inference": False, "threshold": args.threshold,
        "ocr_policy": "same before/after pipeline: preserve ROI aspect ratio; rotations 0/90/180/270; "
                      "highest score over complete OCR alphabet, then exact piece-character mapping/rejection",
        "ocr_score_note": "通用 OCR 原始分数；未校准为正确概率，不表示真实比赛精度。",
        "limitations": ["同高棋面共面是场景假设，AI 图像并非严格相机成像。",
                        "目标矩形是人工演示布局，不是从生成提示词获得的实测尺寸标定。",
                        "比赛随机棋子布局不能直接提供这四个已知几何对应点。",
                        "棋子侧壁、桌面与棋面不同高，单一 H 不能同时将其全部校正。",
                        "无法恢复被遮挡内容、镜头中未采集的细节或生成图本身的错字。",
                        "未进行镜头去畸变、自动找棋子或真实比赛准确率评估。"],
        "elapsed_seconds_including_engine_load": elapsed,
        "versions": {p: importlib.metadata.version(p) for p in (
            "rapidocr", "onnxruntime", "opencv-python-headless", "numpy", "Pillow")},
        "visualizations": {name: relative_path(path) for name, path in outputs.items()},
        "predictions": records,
    }
    output_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8", newline="\n")
    print(f"结果：{output_path}\n前后对比：{outputs['comparison']}\nOCR 对比：{outputs['piece_comparison']}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, TypeError, ValueError, cv2.error) as error:
        raise SystemExit(f"错误：{error}") from error
