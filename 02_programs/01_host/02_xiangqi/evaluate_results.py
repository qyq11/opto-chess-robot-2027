"""在推理结束后，对照独立人工标注统计结果；不修改任何预测。"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def evaluate(prediction_file: Path, truth_file: Path) -> dict:
    predicted = json.loads(prediction_file.read_text(encoding="utf-8"))
    truths = json.loads(truth_file.read_text(encoding="utf-8"))
    labels = {row["id"]: row for row in truths["labels"]}
    rows = predicted["predictions"]
    ids = [row["id"] for row in rows]
    if len(set(ids)) != len(ids) or set(ids) != set(labels) or len(labels) != len(truths["labels"]):
        raise ValueError("预测与人工标注必须逐个对应，ID不可重复。")
    details = []
    for row in rows:
        truth = labels[row["id"]]
        details.append({
            "id": row["id"], "expected_raw_character": truth["raw_character"],
            "expected_character": truth["character"], "expected_ink": truth["ink"],
            "raw_ocr": row["raw_text"], "predicted_character": row["character"],
            "predicted_ink": row["ink"], "score": row["score"],
            "character_correct": row["character"] == truth["character"],
            "full_class_correct": row["character"] == truth["character"] and row["ink"] == truth["ink"],
            "rejected": row["character"] == "unknown",
        })
    rejected = sum(row["rejected"] for row in details)
    correct = sum(row["full_class_correct"] for row in details)
    target = [row for row in details if row["expected_character"] in ("将", "帅")]
    return {
        "scope": "两张公开照片上的12个人工ROI，包含开发时观察/调参，不是独立测试集或比赛准确率",
        "prediction_file_sha256": hashlib.sha256(prediction_file.read_bytes()).hexdigest(),
        "ground_truth_file_sha256": hashlib.sha256(truth_file.read_bytes()).hexdigest(),
        "total": len(details), "accepted": len(details) - rejected, "unknown": rejected,
        "character_correct": sum(row["character_correct"] for row in details),
        "full_class_correct": correct,
        "accepted_but_wrong_full_class": len(details) - rejected - correct,
        "general_marshall_cases": len(target),
        "general_marshall_correct": sum(row["full_class_correct"] for row in target),
        "details": details,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--predictions", type=Path, default=ROOT / "05_data/03_results/03_xiangqi/predictions.json")
    parser.add_argument("--truth", type=Path, default=ROOT / "05_data/01_raw/02_xiangqi/ground_truth.json")
    parser.add_argument("--output", type=Path, default=ROOT / "05_data/03_results/03_xiangqi/evaluation.json")
    args = parser.parse_args()
    if args.output.resolve() in {args.predictions.resolve(), args.truth.resolve()}:
        parser.error("评估输出不可覆盖预测或人工标注。")
    report = evaluate(args.predictions, args.truth)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8", newline="\n")
    print(f"总数={report['total']}，字样与字色均正确={report['full_class_correct']}，"
          f"未知={report['unknown']}，接受但错类={report['accepted_but_wrong_full_class']}")
    print(f"将/帅样本={report['general_marshall_cases']}，正确={report['general_marshall_correct']}")
    for row in report["details"]:
        print(f"{row['id']}: 真值={row['expected_ink']}_{row['expected_character']}，"
              f"OCR={row['raw_ocr']!r}，输出={row['predicted_ink']}_{row['predicted_character']}，"
              f"正确={row['full_class_correct']}")


if __name__ == "__main__":
    main()
