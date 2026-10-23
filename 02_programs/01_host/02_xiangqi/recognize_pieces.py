"""人工圈定棋面 -> 本地 OCR -> 字形归一 -> 棋子类别。

默认读取可溯源的两张真实照片和不含标签的 ROI 文件。
不读 ground_truth.json，不利用棋盘布局或已知答案推断棋子。
模型为通用文字识别器；输出分数不是经校准的正确概率。
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
from PIL import Image, ImageDraw, ImageFont
from rapidocr import RapidOCR


ROOT = Path(__file__).resolve().parents[3]
DEFAULT_REGIONS = ROOT / "05_data/01_raw/02_xiangqi/regions.json"
DEFAULT_MODEL = ROOT / "05_data/02_models/01_xiangqi_ocr/PP-OCRv6_rec_small.onnx"
DEFAULT_OUTPUT = ROOT / "05_data/03_results/03_xiangqi/predictions.json"
DEFAULT_IMAGES = ROOT / "05_data/03_results/03_xiangqi"
MODEL_SHA256 = "6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884"

# 单字精确匹配；不把相似的非棋子汉字强行纠正为已知类别。
CHARACTERS = {
    "将": "将", "將": "将", "帅": "帅", "帥": "帅",
    "士": "士", "仕": "仕", "象": "象", "相": "相",
    "车": "车", "車": "车", "俥": "车",
    "马": "马", "馬": "马", "傌": "马",
    "炮": "炮", "砲": "炮",
    "卒": "卒", "兵": "兵",
}
TYPE_NAMES = {"将": "将", "帅": "帅", "士": "士", "仕": "仕", "象": "象", "相": "相",
              "车": "车", "马": "马", "炮": "炮", "卒": "卒", "兵": "兵"}


def read_image(path: Path) -> np.ndarray:
    image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"无法解码图像：{path}")
    return image


def relative_path(path: Path) -> str:
    full = path.resolve()
    return full.relative_to(ROOT).as_posix() if full.is_relative_to(ROOT) else str(full)


def crop_region(image: np.ndarray, box: list[int]) -> np.ndarray:
    if len(box) != 4 or any(not isinstance(x, int) for x in box):
        raise ValueError("bbox 须为四个整数 [x1,y1,x2,y2]。")
    x1, y1, x2, y2 = box
    if not (0 <= x1 < x2 <= image.shape[1] and 0 <= y1 < y2 <= image.shape[0]):
        raise ValueError(f"ROI 越界或退化：{box}")
    return image[y1:y2, x1:x2].copy()


def variants(crop: np.ndarray):
    """固定的 24 种候选：3个中心裁剪 × 2种缩放 × 4个直角旋转。

    这只是预处理搜索，不等价于相机标定或透视矫正；角度为逆时针。
    所有样本使用相同规则，原始图片不会被改变。
    """
    height, width = crop.shape[:2]
    for fraction in (1.0, 0.8, 0.65):
        dx = round(width * (1 - fraction) / 2)
        dy = round(height * (1 - fraction) / 2)
        center = crop[dy:height - dy, dx:width - dx]
        for square in (False, True):
            resized = cv2.resize(center, (160, 160)) if square else center
            for k in range(4):
                yield {"center_fraction": fraction, "resize_to_square": square,
                       "rotation_ccw_deg": 90 * k}, np.ascontiguousarray(np.rot90(resized, k))


def classify_text(text: str, score: float, threshold: float) -> tuple[str, str]:
    text = text.strip()
    if score < threshold:
        return "unknown", "low_score"
    if text not in CHARACTERS:
        return "unknown", "not_a_single_piece_character"
    return CHARACTERS[text], "accepted"


def ink_color(crop: np.ndarray) -> tuple[str, dict]:
    """仅区分红色字、深色字和未知；深色字按传统黑方命名，不推断所有权。"""
    h, w = crop.shape[:2]
    center = crop[h // 5:h - h // 5, w // 5:w - w // 5]
    hsv = cv2.cvtColor(center, cv2.COLOR_BGR2HSV)
    hue, saturation, value = cv2.split(hsv)
    blue, green, red_channel = cv2.split(center.astype(np.float32))
    red = (((hue <= 12) | (hue >= 170)) & (saturation >= 85) & (value >= 35)
           & (red_channel > 1.45 * green) & (red_channel > 1.45 * blue))
    dark = value < max(30, 0.60 * float(np.median(value)))
    evidence = {"red_pixel_ratio": float(red.mean()), "dark_pixel_ratio": float(dark.mean())}
    if evidence["dark_pixel_ratio"] >= 0.08 and evidence["dark_pixel_ratio"] > evidence["red_pixel_ratio"]:
        return "black", evidence
    if evidence["red_pixel_ratio"] >= 0.025:
        return "red", evidence
    return "unknown", evidence


def recognize_crop(engine: RapidOCR, crop: np.ndarray, threshold: float):
    attempts = []
    views = []
    for parameters, view in variants(crop):
        result = engine(view, use_det=False, use_cls=False, use_rec=True)
        text = result.txts[0] if result.txts else ""
        score = float(result.scores[0]) if result.scores else 0.0
        attempts.append({**parameters, "raw_text": text, "score": score})
        views.append(view)
    # 先在完整 OCR 字表中按分数选候选，再判断是否棋字；不优先挑已知答案。
    best_index = max(range(len(attempts)), key=lambda i: attempts[i]["score"])
    best = attempts[best_index]
    character, status = classify_text(best["raw_text"], best["score"], threshold)
    ink, evidence = ink_color(crop)
    piece_class = f"{ink}_{TYPE_NAMES[character]}" if character != "unknown" and ink != "unknown" else "unknown"
    record = {"character": character, "ink": ink, "piece_class": piece_class,
              "raw_text": best["raw_text"], "score": best["score"], "status": status,
              "selected_preprocessing": {k: best[k] for k in (
                  "center_fraction", "resize_to_square", "rotation_ccw_deg")},
              "ink_evidence": evidence, "attempts": attempts, "contest_points": None}
    return record, views[best_index]


def get_font(size: int) -> ImageFont.FreeTypeFont:
    for path in ("C:/Windows/Fonts/msyh.ttc",
                 "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                 "/System/Library/Fonts/PingFang.ttc"):
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    raise ValueError("未找到中文字体，请安装 Noto Sans CJK 后运行。")


def rgb(image: np.ndarray) -> Image.Image:
    return Image.fromarray(cv2.cvtColor(image, cv2.COLOR_BGR2RGB))


def make_contact_sheet(
    records: list[dict], crops: list[np.ndarray], selected: list[np.ndarray], credits: dict[str, str]
) -> Image.Image:
    columns = 4
    rows = (len(records) + columns - 1) // columns
    canvas = Image.new("RGB", (1600, 164 + rows * 330), "#f2f5f9")
    draw = ImageDraw.Draw(canvas)
    draw.text((30, 22), "象棋字样识别与分类：本地 OCR 实际输出", font=get_font(32), fill="#17324f")
    draw.text((30, 74), "人工选择棋面 ROI；24 种固定预处理；低分或非棋字输出“未知”", font=get_font(21), fill="#526579")
    for i, (record, crop, chosen) in enumerate(zip(records, crops, selected)):
        x, y = 20 + i % columns * 395, 120 + i // columns * 330
        draw.rounded_rectangle((x, y, x + 375, y + 314), radius=12, fill="white")
        accepted = record["character"] != "unknown"
        label = record["character"] if accepted else "未知"
        draw.text((x + 14, y + 10), f"{record['id']} → {label}", font=get_font(25), fill="#116c56" if accepted else "#a4541d")
        for offset, picture, caption in [(14, crop, "原图裁剪"), (199, chosen, "实际 OCR 输入")]:
            thumbnail = rgb(picture)
            thumbnail.thumbnail((160, 160), Image.Resampling.LANCZOS)
            canvas.paste(thumbnail, (x + offset + (160 - thumbnail.width) // 2, y + 54 + (160 - thumbnail.height) // 2))
            draw.text((x + offset, y + 218), caption, font=get_font(17), fill="#526579")
        raw = record["raw_text"] or "空"
        draw.text((x + 14, y + 247), f"OCR：{raw}   分数：{record['score']:.3f}", font=get_font(19), fill="#17324f")
        draw.text((x + 14, y + 278), f"字色：{record['ink']}   旋转：{record['selected_preprocessing']['rotation_ccw_deg']}°", font=get_font(16), fill="#526579")
    credit = " / ".join(dict.fromkeys(credits.values()))
    draw.text((24, canvas.height - 30), f"原图：{credit}；本图已裁剪、缩放、旋转和标注。", font=get_font(17), fill="#526579")
    return canvas


def annotate_photos(
    records: list[dict], images: dict[str, np.ndarray], output_dir: Path, credits: dict[str, str]
) -> list[str]:
    paths = []
    for number, (name, image) in enumerate(images.items(), 1):
        picture = rgb(image)
        factor = min(1, 1800 / picture.width)
        picture = picture.resize((round(picture.width * factor), round(picture.height * factor)))
        canvas = Image.new("RGB", (picture.width, picture.height + 70), "white")
        canvas.paste(picture, (0, 0))
        draw = ImageDraw.Draw(canvas)
        for record in records:
            if record["image"] != name:
                continue
            box = [round(v * factor) for v in record["bbox"]]
            color = "#127557" if record["character"] != "unknown" else "#b95d00"
            draw.rectangle(box, outline=color, width=4)
            text = f"{record['id']} {record['character']} {record['score']:.2f}"
            position = (box[0], max(0, box[1] - 30))
            background = draw.textbbox(position, text, font=get_font(22))
            draw.rectangle(background, fill="white")
            draw.text(position, text, font=get_font(22), fill=color)
        draw.text((15, picture.height + 12), f"Photo: {credits[name]} | ROI/OCR overlay; resized", font=get_font(21), fill="#17324f")
        path = output_dir / f"20261023_xiangqi_annotated_{number:02d}.png"
        canvas.save(path)
        paths.append(relative_path(path))
    return paths


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regions", type=Path, default=DEFAULT_REGIONS)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--images-dir", type=Path, default=DEFAULT_IMAGES)
    parser.add_argument("--threshold", type=float, default=0.70)
    args = parser.parse_args()
    if not 0 <= args.threshold <= 1:
        parser.error("threshold 必须介于0和1之间。")
    if hashlib.sha256(args.model.read_bytes()).hexdigest() != MODEL_SHA256:
        raise ValueError("OCR 模型哈希不匹配，详见 05_data/02_models/01_xiangqi_ocr/metadata.json。")
    regions = json.loads(args.regions.read_text(encoding="utf-8"))["regions"]
    if not regions or len({r["id"] for r in regions}) != len(regions):
        raise ValueError("ROI 清单不能为空，且 id 不可重复。")
    # 不接受标签字段，避免把评估答案混进推理输入。
    if any(set(r) != {"id", "image", "bbox"} for r in regions):
        raise ValueError("ROI 只允许 id、image、bbox 三个字段；人工答案请放独立文件。")
    images = {name: read_image(ROOT / name) for name in dict.fromkeys(r["image"] for r in regions)}
    protected = {args.regions.resolve(), args.model.resolve(), *((ROOT / p).resolve() for p in images)}
    contact_path = args.images_dir / "20261023_xiangqi_recognition_01.png"
    destinations = [args.output, contact_path, *[
        args.images_dir / f"20261023_xiangqi_annotated_{number:02d}.png"
        for number in range(1, len(images) + 1)
    ]]
    resolved = [path.resolve() for path in destinations]
    if any(path in protected for path in resolved) or len(set(resolved)) != len(resolved):
        raise ValueError("全部输出路径必须彼此不同，且不能覆盖原图、ROI 或模型。")
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in images}
    source_file = ROOT / "05_data/01_raw/02_xiangqi/sources.json"
    sources = json.loads(source_file.read_text(encoding="utf-8"))["images"] if source_file.exists() else []
    by_hash = {source["sha256"]: f"{source['author']} · {source['license']}" for source in sources}
    credits = {name: by_hash.get(sha, "自定义输入，来源信息未登记") for name, sha in hashes.items()}
    engine = RapidOCR(params={
        "Global.use_det": False, "Global.use_cls": False, "Global.log_level": "warning",
        "Global.model_root_dir": str(args.model.parent), "Rec.model_path": str(args.model),
        "EngineConfig.onnxruntime.intra_op_num_threads": 2,
        "EngineConfig.onnxruntime.inter_op_num_threads": 1,
    })
    records, crops, selected = [], [], []
    started = time.perf_counter()
    for region in regions:
        crop = crop_region(images[region["image"]], region["bbox"])
        prediction, chosen = recognize_crop(engine, crop, args.threshold)
        records.append({**region, **prediction})
        crops.append(crop)
        selected.append(chosen)
        print(f"{region['id']}: OCR={prediction['raw_text']!r}, score={prediction['score']:.5f}, "
              f"class={prediction['piece_class']}, status={prediction['status']}", flush=True)
    elapsed = time.perf_counter() - started
    args.images_dir.mkdir(parents=True, exist_ok=True)
    make_contact_sheet(records, crops, selected, credits).save(contact_path)
    annotated = annotate_photos(records, images, args.images_dir, credits)
    report = {
        "model_sha256": MODEL_SHA256, "regions_file": relative_path(args.regions),
        "threshold": args.threshold, "roi_mode": "manual_bbox", "ground_truth_used_for_inference": False,
        "selection": "highest raw OCR score across all 24 variants, then exact character mapping/rejection",
        "elapsed_seconds_including_first_model_load": elapsed,
        "versions": {p: importlib.metadata.version(p) for p in ("rapidocr", "onnxruntime", "opencv-python-headless", "numpy", "Pillow")},
        "input_sha256": hashes, "photo_credits": credits,
        "visualizations": {"contact_sheet": relative_path(contact_path), "annotated": annotated},
        "predictions": records,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8", newline="\n")
    print(f"结果：{args.output}\n对比图：{contact_path}\n耗时：{elapsed:.3f} 秒（含首次模型加载）")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, cv2.error) as error:
        raise SystemExit(f"错误：{error}") from error
