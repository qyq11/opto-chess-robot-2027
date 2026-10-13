"""Download small, public research references. No code or model installation.

Run: python download_research.py
Outputs are confined to ../02_研究参考. PDF signatures and SHA256 are recorded.
"""
from __future__ import annotations

import csv
import hashlib
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

DEST = Path(__file__).resolve().parent.parent / "02_研究参考"
ITEMS = [
    ("论文/Force_Push_2024.pdf", "https://arxiv.org/pdf/2401.17517", "移动机器人力反馈推送"),
    ("论文/Fine_Manipulation_by_Pushing_1992.pdf", "https://publications.ri.cmu.edu/storage/publications/pub_files/pub2/lynch_kevin_1992_1/lynch_kevin_1992_1.pdf", "稳定多接触推送力学；1996论文官网直链失效，保留1992原始研究"),
    ("论文/Planar_Pushing_Dataset_2016.pdf", "https://arxiv.org/pdf/1604.04038", "摩擦及推送变量实验"),
    ("论文/Visual_Pushing_and_Grasping_2018.pdf", "https://arxiv.org/pdf/1803.09956", "视觉强化学习推抓；原项目站PDF404，改作者arXiv稿"),
    ("论文/MobileNetV3_2019.pdf", "https://arxiv.org/pdf/1905.02244", "轻量视觉分类骨干"),
    ("开源说明/force_push_README.md", "https://raw.githubusercontent.com/learnsyslab/force_push/main/README.md", "参考复现说明；MIT"),
    ("开源说明/force_push_LICENSE.txt", "https://raw.githubusercontent.com/learnsyslab/force_push/main/LICENSE", "MIT许可证"),
    ("开源说明/Visual_Pushing_and_Grasping_README.md", "https://raw.githubusercontent.com/andyzeng/visual-pushing-grasping/master/README.md", "参考旧UR5环境；BSD-2-Clause"),
    ("开源说明/Visual_Pushing_and_Grasping_LICENSE.txt", "https://raw.githubusercontent.com/andyzeng/visual-pushing-grasping/master/LICENSE", "BSD-2-Clause许可证"),
    ("开源说明/Nav2_MPPI_README.md", "https://raw.githubusercontent.com/ros-navigation/navigation2/main/nav2_mppi_controller/README.md", "局部轨迹优化说明"),
    ("开源说明/Nav2_MPPI_package.xml", "https://raw.githubusercontent.com/ros-navigation/navigation2/main/nav2_mppi_controller/package.xml", "MPPI包许可证MIT"),
    ("开源说明/Nav2_repository_LICENSE.txt", "https://raw.githubusercontent.com/ros-navigation/navigation2/main/LICENSE", "仓库多许可证；逐包检查"),
    ("开源说明/Box2D_README.md", "https://raw.githubusercontent.com/erincatto/box2d/main/README.md", "2D碰撞引擎；MIT"),
    ("开源说明/Box2D_LICENSE.txt", "https://raw.githubusercontent.com/erincatto/box2d/main/LICENSE", "MIT许可证"),
    ("开源说明/Stable_Baselines3_README.md", "https://raw.githubusercontent.com/DLR-RM/stable-baselines3/master/README.md", "离线策略训练；MIT"),
    ("开源说明/Stable_Baselines3_LICENSE.txt", "https://raw.githubusercontent.com/DLR-RM/stable-baselines3/master/LICENSE", "MIT许可证"),
    ("开源说明/AprilTag_README.md", "https://raw.githubusercontent.com/AprilRobotics/apriltag/master/README.md", "仅自建训练场或获官方允许；BSD-2-Clause"),
    ("开源说明/AprilTag_LICENSE.md", "https://raw.githubusercontent.com/AprilRobotics/apriltag/master/LICENSE.md", "BSD-2-Clause许可证"),
    ("开源说明/OpenCV_LICENSE.txt", "https://raw.githubusercontent.com/opencv/opencv/4.x/LICENSE", "OpenCV4.5+ Apache-2.0及历史版权"),
    ("开源说明/TorchVision_LICENSE.txt", "https://raw.githubusercontent.com/pytorch/vision/main/LICENSE", "TorchVision BSD-3-Clause；第三方依赖另查"),
]


def download(item):
    relative, url, purpose = item
    output = DEST / relative
    output.parent.mkdir(parents=True, exist_ok=True)
    result = dict(file=relative, url=url, purpose=purpose, date="2026-10-13", status="", bytes="", sha256="")
    if output.exists():
        data = output.read_bytes()
        if data and (not relative.endswith(".pdf") or data.startswith(b"%PDF-")):
            result.update(status="downloaded", bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), resolved_url=url)
            return result
    for attempt in range(2):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 ResearchReferenceDownloader/1.0"})
            with urllib.request.urlopen(request, timeout=35) as response:
                data = response.read(25 * 1024 * 1024)
                result["resolved_url"] = response.url
            if relative.endswith(".pdf") and not data.startswith(b"%PDF-"):
                raise ValueError("response is not a PDF")
            if not relative.endswith(".pdf") and (b"<!DOCTYPE html" in data[:200] or b"<html" in data[:200]):
                raise ValueError("unexpected HTML instead of source file")
            output.write_bytes(data)
            result.update(status="downloaded", bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
            print(f"OK {relative} ({len(data)} bytes)", flush=True)
            return result
        except Exception as exc:
            result["status"] = f"FAILED: {exc}"
            if attempt == 0:
                time.sleep(1)
    print(f"FAIL {relative}: {result['status']}", flush=True)
    return result


def main():
    DEST.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=4) as pool:
        rows = list(pool.map(download, ITEMS))
    fields = ["file", "url", "resolved_url", "purpose", "date", "status", "bytes", "sha256"]
    with (DEST / "研究下载清单.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    failed = sum(row["status"] != "downloaded" for row in rows)
    print(f"Done: {len(rows) - failed}/{len(rows)} files, {failed} failures.")


if __name__ == "__main__":
    main()
