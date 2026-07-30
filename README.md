<p align="center">
  <img src="docs/images/readme-header.png" alt="CS15 Lite for BBK 9588" width="100%">
</p>

<p align="center">
  <a href="https://github.com/HelloClyde/CS15-Lite-for9588/actions/workflows/release.yml"><img src="https://github.com/HelloClyde/CS15-Lite-for9588/actions/workflows/release.yml/badge.svg" alt="Tag release"></a>
  <a href="https://github.com/HelloClyde/CS15-Lite-for9588/releases/latest"><img src="https://img.shields.io/github/v/release/HelloClyde/CS15-Lite-for9588" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/platform-BBK%209588-e4572e" alt="BBK 9588">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--2.0--or--later-blue" alt="GPL-2.0-or-later"></a>
</p>

`CS15 Lite` 是面向步步高 BBK 9588 学习机的小内存 Counter-Strike 1.5
风格单机移植。它不是在 9588 上运行完整 GoldSrc，而是把合法持有的历史地图、
StudioMDL、贴图、动画和音效离线转换成紧凑资源，再由一个专门的定点数软件 3D
运行时加载。

当前版本已经可以在真机上完成主菜单、地图与阵营选择、分槽购买、移动瞄准、
完整弹匣/备弹换弹、Bot 对战、炸弹与人质目标和回合循环。画面为原生
`320×240` 横屏，实体方向键移动，触摸拖动视角，实体确定键开火。

> [!IMPORTANT]
> 使用 `CS15.C15PAK` 或从原始素材重新构建资源包前，请确保已通过正规渠道
> 取得相应游戏资源的合法授权。重建方法参见
> [资源包指南](RESOURCE_PACK.md)。

## 游戏截图

![CS15 Lite 游戏截图拼图](docs/images/gameplay-collage.png)

<sub>经典主菜单与前三张真机实战截图拼图。</sub>

## 快速开始

### 1. 下载一键安装包

从 [Releases](https://github.com/HelloClyde/CS15-Lite-for9588/releases/latest)
下载 `CS15-Lite-for-9588-<版本>.zip`。解压后，将其中的 `应用` 文件夹复制到
9588 的 `A:\` 根目录并选择合并，即可同时安装程序和配套资源包。

ZIP 内已经排好真机目录：

```text
应用\程序\CS15Lite.bda
应用\数据\CS15LITE\CS15.C15PAK
```

从旧版升级时，请先删除真机上的旧
`A:\应用\数据\CS15LITE\CS15.C15PAK`，再复制新版文件。M20 默认保留 `v0.1.3`
的最大 64 像素 authored mip 与 256 色世界贴图，并将人物皮肤保留为最大
64×64；地图 PVS 与全部贴图在进入地图时常驻，避免游玩中反复读取 FAT。
直接覆盖旧包可能因 9588 的 FAT 空闲空间碎片化而复制失败。

### 2. 手动安装（可选）

也可以从同一个 Release 单独下载 `CS15Lite.bda`，复制到：

```text
A:\应用\程序\CS15Lite.bda
```

再下载配套的 `CS15.C15PAK`，复制到：

```text
A:\应用\数据\CS15LITE\CS15.C15PAK
```

也可以按照 [RESOURCE_PACK.md](RESOURCE_PACK.md)，使用自己合法持有的
CS 1.5 资源在本地重新生成。

程序与资源包有版本标记；若二者不匹配，启动地图时会显示
`RESOURCE PACK OUTDATED`，不会带着错误布局继续运行。

### 3. 启动

从学习机的游戏分类启动 `CS Lite`。首次建议进入 `Options`：

- `DIFFICULTY`：选择 `EASY / NORMAL / HARD`；
- `AUDIO`：打开或关闭真机 PCM 音效。

## 功能介绍

- **七张经典地图**：`de_dust2`、`fy_iceworld`、`cs_assault`、`cs_italy`、
  `de_inferno`、`de_nuke`、`cs_office`；不打包 `de_dust`，并保留 BSP/PVS、
  `v0.1.3` 同款 64 像素/256 色世界贴图、hull 碰撞、重力、台阶和墙体滑动。
  M20 将当前地图的 PVS 和贴图全部常驻，并让可见面缓存覆盖完整
  10,240 面 bitset，避免大地图进入复杂叶子后丢失世界画面。
- **23 把经典武器**：Knife、Glock、USP、P228、Desert Eagle、Dual Elite、
  Five-SeveN、M3、XM1014、MAC-10、TMP、MP5、UMP45、P90、AK-47、SG552、
  M4A1、AUG、Scout、AWP、G3SG1、SG550、M249，均使用历史第一人称模型、
  贴图和可用的 Idle / Fire / Reload / Draw 动画。
- **枪械规则**：按武器区分射速、弹匣、备弹、移动散布、连续射击后坐力、
  距离衰减、穿透层数和命中部位；霰弹枪进行多弹丸判定，头部命中有独立
  伤害与反馈。
- **武器副功能**：Scout/AWP/G3SG1/SG550 可开镜，USP/M4A1 可切换消音，
  Glock 可切换三连发，Knife 支持慢速重击；右侧 `ALT` 键按当前武器显示功能。
- **枪口火焰**：将历史 `muzzleflash1.spr` / `muzzleflash2.spr` 离线压缩为
  23×23、4-bit 调色板的加色 Sprite，并绑定 StudioMDL 枪口 attachment。
- **队友和敌人**：完整保留原始 StudioMDL 的 740/790 个三角形、跨骨骼
  关节接缝、最大 64×64/256 色人物皮肤、历史步行动画和离线 bone merge
  第三人称武器，固定池最多 7 个 Bot。
- **可调 AI**：Entry / Support / Anchor 三种角色共享低内存路线，具备最后
  目击位置追踪、队友间距、远近射击区间、低血量撤退、侧移、短点射和目标
  分工；难度实际改变反应时间、连发节奏、命中率、伤害和移动速度。
- **炸弹回合**：`de_dust2`、`de_inferno`、`de_nuke` 从 BSP 实体提取 A/B 点；
  T 携带、安装和引爆 C4，
  携带者死亡会在世界中掉落、T 可拾取，CT 可用拆弹器加速拆弹；Bot 也会
  拾取、安装与拆除，HUD 显示状态和倒计时。
- **人质回合**：`cs_assault`、`cs_italy` 从 BSP 提取人质和营救区；CT 使用
  人质后会跟随，CT Bot 也会带回人质，营救完成进入目标胜负判定。
- **投掷物与装备**：购买菜单提供 HE、Flashbang、Smoke、Kevlar、Helmet 和
  Defuse Kit；手雷有抛物线、碰撞反弹、爆炸伤害，闪光遮屏，烟雾会遮断 Bot
  视线。
- **地图交互**：离线提取梯子、门、按钮、可破坏物和平台；运行时将它们加入
  玩家/Bot 碰撞、使用与子弹破坏判定。
- **暂停与计分板**：游戏中短按实体退出键冻结回合、Bot、玩家、炸弹和动画
  计时，并显示玩家和 7 个 Bot 的阵营、击杀、死亡及 T/CT 回合比分；
  可继续游戏或释放当前地图资源后返回主菜单。
- **掉枪、拾枪与观战**：玩家/Bot 死亡会把当前枪和剩余弹药留在世界中，
  `USE` 拾取并按主武器/手枪槽替换；短按 `WEAPON` 切枪、长按 0.7 秒主动
  丢枪。死亡后自动观战存活队友，按实体确定键切换目标。
- **完整游戏流程**：经典主菜单、地图与 T/CT 选择、按主武器/手枪槽替换的
  手枪/霰弹枪/冲锋枪/步枪/机枪/装备分类购买、固定右下角开始按钮、
  5 秒冻结期、出生购买区与 20 秒购买期、经典目标/歼灭胜负提示、金钱、
  护甲、弹药和队伍存活数。
- **战斗 HUD 与反馈**：雷达、回合计时、动态准星、弹匣/备弹、护甲、手雷数、
  击杀信息、尸体、血花/弹痕和目标状态。
- **真机音频**：11025 Hz signed 16-bit mono 资源流式读取，播放时以固定
  2 KiB scratch 升采样到 22050 Hz；玩家和 Bot 两路流式混音，
  支持历史枪声、换弹、脚步、受伤/死亡、命中、投掷物、人质和胜负语音，
  支持 11025/22050 Hz 原始音效离线归一化，退出时恢复系统衰减值。
- **横屏输入**：实体方向键移动、实体确定键开火、触摸拖动视角，右侧半透明
  虚拟键负责使用/安装/拆弹、购买、换弹、切枪、跳跃、下蹲和武器副功能。
- **低开销渲染**：世界多边形使用视锥快速接受/拒绝，枪模采用扫描线光栅；
  横屏 RGB565 以 8×8 分块转置后直接写入固件 framebuffer，不再经过整帧
  旋转暂存区，也不走 `bda_gui_render_picture()`。

## 控制

| 输入 | 功能 |
|---|---|
| 实体方向键 | 前进、后退、左右平移 |
| 实体确定键 | 开火 / 菜单确认 |
| 触摸拖动 | 跟手转动视角 |
| `USE / BUY` | 长按安装或拆除 C4；点击使用人质/门/按钮；购买期内在出生区打开购买菜单 |
| `RELOAD` | 换弹 |
| 短按 / 长按 `WEAPON` | 切换已拥有武器 / 丢下当前武器 |
| `ALT` | 开镜、消音、Glock 连发、Knife 重击或投掷已购买手雷 |
| `JUMP` / `CROUCH` | 跳跃 / 下蹲 |
| 短按退出键 | 暂停 / 恢复；暂停菜单显示比分并可返回主菜单 |
| 长按退出键 | 紧急返回系统菜单 |

## 小内存设计

9588 无法承受桌面 GoldSrc 的资源常驻方式，因此运行时使用按地图申请的 arena
和离线预处理：

| 区域 | 上限 / 当前峰值 |
|---|---:|
| 地图 arena | 仅为当前地图动态申请；40,000 B（Iceworld）到 1,300,000 B（Office），含完整 PVS |
| 贴图 arena | 按地图动态申请；Iceworld 经典贴图全集 15,360 B；Office 全集 520,176 B |
| 模型 arena | 96 KiB；64×64 队伍皮肤只共享加载一次 |
| 资源/音频共享 scratch | 2 KiB |
| 地图工作集 | 地图 arena + 常驻贴图硬限制 4 MiB；当前最大配额约 2.30 MiB |
| 完整静态镜像 | 可见面索引增加约 17 KiB；构建时仍强制检查 1,572,864 B 上限 |

世界贴图默认使用 `v0.1.3` 的最大 64 像素 authored mip 与 256 色 RGB565
调色板。`-CompressWorldTextures` 显式启用 32 像素/64 色低内存档；
`-FullWorldTextures` 可生成 mip0 实验包，但当前定点仿射光栅器没有 GoldSrc 的
按距离 mip 选择与过滤，真机观感反而更容易走样。大地图的几何、碰撞、PVS
和全部贴图在加载阶段一次读入，游玩中不再做世界资源 FAT 随机读取。玩家皮肤
默认保留最大 64×64/256 色，
`-CompressPlayerTextures` 才会显式降为 16×16；900 项以上资源目录在磁盘上按
ID 排序并二分查询。T/CT 行走动画以及全部第一人称枪模、贴图和动画在加载阶段
常驻内存，换枪不再访问 FAT；音频同样优先常驻，
低内存时才回退到 512-byte 分块读取，并输出为
1024-byte 固件块。

性能采集方法与本版模拟器基准见
[docs/PERFORMANCE.md](docs/PERFORMANCE.md)。真机退出游戏后，把
`A:\应用\数据\CS15LITE\runtime.log` 交给
`tools\analyze-runtime-log.ps1`，即可得到平均值、中位数、P10、最低 FPS
以及 logic/world/entities/view/HUD/audio/present 分项平均/峰值、渲染/
提交耗时和缓存峰值。M20 performance pass 7 还会把 logic 拆成
开火、玩家、Bot、目标/回合四部分，报告逻辑追赶与丢弃的历史步数以及
动画/枪模常驻内存；世界与模型光栅器对常见的不透明贴图使用专门化像素
路径，并对 2 的幂贴图及范围内 UV 进一步走快速分支，同时单独报告世界
帧缓存/深度清理耗时。

## 从源码构建

环境要求：Windows、PowerShell、Git、Python 3.10+ 和可用的 `gcc` 主机编译器。
SDK 以 `sdk/` submodule 引用：

```powershell
git clone --recurse-submodules https://github.com/HelloClyde/CS15-Lite-for9588.git
cd CS15-Lite-for9588

.\sdk\scripts\setup_toolchain.ps1
.\tools\test.ps1
.\tools\build.ps1 -Clean
```

输出：

```text
build\engine\CS Lite.bda
```

构建脚本固定使用 `-Os`、独立函数/数据 section 和 `--gc-sections`，并在链接后
强制检查 1.5 MiB 静态镜像上限。资源格式见 [docs/formats.md](docs/formats.md)。

## Tag Release

`.github/workflows/release.yml` 只在推送 `v*` tag 时运行：

```powershell
git tag v0.1.0
git push origin v0.1.0
```

工作流从固定的素材 Release 下载 `CS15-original-cstrike-assets.zip` 并验证
SHA-256。随后离线预处理地图、模型、贴图、动画和音频，运行 C/Python 测试，
构建 BDA，然后向 GitHub Release 上传：

- `CS15Lite.bda` 与 SHA-256；
- CI 重新生成的 `CS15.C15PAK` 与 SHA-256；
- `CS15-Lite-for-9588-<版本>.zip`：解压后可直接复制到 9588 的安装包；
- 资源转换器、格式文档和资源包指南组成的 resource-tools ZIP。

原始素材归档固定保存在 `v0.1.1` Release，新的 tag 不会重复上传这份
158 MB 归档。

## 目录

```text
sdk/                 BBK 9588 SDK submodule
src/app/             游戏流程、菜单、Bot 与回合逻辑
src/audio/           双通道流式 PCM
src/model/           紧凑 StudioMDL 与动画流
src/render/          RGB565 世界/模型软件光栅器
src/world/           BSP、碰撞和玩家移动
tools/assetc.py      合法本地资源离线转换器
tools/build-resource-pack.ps1
                      从 cstrike 生成并校验资源包
tools/build.ps1      BDA 交叉构建与静态内存门禁
tests/               主机 C 测试与资源格式测试
```

## 声明与许可证

本项目是非官方爱好者工程，与 Valve、Counter-Strike、步步高无隶属或背书关系。
Counter-Strike、Half-Life 及相关素材的权利归各自权利人所有；仓库中的游戏截图
仅用于说明兼容性和开发状态。使用相关原始素材和转换资源包前，需通过正规渠道
取得相应授权。

源代码按 [GNU GPL v2 or later](LICENSE) 发布。`sdk` submodule 保留其自身许可证。
