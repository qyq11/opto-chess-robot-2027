"""按资料原语言和简短主题命名；更新本地引用，保持下载文件字节不变。"""
from pathlib import Path
from urllib.parse import quote
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "2027光电赛资料"
MAPPING = {
    "01_接触力反馈稳健推送_机器人自动化快报_2024.pdf": "Force_Push_2024.pdf",
    "02_基于推送的精细操作_机器人自动化会议_1992.pdf": "Fine_Manipulation_by_Pushing_1992.pdf",
    "03_百万级平面推送实验与数据集_智能机器人会议_2016.pdf": "Planar_Pushing_Dataset_2016.pdf",
    "04_视觉推送与抓取协同学习_智能机器人会议_2018.pdf": "Visual_Pushing_and_Grasping_2018.pdf",
    "05_轻量移动视觉网络第三代设计_国际计算机视觉会议_2019.pdf": "MobileNetV3_2019.pdf",
    "树莓派五代_产品简介.pdf": "Raspberry_Pi_5_Product_Brief.pdf",
    "树莓派计算模块五代_数据手册.pdf": "Compute_Module_5_Datasheet.pdf",
    "全局快门相机_产品简介.pdf": "Global_Shutter_Camera_Product_Brief.pdf",
    "视觉惯性多地图定位建图_原作者论文.pdf": "ORB_SLAM3_2021_Paper.pdf",
    "视觉惯性定位建图_相机与惯性测量单元标定教程.pdf": "ORB_SLAM3_Camera_IMU_Calibration.pdf",
    "长期大规模雷达与视觉定位建图_原作者论文.pdf": "RTABMap_Paper.pdf",
    "2026-07-21_第十五届赛题和优秀应征赛题奖通知.pdf": "2026-07-21_第十五届赛题通知.pdf",
    "2026-04-27_第十五届赛题创意征集通知.pdf": "2026-04-27_第十五届赛题征集.pdf",
    "2026-01-04_第十四十五届承办单位公布通知.pdf": "2026-01-04_第十四十五届承办通知.pdf",
    "2025-10-20_第十四十五届承办单位征集通知.pdf": "2025-10-20_第十四十五届承办征集.pdf",
    "官网平台操作手册_当前链接_2026-10-13下载.pdf": "官网操作手册_2026-10-13.pdf",
    "用户原始下载_第十五届赛题附件_3页.pdf": "第十五届赛题附件_原始版.pdf",
    "2026-07-21_第十五届赛题和优秀应征赛题奖通知.txt": "第十五届赛题_提取文本.txt",
    "用户PDF_第1页.png": "赛题要求_首页预览.png",
    "用户PDF_提取文本.txt": "赛题要求_提取文本.txt",
    "PDF图_Im0.jpg": "官方赛场示意图.jpg",
    "VPG_README.md": "Visual_Pushing_and_Grasping_README.md",
    "VPG_LICENSE.txt": "Visual_Pushing_and_Grasping_LICENSE.txt",
    "SB3_README.md": "Stable_Baselines3_README.md",
    "SB3_LICENSE.txt": "Stable_Baselines3_LICENSE.txt",
    "manifest.json": "官方下载记录.json",
    "三维仿真_官方入门教程.html": "Gazebo_Getting_Started.html",
    "三维仿真_双轮小车建模教程.html": "Gazebo_Building_Robot.html",
    "三维仿真_场景搭建教程.html": "Gazebo_Building_World.html",
    "中文重命名记录.json": "首次重命名记录.json",
    "官网当前参赛指南_API快照_2026-10-13.json": "参赛指南_2026-10-13.json",
    "官网当前赛事_API快照_2026-10-13.json": "当前赛事_2026-10-13.json",
    "官网导航配置_API快照_2026-10-13.json": "网站配置_2026-10-13.json",
    "官网栏目201_API快照_2026-10-13.json": "章程细则_2026-10-13.json",
    "官网栏目202_API快照_2026-10-13.json": "文件通知_2026-10-13.json",
    "官网栏目203_API快照_2026-10-13.json": "赛事动态_2026-10-13.json",
    "官网栏目204_API快照_2026-10-13.json": "获奖公示_2026-10-13.json",
    "官网栏目206_API快照_2026-10-13.json": "历届作品_2026-10-13.json",
    "官网栏目208_API快照_2026-10-13.json": "竞赛通讯_2026-10-13.json",
    "官网赛事动态_API快照_2026-10-13.json": "赛事动态_完整接口.json",
    "2026-07-13_天津大学新闻网_确认2027承办.html": "天津大学_2027承办消息.html",
}
for news_id, label in [(131060, "赛题通知"), (123366, "赛题征集"),
                       (122769, "承办通知"), (122768, "承办征集"), (122647, "章程通知")]:
    for old, new in [("原始API.json", "原始接口.json"), ("原始网页.html", "官网网页.html"),
                     ("可读内容快照.html", "阅读版.html")]:
        MAPPING[f"官网通知_{news_id}_{old}"] = f"{label}_{new}"

plan = []
for src in PACKAGE.rglob("*"):
    if src.is_file() and (src.name in MAPPING or src.name.endswith(".pdf.png")):
        name = MAPPING.get(src.name)
        if name is None:
            name = MAPPING.get(src.name[:-4], src.name[:-4]) + ".png"
        src, dst = src.resolve(), src.with_name(name).resolve()
        if PACKAGE.resolve() not in src.parents or PACKAGE.resolve() not in dst.parents:
            raise RuntimeError("目标越出资料目录")
        if dst.exists():
            raise FileExistsError(str(dst))
        plan.append((src, dst, hashlib.sha256(src.read_bytes()).hexdigest()))
for src, dst, digest in plan:
    src.rename(dst)
    if hashlib.sha256(dst.read_bytes()).hexdigest() != digest:
        raise RuntimeError("重命名后内容发生变化")

updated = []
for p in PACKAGE.rglob("*"):
    if not p.is_file() or p.resolve() == Path(__file__).resolve():
        continue
    if p.name in ("首次重命名记录.json", "资料重命名记录.json"):
        continue
    if p.suffix.lower() not in (".md", ".py", ".json", ".csv", ".txt", ".html"):
        continue
    data = p.read_bytes()
    try:
        content = data.decode("utf-8")
    except UnicodeDecodeError:
        continue
    new = content
    for old, name in MAPPING.items():
        new = new.replace(old, name).replace(quote(old), quote(name))
    if new != content:
        p.write_bytes(new.encode("utf-8"))
        updated.append(str(p.relative_to(PACKAGE)))

if plan:
    report = {"date": "2026-10-13", "naming_rule": "中文资料用中文；英文资料用简短英文题名；必要时加年份或用途",
              "renamed_files": [{"original": str(s.relative_to(PACKAGE)),
                                 "renamed": str(d.relative_to(PACKAGE)),
                                 "sha256_before_and_after": h} for s, d, h in plan],
              "updated_references_in": updated}
    (PACKAGE / "05_核验记录" / "资料重命名记录.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print("已规范命名", len(plan), "个文件；更新", len(updated), "个引用文件。")
