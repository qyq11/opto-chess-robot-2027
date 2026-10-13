"""归档官方硬件资料，记录链接、字节数及 SHA256；仅用 Python 标准库。"""
from pathlib import Path
from urllib.request import Request, urlopen
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "02_研究参考" / "硬件官方文档"
OUT.mkdir(parents=True, exist_ok=True)
ITEMS = [
    ("Raspberry_Pi_5_Product_Brief.pdf", "https://pip.raspberrypi.com/documents/RP-008348-DS-raspberry-pi-5-product-brief.pdf", "https://www.raspberrypi.com/products/raspberry-pi-5/"),
    ("Compute_Module_5_Datasheet.pdf", "https://datasheets.raspberrypi.com/cm5/cm5-datasheet.pdf", "https://www.raspberrypi.com/products/compute-module-5/"),
    ("Global_Shutter_Camera_Product_Brief.pdf", "https://datasheets.raspberrypi.com/gs-camera/gs-camera-product-brief.pdf", "https://www.raspberrypi.com/products/raspberry-pi-global-shutter-camera/"),
]
records = []
for name, url, source in ITEMS:
    row = dict(file=name, url=url, source_page=source, retrieved="2026-10-13")
    try:
        req = Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urlopen(req, timeout=45) as response:
            data = response.read()
        if not data.startswith(b"%PDF-"):
            raise ValueError("响应不是 PDF")
        (OUT / name).write_bytes(data)
        row.update(status="downloaded", bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    except Exception as error:
        row.update(status="failed", error=str(error))
    records.append(row)
    print(name, row["status"])
(OUT / "下载记录.json").write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding="utf-8")
