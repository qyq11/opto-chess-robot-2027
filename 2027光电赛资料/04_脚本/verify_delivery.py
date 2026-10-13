"""验证交付 PDF 可读性、主要 Markdown 本地链接和脚本归档。

此为资料完整性核验，不代表实车方案已测试。依赖 pypdf。
"""
from pathlib import Path
import hashlib
import json
import re
from pypdf import PdfReader

ROOT = Path(__file__).resolve().parents[1]
records, errors = [], []
for p in sorted(ROOT.rglob("*.pdf")):
    data = p.read_bytes()
    row = {"file": str(p.relative_to(ROOT)), "bytes": len(data),
           "sha256": hashlib.sha256(data).hexdigest()}
    try:
        if not data.startswith(b"%PDF-"):
            raise ValueError("PDF 文件签名不匹配")
        r = PdfReader(p)
        row["pages"] = len(r.pages)
        row["text_characters"] = sum(len(pg.extract_text() or "") for pg in r.pages)
        row["status"] = "readable"
    except Exception as e:
        row.update(status="error", error=str(e))
        errors.append(str(p))
    records.append(row)

for name in ["方案_智能车光电象棋对抗.md", "资料清单.md", "02_研究参考/SLAM_补充比较.md"]:
    p = ROOT / name
    text = p.read_text(encoding="utf-8")
    for target in re.findall(r"!?\[[^\]]*\]\(([^)]+)\)", text):
        if target.startswith(("https://", "http://", "#")):
            continue
        if not (p.parent / target).exists():
            errors.append(f"{name} 缺失本地链接：{target}")
    if re.search(r"placeholder|TODO|待补链接", text, re.I):
        errors.append(f"{name} 含占位符")

for p in ROOT.parent.rglob("*.py"):
    if p.parent != ROOT / "04_脚本":
        errors.append(f"Python 文件未归档：{p}")

report = {"checked_at": "2026-10-13", "pdf_count": len(records),
          "pdfs": records, "errors": errors,
          "note": "官网操作手册空白为已确认的源文件异常；实车尚未测试。"}
(ROOT / "05_核验记录" / "交付核验.json").write_text(
    json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print("PDF 文件数：", len(records), "，核验错误数：", len(errors))
for e in errors:
    print(e)
if errors:
    raise SystemExit(1)
