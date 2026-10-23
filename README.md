# opto-chess-robot-2027

面向 2027 年全国大学生光电设计竞赛「智能车光电象棋对抗」的项目仓库，保存开发文档、上下位机程序、硬件设计、核心资料和验证成果。

当前处于**方案设计与算法示例验证阶段**，尚未完成整机程序与实车验证。2026-10-23 会议已确认碳纤维底板、麦克纳姆轮的采购方向，具体规格待确认，采购待执行。方案中的性能指标及未确认选型是设计建议，比赛要求以官方发布及后续修订为准。

## 项目入口

| 内容 | 入口与当前状态 |
| --- | --- |
| 计划 | [开发计划](01_docs/01_plans/01_开发计划.md)：统一维护任务状态、下一步和完成标准 |
| 方案 | [整机方案](01_docs/02_design/01_整机方案.md)、[四种方案](01_docs/02_design/02_四种方案.md)：当前方向与候选比较 |
| 会议 | [2026-10-23 会议纪要](01_docs/04_meetings/01_20261023_会议纪要.md)：保存决定与落实情况 |
| 硬件 | [硬件清单](03_hardware/01_硬件清单.md)：仅登记已确认方向，不预设型号和采购状态 |
| 资料 | [资料清单](01_docs/05_resources/01_资料清单.md)、[官网下载链接](01_docs/05_resources/02_官网下载链接.md) |
| 透视变换 | [研究记录](01_docs/03_records/01_20261023_透视变换.md)：已完成 OpenCV 官方样图的单图示例与图内误差检查，项目相机待验证 |
| 象棋识别 | [验证记录](01_docs/03_records/02_20261023_象棋识别.md)：两张真实照片、12 个人工 ROI 中 7 个字样与字色正确、5 个拒识，将/帅正确 3/4；另有低机位生成图校正演示 |

象棋识别尚未完成全图自动检测与比赛场景验收；生成图演示不代表实拍精度。原图、模型、来源、预测与结果均可从对应记录访问。

## 目录

```text
opto-chess-robot-2027/
├── README.md                         项目总入口
├── AGENTS.md                         项目维护规则
├── 01_docs/                          文档原稿，由 Docusaurus 直接读取
│   ├── 01_plans/                     开发计划
│   ├── 02_design/                    整机方案与接口说明
│   ├── 03_records/                   实验与联调记录
│   ├── 04_meetings/                  会议纪要
│   └── 05_resources/                 资料清单与来源入口
├── 02_programs/                      正式程序
│   └── 01_host/                      上位机
│       ├── 01_perspective/           透视变换示例
│       └── 02_xiangqi/               棋面识别、校正与评估
├── 03_hardware/                      硬件清单及后续结构、电气设计
├── 04_references/                    外部原始资料
│   ├── 01_official/                  官方赛题与规则
│   ├── 02_papers/                    论文
│   └── 03_datasheets/                硬件与接口手册
├── 05_data/                          数据与成果
│   ├── 01_raw/                       原始输入、标注与来源
│   ├── 02_models/                    模型、版本、哈希与许可
│   └── 03_results/                   正式结果图、预测与评估
├── 06_website/                       Docusaurus 配置与启动脚本
└── temp/                             临时程序、依赖、缓存与构建产物
```

下位机开始开发后，在 `02_programs/02_firmware/` 保存完整 IDE 工程；机械结构、电路与接线产生实际设计文件后，再在 `03_hardware/` 内细分。上下位机通信协议确定后，以 `01_docs/02_design/` 中的一份协议文档为准，不提前建立空目录。

## 获取与运行

获取仓库需要 [Git](https://git-scm.com/)。在项目根目录执行后续命令。

```sh
git clone https://github.com/qyq11/opto-chess-robot-2027.git
```

```sh
cd opto-chess-robot-2027
```

阅读原始 Markdown 和 PDF 不需要机器人运行环境。运行 Python 示例需 [Python](https://www.python.org/downloads/)：透视变换需 3.10+，象棋识别需 3.11+，具体依赖及运行方式见对应记录。

透视变换示例：

```sh
python -m pip install -r 02_programs/01_host/01_perspective/requirements.txt
```

```sh
python -X utf8 -B 02_programs/01_host/01_perspective/perspective_demo.py
```

象棋识别的完整步骤、模型许可与中文字体要求见[运行与复核](01_docs/03_records/02_20261023_象棋识别.md#5-运行与复核)。

## 文档网站

需要 [Node.js 20+](https://nodejs.org/) 及其附带的 npm。先确认版本：

```sh
node --version
```

首次安装网站依赖：

```sh
npm --prefix 06_website run setup
```

启动本地开发服务：

```sh
npm --prefix 06_website run start
```

打开 <http://localhost:3000/>，首页为开发计划，左侧可浏览方案、记录、会议和资料。编辑 `01_docs/` 中的 Markdown 即可更新内容，停止服务使用 `Ctrl+C`。

构建静态网站并检查链接：

```sh
npm --prefix 06_website run build
```

预览构建结果：

```sh
npm --prefix 06_website run serve
```

网站依赖、缓存和输出统一写入 `temp/docusaurus/`，静态网站位于 `temp/docusaurus/build/`。清理该目录后可重新安装、构建；文档源与网站配置保存在正式目录。当前网站用于本地阅读，未配置线上发布。

算法验证采用各研究记录中的样例复核与独立评估。单图示例、网站构建和资料完整性检查不代表机器人功能测试。

## 维护与版权

文件归属、编号、临时内容及记录规则见 [AGENTS.md](AGENTS.md)。任务统一在开发计划中维护，同一事项持续更新一份记录；移动文件时同步修正链接、程序路径及来源元数据。

可通过 [Issues](https://github.com/qyq11/opto-chess-robot-2027/issues) 或 Pull Request 提交更新与勘误。补充资料时保留来源、日期和许可，区分官方要求、设计推断与实测结果。

官方文件、论文、硬件文档及开源项目的权利归各自发布方或作者所有。采用代码时需从原项目获取源码与许可并核查条件。仓库暂未为原创内容指定统一开源许可证，资料归档不表示额外转载、修改或商用授权。
