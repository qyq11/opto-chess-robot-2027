"""归档 SLAM 比较使用的一手 README/许可/参数快照。仅用标准库。"""
from pathlib import Path
from urllib.request import Request, urlopen
from datetime import datetime
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "02_研究参考" / "SLAM_开源快照"
OUT.mkdir(parents=True, exist_ok=True)
ITEMS = [
    ("ORB_SLAM3_README.md", "https://raw.githubusercontent.com/UZ-SLAMLab/ORB_SLAM3/master/README.md"),
    ("ORB_SLAM3_LICENSE.txt", "https://raw.githubusercontent.com/UZ-SLAMLab/ORB_SLAM3/master/LICENSE"),
    ("RTABMap_README.md", "https://raw.githubusercontent.com/introlab/rtabmap/master/README.md"),
    ("RTABMap_LICENSE.txt", "https://raw.githubusercontent.com/introlab/rtabmap/master/LICENSE"),
    ("RoboMaster_pb2025_README.md", "https://raw.githubusercontent.com/SMBU-PolarBear-Robotics-Team/pb2025_sentry_nav/main/README.md"),
    ("RoboMaster_pb2025_LICENSE.txt", "https://raw.githubusercontent.com/SMBU-PolarBear-Robotics-Team/pb2025_sentry_nav/main/LICENSE"),
    ("RoboMaster_pb2025_实车参数.yaml", "https://raw.githubusercontent.com/SMBU-PolarBear-Robotics-Team/pb2025_sentry_nav/main/pb2025_nav_bringup/config/reality/nav2_params.yaml"),
]
records = []
for name, url in ITEMS:
    row = dict(file=name, source_url=url,
               retrieved_at=datetime.now().astimezone().isoformat())
    try:
        with urlopen(Request(url, headers={"User-Agent": "Mozilla/5.0"}), timeout=30) as r:
            data = r.read()
        data.decode("utf-8")
        if b"<!DOCTYPE html" in data[:100] or not data:
            raise ValueError("响应不是预期的非空文本")
        (OUT / name).write_bytes(data)
        row.update(status="downloaded", bytes=len(data),
                   sha256=hashlib.sha256(data).hexdigest())
    except Exception as e:
        row.update(status="failed", error=str(e))
    records.append(row)
    print(name, row["status"])
(OUT / "下载记录.json").write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding="utf-8")
if any(r["status"] != "downloaded" for r in records):
    raise SystemExit(1)
