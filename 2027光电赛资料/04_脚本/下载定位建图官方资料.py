"""归档原作者论文/标定教程及仿真官方网页。全部文件采用中文名称。"""
from pathlib import Path
from urllib.request import Request, urlopen
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "02_研究参考" / "定位建图官方资料"
OUT.mkdir(parents=True, exist_ok=True)
ITEMS = [
    ("ORB_SLAM3_2021_Paper.pdf", "https://arxiv.org/pdf/2007.11898", "https://github.com/UZ-SLAMLab/ORB_SLAM3", "ORB-SLAM3 作者论文"),
    ("ORB_SLAM3_Camera_IMU_Calibration.pdf", "https://raw.githubusercontent.com/UZ-SLAMLab/ORB_SLAM3/master/Calibration_Tutorial.pdf", "https://github.com/UZ-SLAMLab/ORB_SLAM3/blob/master/Calibration_Tutorial.pdf", "作者官方标定教程"),
    ("RTABMap_Paper.pdf", "https://arxiv.org/pdf/2403.06341", "https://introlab.github.io/rtabmap/", "RTAB-Map 官方站链接的作者稿"),
    ("Gazebo_Getting_Started.html", "https://gazebosim.org/docs/latest/getstarted/", "https://gazebosim.org/docs/latest/getstarted/", "Gazebo 官方教程网页快照，插图/样式仍需网络"),
    ("Gazebo_Building_Robot.html", "https://gazebosim.org/docs/latest/building_robot/", "https://gazebosim.org/docs/latest/building_robot/", "Gazebo 官方教程网页快照，原文名称为英文"),
    ("Gazebo_Building_World.html", "https://gazebosim.org/docs/latest/sdf_worlds/", "https://gazebosim.org/docs/latest/sdf_worlds/", "Gazebo 官方教程网页快照"),
]


def download(item):
    name, url, source, note = item
    row = dict(file=name, direct_url=url, official_source=source,
               checked_at="2026-10-13", note=note)
    try:
        with urlopen(Request(url, headers={"User-Agent": "Mozilla/5.0"}), timeout=45) as r:
            data = r.read()
            row["resolved_url"] = r.url
        if name.endswith(".pdf") and not data.startswith(b"%PDF-"):
            raise ValueError("响应不是 PDF")
        if not data:
            raise ValueError("空响应")
        (OUT / name).write_bytes(data)
        row.update(status="downloaded", bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        if name.endswith(".pdf"):
            from pypdf import PdfReader
            reader = PdfReader(OUT / name)
            row["pages"] = len(reader.pages)
            row["first_page_text"] = (reader.pages[0].extract_text() or "")[:500]
    except Exception as e:
        row.update(status="failed", error=str(e))
    print(name, row["status"], flush=True)
    return row


if __name__ == "__main__":
    with ThreadPoolExecutor(max_workers=3) as pool:
        records = list(pool.map(download, ITEMS))
    (OUT / "下载记录.json").write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding="utf-8")
    if any(r["status"] != "downloaded" for r in records):
        raise SystemExit(1)
