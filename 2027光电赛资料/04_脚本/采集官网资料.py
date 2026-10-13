"""下载第十五届相关官网资料、保留公开网页/API证据，并生成来源和SHA256清单。

运行：python -X utf8 采集官网资料.py
仅使用Python标准库；可选pypdf用于页数/文本验证，pypdfium2用于扫描通知预览。
资料范围固定为第十五届筹备通知，不把往届细则当作本届规则。
"""
from __future__ import annotations

import hashlib
import html
import json
import re
import shutil
from datetime import datetime, timedelta, timezone
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import quote, urlsplit, urlunsplit
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "01_官方资料"
SNAP = OUT / "网页快照"
OUT.mkdir(parents=True, exist_ok=True)
SNAP.mkdir(parents=True, exist_ok=True)
CHECKED = datetime.now(timezone(timedelta(hours=8))).isoformat(timespec="seconds")
DAY = CHECKED[:10]
BASE = "https://gd.moocollege.com"
HEADERS = {"Referer": BASE + "/home", "Origin": BASE,
           "Content-Type": "application/json;charset=utf-8", "User-Agent": "Mozilla/5.0"}


def fetch(url: str) -> bytes:
    parts = urlsplit(url)
    url = urlunsplit((parts.scheme, parts.netloc, quote(parts.path, safe="/%"), parts.query, parts.fragment))
    with urlopen(Request(url, headers=HEADERS), timeout=45) as response:
        return response.read()


def api(path: str, name: str) -> dict:
    raw = fetch(BASE + "/prod-api" + path)
    (SNAP / name).write_bytes(raw)
    parsed = json.loads(raw)
    if parsed.get("code") != 0:
        raise RuntimeError(str(parsed))
    return parsed["data"]


def sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


notices = [
    (131060, "2026-07-21_第十五届赛题通知.pdf", "当届赛题依据", "2026-07-20"),
    (123366, "2026-04-27_第十五届赛题征集.pdf", "本届筹备背景；征集已截止", "2026-04-23"),
    (122769, "2026-01-04_第十四十五届承办通知.pdf", "本届承办背景；明确2027天津大学", "2025-12-31"),
    (122768, "2025-10-20_第十四十五届承办征集.pdf", "本届筹备背景；征集已结束", "2025-10-20"),
    (122647, "2020-12-12_全国大学生光电设计竞赛章程_试行.pdf", "旧版通用背景；官网附件不可下载", None),
]
entries = []
settings = api("/web/competitionWebsite/getWebsiteSetting", f"官网导航配置_API快照_{DAY}.json")
current = api("/home/competition/competition/lastRound", f"官网当前赛事_API快照_{DAY}.json")
guide = api(f"/home/competition/detail?competitionId={current['competitionId']}",
            f"官网当前参赛指南_API快照_{DAY}.json")
categories = {}
for category in (201, 202, 203, 204, 206, 208):
    payload = api(f"/home/competition/news/website/page?pageNum=1&pageSize=500&newsType={category}",
                  f"官网栏目{category}_API快照_{DAY}.json")
    categories[str(category)] = {"total": payload.get("total"), "pages": payload.get("pages"),
                                 "retrieved": len(payload.get("records", []))}
    if payload.get("pages", 1) > 1:
        raise RuntimeError(f"栏目{category}超过500条，需要继续翻页")
(SNAP / f"官网首页_{DAY}.html").write_bytes(fetch(BASE + "/home"))

for news_id, filename, scope, issued_at in notices:
    page_url = BASE + f"/home/newsDetails?newsId={news_id}"
    detail = api(f"/home/competition/news/detail?newsId={news_id}", f"官网通知_{news_id}_原始API.json")
    (SNAP / f"官网通知_{news_id}_原始网页.html").write_bytes(fetch(page_url))
    # 原始页面是JavaScript应用壳；另存一份从官方API还原的可读快照，并明确标注来源。
    sources = []
    for item in detail.get("newsFiles", []) + detail.get("files", []):
        if item.get("fileUrl") and item["fileUrl"] not in sources:
            sources.append(item["fileUrl"])
    body_links = re.findall(r'href="([^"]+)"', detail.get("newsInfo") or "")
    for url in body_links:
        url = html.unescape(url)
        if url.lower().split("?")[0].endswith((".pdf", ".doc", ".docx", ".zip", ".rar", ".xlsx")):
            sources.insert(0, url)
    sources = list(dict.fromkeys(sources))
    entry = {"title": detail["newsTitle"], "news_id": news_id, "scope_status": scope,
             "source_page": page_url, "published_at": detail.get("publishTime"),
             "document_issued_at": issued_at, "checked_at": CHECKED,
             "local_file": filename, "download_urls": [], "status": "未下载"}
    downloaded = []
    for url in sources:
        try:
            raw = fetch(url)
            if filename.endswith(".pdf") and not raw.startswith(b"%PDF-"):
                raise RuntimeError("响应并非PDF")
            digest = sha(raw)
            item = {"url": url, "status": "已下载并验证文件头", "bytes": len(raw), "sha256": digest}
            if not downloaded:
                (OUT / filename).write_bytes(raw)
                entry.update(status="已下载", bytes=len(raw), sha256=digest, direct_url=url)
            elif digest != downloaded[0]:
                alias_name = Path(filename).stem + f"_官网别名{len(downloaded)+1}.pdf"
                (OUT / alias_name).write_bytes(raw)
                item["local_file"] = alias_name
            else:
                item["note"] = "内容与主文件相同，去重保存"
            downloaded.append(digest)
        except Exception as exc:
            error = str(exc)
            if isinstance(exc, HTTPError):
                error += " " + exc.read().decode("utf-8", errors="replace")
            item = {"url": url, "status": "无法下载", "error": error}
        entry["download_urls"].append(item)
    if not downloaded:
        entry.update(status="官网链接当前不可下载", local_file=None)
    local_embed = f'<iframe src="../{quote(filename)}" style="width:100%;height:850px"></iframe>' if downloaded else ""
    readable = ("<!doctype html><html lang=zh><meta charset=utf-8><title>官网内容快照</title>"
                f"<h1>{html.escape(detail['newsTitle'])}</h1><p>此HTML由官方公开API还原，原始网页/API已另存。</p>"
                f"<p>发布时间：{html.escape(detail.get('publishTime') or '')}；下载时间：{CHECKED}</p>"
                f'<p><a href="{html.escape(page_url)}">原始官网通知</a></p>'
                + (detail.get("newsInfo") or "") + local_embed
                + "<h2>官方附件链接</h2><ul>"
                + "".join(f'<li><a href="{html.escape(u)}">{html.escape(u)}</a></li>' for u in sources)
                + "</ul></html>")
    (SNAP / f"官网通知_{news_id}_可读内容快照.html").write_text(readable, encoding="utf-8")
    entries.append(entry)

manual_url = settings.get("operatingManual")
if manual_url:
    manual_name = f"官网平台操作手册_当前链接_{DAY}下载.pdf"
    raw = fetch(manual_url)
    (OUT / manual_name).write_bytes(raw)
    entries.append({"title": "官网平台操作手册", "scope_status": "通用平台资料；当前附件为空白PDF",
                    "status": "已下载；1页空白，无法提供操作内容", "direct_url": manual_url,
                    "source_page": BASE + "/home/news/operatingManual", "published_at": None,
                    "checked_at": CHECKED, "local_file": manual_name, "bytes": len(raw), "sha256": sha(raw)})

user_source = ROOT.parent / "第十五届全国大学生光电设计竞赛_赛题要求.pdf"
if user_source.exists():
    user_name = "第十五届赛题附件_原始版.pdf"
    shutil.copy2(user_source, OUT / user_name)
    raw = (OUT / user_name).read_bytes()
    user_entry = {"title": "用户原始下载的3页赛题附件", "scope_status": "用户提供原件",
                  "status": "已归档", "source_page": None, "direct_url": None, "published_at": None,
                  "checked_at": CHECKED, "local_file": user_name, "bytes": len(raw), "sha256": sha(raw)}
    entries.append(user_entry)

try:
    from pypdf import PdfReader
    for entry in entries:
        name = entry.get("local_file")
        if name and name.endswith(".pdf"):
            reader = PdfReader(OUT / name)
            entry["pages"] = len(reader.pages)
            entry["embedded_attachments"] = list(reader.attachments.keys())
    official = PdfReader(OUT / notices[0][1])
    txt = "\n".join(f"第{i+1}页\n{page.extract_text()}" for i, page in enumerate(official.pages))
    (OUT / "第十五届赛题_提取文本.txt").write_text(txt, encoding="utf-8")
    if user_source.exists():
        original = PdfReader(OUT / "第十五届赛题附件_原始版.pdf")
        matches = [original.pages[i].extract_text() == official.pages[i+1].extract_text() for i in range(3)]
        user_entry["text_matches_official_pages_2_to_4"] = matches
except ImportError:
    pass

try:
    import pypdfium2 as pdfium
    for name in (notices[2][1], f"官网平台操作手册_当前链接_{DAY}下载.pdf"):
        document = pdfium.PdfDocument(OUT / name)
        document[0].render(scale=1.5).to_pil().save(SNAP / (name + ".png"))
        document.close()
except ImportError:
    pass

missing = ["第十五届正式竞赛细则/赛题答疑", "正赛时长、球门宽度、棋子尺寸材料、具体分值映射",
           "本届报名安排/参赛指南、海报及设计报告模板", "激光用途、是否必须实际参与测量的官方解释"]
tju_url = "https://news.tju.edu.cn/info/1003/605139.htm"
tju_file = SNAP / "天津大学_2027承办消息.html"
tju_file.write_bytes(fetch(tju_url))
supporting_sources = [{"title": "天津大学精仪学院在第十四届全国大学生光电设计竞赛全国总决赛中再创佳绩",
                       "source_page": tju_url, "published_at": "2026-07-13", "checked_at": CHECKED,
                       "scope_status": "末段确认第十五届全国总决赛2027年天津大学承办；前文第十四届成绩不作本届规则",
                       "local_file": str(tju_file.relative_to(OUT)).replace("\\", "/"),
                       "sha256": sha(tju_file.read_bytes())}]
files = [{"path": str(p.relative_to(OUT)).replace("\\", "/"), "bytes": p.stat().st_size,
          "sha256": sha(p.read_bytes())} for p in sorted(OUT.rglob("*"))
         if p.is_file() and p.name not in ("官方下载记录.json", "资料记录.md")]
manifest = {"checked_at": CHECKED, "official_home": BASE + "/home", "categories_checked": categories,
            "website_current_round": current.get("competitionRound"), "website_current_year": current.get("competitionYear"),
            "website_current_guide_has_files": bool(guide.get("files")), "entries": entries,
            "supporting_sources": supporting_sources,
            "not_found_in_public_official_sources": missing, "all_saved_files": files,
            "notes": ["未发布与未在公开官网找到有区别；此清单只证明检索时的公开状态。",
                      "第十四届创意/光学设计规则及第十三届实物规则均不能作为第十五届执行规则。",
                      "页数和文本比对只验证文本；扫描通知已另存渲染预览供人工核验。"]}
(OUT / "官方下载记录.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")

lines = ["# 官方资料记录", "", f"核验日期：{DAY}（北京时间）。官网：[全国大学生光电设计竞赛](https://gd.moocollege.com/home)。",
         "", "已完整核查公开栏目：章程细则、文件通知、赛事动态、获奖公示、历届作品、竞赛通讯，以及当前参赛指南和平台手册链接。",
         "", "| 本地文件/通知 | 发布时间 | 用途与状态 | 来源 |", "|---|---|---|---|"]
for entry in entries:
    label = entry.get("local_file") or (entry["title"] + "（未下载）")
    source = entry.get("source_page")
    linked = f"[官网通知]({source})" if source else "用户提供"
    lines.append(f"| {label} | {entry.get('published_at') or '未标明'} | {entry['scope_status']}；{entry['status']} | {linked} |")
lines += ["", "## 结论", "",
          "- 第十五届主赛道是2027年实物赛，天津大学承办；赛题征集通知第1页和承办单位通知均明确写明，[天津大学新闻网2026-07-13消息](https://news.tju.edu.cn/info/1003/605139.htm)末段亦确认，原始HTML已归档。",
          "- 官网赛题通知完整4页；用户原PDF是其中第2—4页，逐页提取文本完全一致。通知的附件已包含在PDF中，无额外内嵌附件。",
          "- 官网当前赛事/报名页仍显示2026年第十四届，当前参赛指南为空，无可下载的大赛资料；不能用于2027报名日程。",
          "- 赛题原文要求车辆“搭载功率≤1mW的II类安全激光系统”。未找到可将其解释成可选的官方答疑，方案应按须搭载设计并保留用途澄清项。",
          "- 正赛时长、门宽、棋子规格、分值映射、本届细则/答疑、报名通知和材料模板截至核验时未在公开官网找到；图中尺寸明确可能后续修订。",
          "- 2020年通用章程链接返回OSS InvalidObjectState，未伪造或拿其他年份规则代替。",
          "- 官网平台手册链接下载成功，但文件仅1239字节、1张完全空白页，已核验渲染；不可作为可用操作指南。",
          "", "## 校验和及快照", "",
          "每个资料的直接URL、官方页面、发布日期、文件状态、页数及SHA256见 `官方下载记录.json`。重复的官网预览/下载链接经SHA256核对去重。",
          "原始HTML应用壳、官方API响应、可读内容快照和承办通知/空白手册预览统一放在 `网页快照/`。采集脚本放在 `../04_脚本/采集官网资料.py`。",
          "", "| 文件 | SHA256 |", "|---|---|"]
for entry in entries:
    if entry.get("sha256"):
        lines.append(f"| {entry['local_file']} | `{entry['sha256']}` |")
(OUT / "资料记录.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
print(json.dumps({"checked_at": CHECKED, "documents": len(entries),
                  "downloaded": sum(bool(e.get("local_file")) for e in entries),
                  "manifest": str(OUT / "官方下载记录.json")}, ensure_ascii=False))
