# 项目长期约定（Inotia 4 Switch port）

## 调试授权与流程（用户明确授权，2026-11-03）
- **推送 dist/ 前直接结束 eden 进程**，不需要每次询问：
  `Stop-Process -Id <pid> -Force`（`Get-Process eden,eden-cli` 可能有多个实例，
  包括关窗口后残留的）。eden 会占住 `dist/switch/inotia4/inotia4_nx.nro`，
  不结束就报 "Device or resource busy"。
- 调试阶段**只推 Eden**（`%APPDATA%\eden\sdmc\switch\inotia4\`），
  **不推 SD 卡**；等 Eden 里游戏跑通再推卡。
- 推 Eden 不需要 `deploy.py --eden`（它要求 `~/Downloads/Inotia4.apk` 存在，
  且会覆盖 config.ini）。直接 `cp build/inotia4_nx.nro` 到 sdmc + dist，再 `md5sum` 核对。

## 环境事实
- `base(4).apk`（用户给的那个）是 **armeabi-v7a 32 位**包，端口用的是 arm64
  `libgame.so`（`data/libgame.so`，2793248 字节）。符号级结论以 arm64 为准。
- 存档目录：`save/com.com2us.inotia4.normal.freefull.google.global.android.common/`
  下 `<32hex>/save%d.dat` 与 `<32hex>A/thpcheckslot%d.dat`。
- Eden 安装位置：`C:\1GAME\BaiduDownload\eden\eden\eden.exe`；
  启动 `eden.exe -g "<sdmc>\switch\inotia4\inotia4_nx.nro"`。
  ⚠️ 我用工具启动的 GUI 进程会被沙箱回收，**窗口必须由用户自己拉**。

## 已验证的基线构建（2026-11-03）
- `build/inotia4_nx.nro` md5 `aa29fc3837bdf522e03c77f34a9260b9`，build 14:24:24
- 用户实测通过：存档读写、跨重启读档、删除存档、回主菜单后再按 START 进存档页
- 快照：`snapshots/save-fix-20261103-v2.zip`（src + README + 记忆）


## 测试窗口必须放第二屏（用户明确要求，2026-11-04）
- **拉起/测试一律在 2 屏**，不能抢占或最小化用户的 1 屏内容。
- 工具：`python tools/win_place.py --monitor 2 --watch 30`（主屏算 1 号）。
  用 `SetWindowPos + SWP_NOACTIVATE` 移动，不抢焦点。
- 标准流程：先起 eden（后台），再跑上面的命令；窗口出现 0.5s 内被挪到 2 屏。
- `--list` 可查显示器与窗口当前坐标。

## 字体（2026-11-04）
- 卡上 `font.ttf` 现在放的是**原机字体** `DroidSansFallback.ttf`（3.7MB，50951 字形），
  来自用户提供的 `原机字体.zip`；`dist/switch/inotia4/font.ttf` 同步。
- 加载日志：`text: loaded font sdmc:/switch/inotia4/font.ttf (N glyphs)`；
  没有 font.ttf 时回退主机共享字体（日志 `no TTF on the card`）。
- 之前"字体导致卡死"的结论是**错的**，真因是日志里调了引擎的 `SGL_GetTextSize`。

## GitHub 备份（2026-11-04）
- 本地已 `git init`（分支 main），一个提交 `960ef85`，375 文件 / 58MB：
  `src/`、`build.py`、`deploy.py`、`config.prod.ini`、`README.md`、`tools/*.py`、
  `snapshots/`（每个 known-good 的 NRO + VERSION.txt + 源码 zip）、`.workbuddy/memory/`。
- `.gitignore` 排除：`build/`、`dist/`、`data/`、`apk_new/`、`ref*/`、`wip/`、
  `tools/dkp_rootfs|libnx*|*.zip|*.bin`、`*.png`、`*.log`、存档备份。
  **游戏本体/APK/存档不进仓库。**
- 远端：`BenAlan11/inotia4_nx`（public，main 上是 10-29 的旧版移植代码）。
  计划推到新分支 `backup-20261104`，不动 main。
- ⚠️ 用户给的 fine-grained PAT **只有读权限**：`POST /git/refs` 返回
  "Resource not accessible by personal access token"，`git push` 403。
  需要 Contents: Read and write（或 classic token 的 repo scope）才能推。
- 令牌未写入 .git/config，也已 `credential-manager erase` 清掉缓存。
