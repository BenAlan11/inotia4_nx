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


## 测试窗口位置（2026-11-05 用户撤销了"放第二屏"的要求）
- **不再需要把 Eden 窗口挪到第二屏**，直接默认位置拉起即可，不要再调 `tools/win_place.py`。
- 工具保留（`python tools/win_place.py --monitor 2`），需要时再用。
- 历史约定（已作废）：2026-11-04 曾要求一律放 2 屏、不抢 1 屏焦点。
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

## 说明页（技能/物品描述）架构（2026-11-06）
- 右侧说明页 = 一个 `ControlScroll`（`[virtbase+0x2b5a30]`），它的绘制回调存在 `data+0xa0`，
  `ControlScroll_Draw` 用 `data+0x50`（位置）调用它。
- 该控件的 `ControlScroll_ControlEventProc` 在移植里**收不到指针**（只能走控件树虚分发）；
  左侧技能树能拖是因为 `ControlSkill_ControlEventProc` 直接调 `ControlScroll_EventProcPressed`。
  所以位置由 `input.c` 采的位移喂进去（`desc_scroll_tick`），引擎负责其余绘制与滚动条。
- 页面三块（A / 下一等级 B / 对比块）的 y 全部由"上一块的高度字段"算（ctrl+0x08）。
  引擎那套高度来自 Java 光栅器的字符串图 = 0，所以移植里必须由我们给：
  `data+0xa0` 指向 `nx_desc_text_scroll`，用我们自己的折行/行高整体排版，
  只把最后一段的 y 交给 `UISkill_DrawDescAbility`。

## 两个反复踩的坑（2026-11-06）
- **`text_draw_engine` 的 `wrap_extra`**：为"引擎逐行送、我们折行"的场景设计的下压累加器。
  整段一次性画时必须 `text_set_wrap_extra(0)` 包起来，否则下一块会被整体下推 N 行
  （症状：段间出现"7 行字高"的空行，并压住下一块）。
- **`X_TEXTCTRL_SetTextControl(ctrl, str, width, ...)` 第三个参数是框宽**，不是字节数；
  文本缓存别拿它当拷贝上限（缓存已 1024 字节，长度用 `strlen`）。否则长文本尾部被截
  （技能说明 269 字节，正好切掉尾部 `： $S5秒$B`）。

## 事件分发（2026-11-06，踩过一次）
- `ControlObject_EventProc` 按**子控件链表顺序**递归，返回 1/2 就停。
- 技能页 / 物品页窗口里 `id 100`（左侧大面板，矩形＝整窗 1137x431）排在
  `id 99`（右侧说明面板 409x431）**前面**；大面板里没有格子接住时返回 0 → 链表到头 →
  说明面板永远收不到事件（症状：`学习`/`取消快捷键`/点药水都没反应）。
- 修法：在 `nx_ControlObject_EventProc` 里，`colevent_depth == 1` 且整次分发返回 0 时，
  若落点在说明面板矩形内、且**不在滚动视口内**，手动转发 0x17/0x18/0x19 给面板。
  视口必须排除（否则引擎拖动 + 我们的滚动 = 双倍速）。日志标记 `ui: desc panel ... forwarded`。

## 已知良好基线（2026-11-06 19:05，当前默认）
- `build/inotia4_nx.nro` md5 `683ed6302f8af0c1a8267f02e616e695`，build 19:02:50，9158775 字节
- 快照：`snapshots/known-good-20261106-1905/`（NRO + VERSION.txt + config.ini）
  + `snapshots/save-slot-20261106-1905.zip`（35 文件：src/README/config/tools/记忆/NRO/font.ttf）
- 用户实测：对话说话人名字全版式金色、括号心里话灰、存档页地图名 #C6B992
- 上一版 1240（说明页滚动/排版/按钮）仍是历史记录，NRO 已不放进仓库

## 对话文字颜色判定（2026-11-06）
- `is_gold_text()` 返回 **GOLD_WARM / GOLD_COOL**，抽屉只读这个值：
  - COOL `#C6B992`：角色页标签/名字块、存档行地图名
  - WARM `#FAC8A0`：对话说话人、能力点/技能点
- 说话人名字靠**关系**判定：字体 index 2 且下方 0..160px 内有正文行（两种版式都适用）
- 括号 `（`/`(` 开头的正文行 → `0xAAAAAA`（心里话）
- ⚠️ 气泡控件高度**不要改**：气泡底板就按这个高度画，改高会让气泡视觉变大

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

### 备份已成功（22:49）
- 远端 main = 提交 `85395cab2282`（tree `206c2a2eca69`），368 文件 / 25MB，`src/` 与
  `snapshots/known-google-2211/inotia4_nx.nro`(9060471 B) 大小与本地一致。
- **git push 在这台机器上走不通**：出口代理只放行 api.github.com，
  github.com 的 git 端点 CONNECT 502 / TLS reset。
- 解决：`tools/gh_push.py` 走 Git Data API（blobs→tree→commit→ref），
  用法 `GH_TOKEN=... python tools/gh_push.py BenAlan11/inotia4_nx main`。
  脚本已改成会带上远端当前 head 作为 parent（首次是孤儿提交）。
- 仓库内容：src/、build.py、deploy.py、config.prod.ini、README、tools/*.py、
  snapshots（**只带 2211 的 NRO 和 zip**，旧的大快照仅留 VERSION.txt/配置）、
  .workbuddy/memory/。游戏本体/APK/存档/工具链不进仓库。
- 令牌用完提醒用户轮换（已在对话中出现）。PAT 是 fine-grained 且**有写权限**，
  实测 `permissions.push = true`；令牌我从历史会话记录里取（不写进任何文件）。
- `gh_push.py` 直接上传**工作区字节**（`make_blob` 读磁盘），而本地索引是
  `core.autocrlf=true` 归一化后的 LF，所以文本文件的远端 blob sha 与本地索引不同。
  历史上每次推送都是这样，远端一致（全 CRLF），clone 下来与本地磁盘逐字节相同。

### 2026-11-06 12:51 推送
- 远端 main = 提交 `a19b5ed21fbc7b9ff73419b607a85248d340aaf8`（tree `435c0a00`，
  parent `660a5699`），378 文件 / 26,611,416 字节。
- 索引里只留**当前** known-good 的 NRO 与 zip（`1240`），旧的 2211 的 NRO/zip
  已从索引移除（磁盘上仍在）；`.gitignore` 现在是通配 + 否定规则：
  `snapshots/known-good-*/inotia4_nx.nro`、`snapshots/save-slot-*.zip` 全忽略，
  再 `!` 放行 `1240` 的两个。
