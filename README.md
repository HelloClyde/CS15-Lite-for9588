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
> 本仓库当前为私有仓库，私有 GitHub Release 提供与 BDA 配套的
> `CS15.C15PAK`，并保存用于 CI 预处理的原始 `cstrike` 素材归档。它们仅供
> 获得相关资源授权的仓库成员使用。不要公开、转发或随公开 fork 分发；如需
> 自行重建资源包，请参阅 [资源包指南](RESOURCE_PACK.md)。

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
`A:\应用\数据\CS15LITE\CS15.C15PAK`，再复制新版文件。M17 默认保留世界贴图的
原始 mip0 与 256 色调色板，七地图资源包约 19 MB；直接覆盖旧包可能因 9588 的
FAT 空闲空间碎片化而复制失败。

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
  原始 mip0/256 色世界贴图、hull 碰撞、重力、台阶和墙体滑动。大地图使用
  正面 PVS 预取与按需贴图分页，避免为了高清贴图突破约 2 MiB 工作集。
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
- **队友和敌人**：共享低分辨率人物贴图、历史步行动画、离线 bone merge
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
- **计分板**：按住右侧 `SCORE` 显示玩家和 7 个 Bot 的阵营、击杀、死亡及
  T/CT 回合比分。
- **掉枪、拾枪与观战**：玩家/Bot 死亡会把当前枪和剩余弹药留在世界中，
  `USE` 拾取并按主武器/手枪槽替换；短按 `WEAPON` 切枪、长按 0.7 秒主动
  丢枪。死亡后自动观战存活队友，按实体确定键切换目标。
- **完整游戏流程**：经典主菜单、地图与 T/CT 选择、按主武器/手枪槽替换的
  分页购买、5 秒冻结期、出生购买区与 20 秒购买期、经典目标/歼灭胜负提示、
  金钱、护甲、弹药和队伍存活数。
- **战斗 HUD 与反馈**：雷达、回合计时、动态准星、弹匣/备弹、护甲、手雷数、
  击杀信息、尸体、血花/弹痕和目标状态。
- **真机音频**：11025 Hz signed 16-bit mono 资源流式读取，播放时以固定
  2 KiB scratch 升采样到 22050 Hz；玩家和 Bot 两路流式混音，
  支持历史枪声、换弹、脚步、受伤/死亡、命中、投掷物、人质和胜负语音，
  支持 11025/22050 Hz 原始音效离线归一化，退出时恢复系统衰减值。
- **横屏输入**：实体方向键移动、实体确定键开火、触摸拖动视角，右侧半透明
  虚拟键负责使用/安装/拆弹、购买、换弹、切枪、跳跃、下蹲和计分板。
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
| 按住 `SCORE` | 显示计分板 |
| 长按退出键 | 返回系统菜单 |

## 小内存设计

9588 无法承受桌面 GoldSrc 的资源常驻方式，因此运行时使用按地图申请的 arena
和离线预处理：

| 区域 | 上限 / 当前峰值 |
|---|---:|
| 地图 arena | 仅为当前地图动态申请；32,177 B（Iceworld）到 1,075,328 B（Office），PVS 流式 |
| 贴图 arena | 按地图动态申请；Iceworld 原始贴图全集 150,528 B；Office 按需缓存上限 1,000,000 B |
| 模型 arena | 64 KiB；最大组合估算约 63,008 B |
| 资源/音频共享 scratch | 2 KiB |
| 地图工作集 | 地图 arena + 贴图缓存硬限制 2 MiB；Office 配额 2,088,000 B |
| 完整静态镜像 | 当前 501,104 B；构建时强制检查 1,572,864 B 上限 |

世界贴图默认保留原始 mip0 与 256 色 RGB565 调色板；只有显式传入
`-CompressWorldTextures` 才会启用 32 像素/64 色的低内存方案。Assault、Italy、
Inferno、Nuke 和 Office 根据正面 PVS 预取贴图，保守可见集超出缓存时再按实际绘制
材质分页。几何与碰撞数据仍保持常驻，避免 9588 FAT 随机读取拖慢帧率。玩家皮肤
预滤波为 16×16；900 项以上资源目录在磁盘上按 ID 排序并二分查询，BSP 的 PVS
按需流式读取，动画只流入当前顶点帧，音频按 512-byte 资源块读取并输出为
1024-byte 固件块，不常驻整段 PCM。

性能采集方法与本版模拟器基准见
[docs/PERFORMANCE.md](docs/PERFORMANCE.md)。真机退出游戏后，把
`A:\应用\数据\CS15LITE\runtime.log` 交给
`tools\analyze-runtime-log.ps1`，即可得到平均值、中位数、P10、最低 FPS
以及渲染/提交耗时和三个 arena 峰值。

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
仅用于说明兼容性和开发状态。

私有 Release 中的原始素材归档和转换资源包仅供已获得对应游戏资源授权的成员
使用，不得公开传播。若仓库重新转为公开，必须先移除这些 Release 资源。

源代码按 [GNU GPL v2 or later](LICENSE) 发布。`sdk` submodule 保留其自身许可证。
