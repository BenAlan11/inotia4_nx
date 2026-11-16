# 项目长期约定（Inotia 4 Switch port）

## 调试授权与部署流程（用户授权，长期有效）
- 推 dist/ 前直接结束 eden 进程，不用询问：`Stop-Process -Id <pid> -Force`
  （`Get-Process eden,eden-cli` 常有多个残留实例，会占住 `dist/switch/inotia4/inotia4_nx.nro`）。
- 调试阶段**只推 Eden**：`%APPDATA%\eden\sdmc\switch\inotia4\`，不推 SD 卡。
  直接 `cp build/inotia4_nx.nro` 到 sdmc + dist，再 md5 核对（不用 `deploy.py --eden`，
  它要求 `~/Downloads/Inotia4.apk` 且会覆盖 config.ini）。
- **用户明确要求（2026-11-09）：不要动存档**。只替换 `inotia4_nx.nro`，不碰 `save/`、
  不覆盖 sdmc 的 `config.ini`、不改 `qt-config.ini`。新功能用编译默认值，不引入必须改 ini 的开关。
  收尾用「文件数 + 总字节 + 全目录 md5 + config.ini md5」与部署前只读基线核对并回报。
- 拉起测试一律用 `C:\1GAME\BaiduDownload\eden\eden\eden.exe -g <nro>`（不要 eden-cli.exe）。
  用户接 Xbox 手柄，Eden 自己映射成 NS 手柄，不要改 `qt-config.ini` / `sdl2-config.ini`。
- 测试窗口**不再需要挪到第二屏**，默认位置拉起即可。

## 环境事实
- 端口用的 arm64 `libgame.so`（2793248 B）；`base(4).apk` 是 armeabi-v7a 32 位，符号结论以 arm64 为准。
- 存档目录 `<32hex>/save%d.dat` 与 `<32hex>A/thpcheckslot%d.dat`。
- 字体：卡上 `font.ttf` = 原机 `DroidSansFallback.ttf`（3.7MB，50951 字形）；缺失时回退主机共享字体。

## 引擎通用事实（反复用到）
- **颜色字序**：引擎 32 位颜色 `(a<<24)|(b<<16)|(g<<8)|r`，我们的 `text_draw_engine()` 吃
  `0xRRGGBBAA`。文本控件 `+0x14` 交 `GRPX_SetFontColor` 前必须 `__builtin_bswap32()`。
- **`wrap_extra` 是帧级全局累加器**（`text.c` `text_draw_engine_ex`），只在 `y <= wrap_last_y`
  清。整段一次性画必须 `text_set_wrap_extra(0)` 包住；`nx_textctrl_draw` 每控件画完清一次。
  症状：段间凭空多出数行高空格、后续控件整体下移。
- **`X_TEXTCTRL_SetTextControl(ctrl, str, width, ...)` 第三参是框宽，不是字节数**。
- **引擎全局函数指针槽常是两层**：槽地址 → `ldr x8,[x9]` → `blr x8`。直接调引擎回调前先
  反汇编数清层数，并用 `so_module.phdr` 的 `PT_LOAD|PF_X` 校验目标在可执行段内。
- **引擎 scene key handler 一律不要从移植层调用**（2026-11-08 定稿，曾跳进 .bss 卡死）：
  `menu_key_press()` 已删除；`ab_press()` 对非消息框页只留 `popup_press()`（消息框）与
  B 的 `corner_tap()`。不要按 layer 判断（角色信息页 `POPUPSTATE_Exist()` 返回 0）。
- **不要轻易 hook `SGL_grpFillRect`(0x12b484)**：曾误伤任务页标题栏三条填充。先查现有路径里
  "排除该情况"的参数，再考虑新增 hook。日志 `ra` 减模块基址 = 调用点 vaddr，
  配 `tools/disas_at.py` 定位最近导出符号。
- **`libgame.so` 是 bionic**：`open()` flags 必须翻译（`libc_shim.c:bionic_flags_to_newlib()`）。
  bionic O_CREAT=0x40/O_TRUNC=0x200/O_APPEND=0x400；newlib O_CREAT=0x200/O_TRUNC=0x400/O_APPEND=0x8。
  原样透传会导致写存档从不截断 → 尾部残留 → 读档 `ERROR(00)`。
- **存档格式**（`ENCRYPT_Process2`，表在 `0xbb7e8`）：`len=filesize-3`，`nonce=data[len+1]`，
  `plain[i]=(key[(nonce+i)%kl]+TBL[(nonce+i)&0xff])^cipher[i]`（key 索引不掩 8 位）；
  `data[len]`=密文和，`data[len+2]`=明文和。密钥 `inotia4-switch-save-v1`。
  `thpcheckslot1.dat` 应解出 `2|011110|99|99|99|99|99|END|`（最快标尺）。
  三层保护已加：写后 `"wb"` 重写精确 `len+3`、读失败扫最长合法 L 裁尾重读、启动体检不改名 `.bad`。
  `ftruncate()` 在 Switch devoptab 上不可靠，用 `"wb"` 重写代替。
- 用户要求：**修复靠读绘图逻辑，不要靠量坐标/猜阈值**。

## 页面/控件树
- root = `*(vb+0x2b4140)`；节点 `+0x18` 为相对 x,y，`+0x28` 为 w,h；`p_abs_rect` 给绝对 rect。
- **页面识别只认结构不认 id**（设置页/快捷键页 id 每次启动都变）：root 下每个 `1137x640`
  子节点是一层页面，**最后一个可识别层 = 最上层**（职业页压存档页、存档页压标题页）。
  形状分类：TITLE `369x124`+2×`300x92`；SAVE 3×`634x150`；CLASS 6×`116x114`+`292x64`；
  SETTINGS `784x98` tab 栏。游戏内 HUD 无 1137x640 层 → 导航关闭。
- 三个前置页面只用十字键 + A/B，L/R 无响应；职业页两段确认（A 选职业 → 光标跳 `292x64`
  开始游戏 → 再 A 进游戏）。
- 职业页压在存档页上时，`nx_TouchHandle_Event` 会丢弃落在底层 `634x150` 行 / `66x66` 删除 bin
  上的 tap → 导航注入点 `tx/ty` 要在控件自身矩形内避开这些 shadow 矩形。
- **控件 rect ≠ 可见 art**：标题按钮命中框 `369x124`/`300x92`，图集
  `UI_TITLEMAIN_BUT_cn_s_001.png` 里 alpha 边界 `357x77`/`292x63`，**宽高比全 4.63:1**。
  `nav_title_art()` 取命中框宽 92%、高 = 宽/4.63、居中，`tx/ty` 保持控件中心。
- 金色高亮统一走 `nav_gold_cursor()`（三层琥珀→橙金→亮金），形状由 `NavItem.shape`
  （NAV_SHAPE_ROUNDRECT/CIRCLE/PILL/ELLIPSE）指定，不要退回纯黄直角框。

## 说明页（技能/物品描述）
- 右侧说明页 = `ControlScroll`（`[vb+0x2b5a30]`），绘制回调在 `data+0xa0`，
  `ControlScroll_Draw` 用 `data+0x50` 调它。该控件在移植里收不到指针，
  位置由 `input.c` 的 `desc_scroll_tick` 喂入。
- 三块（A/下一等级B/对比块）y 由上一块高度字段 `ctrl+0x08` 算；引擎那套来自 Java 光栅器
  字符串图 = 0，必须由移植层给：`data+0xa0` 指向 `nx_desc_text_scroll` 整体排版。
- **事件分发**：`ControlObject_EventProc` 按子控件链表顺序递归，返回 1/2 即停。技能/物品页
  `id 100`（整窗 1137x431）排在 `id 99`（说明面板 409x431）前面 → 说明面板收不到事件。
  修法：`colevent_depth==1` 且整次返回 0，落点在说明面板内且不在滚动视口内时，
  手动转发 0x17/0x18/0x19（日志 `desc panel ... forwarded`）。视口必须排除，否则双倍速滚动。

## 对话文字颜色
- `is_gold_text()` 返回 GOLD_WARM `#FAC8A0`（对话说话人、能力/技能点）或
  GOLD_COOL `#C6B992`（角色页标签/名字块、存档行地图名）。说话人靠关系判定：字体 index 2
  且下方 0..160px 内有正文行。括号开头的正文行 → `0xAAAAAA`（心里话）。
- ⚠️ 气泡控件高度不要改（底板按此高度画，改高气泡视觉变大）。

## 帧率架构（2026-11-16 反汇编定稿，别再试「逻辑/渲染解耦」）
- 引擎**没有独立 Render()**。链：`nativeProcess`(0x25ae00) → `jProcessEvent` +
  `TM_Run`(0x25e664) + `ThreadProc` → 定时器到期回调 `MainProcess`(0x169494)
  → 场景绘制 → `grpFlushLcd` → `requestRender` → 我们 `egl_swap()`。
  **1 tick = 1 帧**，逻辑与绘制 1:1 绑定。
- 引擎原生就有真实时间固定步长：`MainProcess` 结尾按 `APPINFO_nTimer`
  （全局 u16 @0x2b9cc8，appinfo.dat，运行时 60 ms）算 deadline 并
  `CS_knlSetTimer` 排下一帧；`CS_knlCurrentTime` → `gettimeofday`。
  移植层 60 Hz 循环（`budget=freq/60`）**只轮询定时器**，不驱动逻辑。
- 动画**按帧计数、无时间插值**（淡入 alpha 每帧固定 -0.067）。
  ⇒ 在两个 tick 之间重复绘制只会得到相同画面，不会「视觉平滑化」；
  改快 tick 则游戏整体变快（60fps ≈ 3.5x，30fps ≈ 1.8x）。
- 11-14 已实测过 `frame_ms` 覆写 APPINFO_nTimer：Eden loop 59/draw 19 →
  `frame_ms=16` 时 draw 59，用户确认 60 FPS，但随后**黑屏**并要求整体回退
  （`537ab67` revert → `2d0f130`，证据 `logs/perf-eden-16ms/`）。
  当前 src/ 里这些改动残留为 0，**不要再引入**。
- 现有 requestRender 间隔实测：平均 49.3ms / 中位 50ms / 3~95ms（143 样本）。
  低风险可做的是**稳定抖动**：60 Hz 轮询把 60ms 周期量化成 34/50/66ms 三档，
  把轮询改细（如 240 Hz 或按 deadline 精确 sleep）可消除忽快忽慢，不动游戏速度。

## 备份
- 远端 `BenAlan11/inotia4_nx`（PAT 有写权限）。**git push 本机不通**（代理只放行 api.github.com），
  用 `GH_TOKEN=... python tools/gh_push.py BenAlan11/inotia4_nx main`（Git Data API）。
  游戏本体/APK/存档/工具链不进仓库。
- 最新可恢复点：`snapshots/known-good-20261116-2122/`（NRO md5 `28c6ed88…`，
  build stamp 21:19:46，未重编译=21:20 部署到 Eden 且实机用户确认的那份），
  提交 `4c60784`，远端 head `4ed04f73`；上一恢复点 `known-good-20261115-1903`
  （`4f97e18f…`，`aae9082`，注意这版带着「技能页右侧按钮点不动」的回归）。
- 远端可能滞后：本机 `git push` 挂住（auth/代理），要用 GH_TOKEN 跑 gh_push.py。
  该脚本已是**增量**（比对远端树，只传差异 + base_tree 建树），2026-11-15 实测
  12 文件/14.9MB 只要 23 秒。索引已按 .gitignore 既定策略收敛到「当前+上一代」
  （`git rm --cached` 旧代，本地文件不删），不要再往库里囤第 3 代以前的大文件。
- `snapshots/sdcard-before-*/` 是推卡前的旧 nro 备份，已忽略，不进库。
- `debug.log` 启动时轮转，grep 要先限定「本轮新增日志」再判断。
