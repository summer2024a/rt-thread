# HP232x 镜像组包与启动 / 升级路径

> 说明 **BL1 如何把系统带到当前固件**、与 hp640 Flash 冷启动链的差异、两种组包格式及制作方法。  
> 参考树：`lynxi-bootrom`、`lynxi-bootwrapper`、`hp640_arm`（本机常见路径 `/work/lynxi-bootrom`、`/work/lynxi-bootwrapper`、`/work/lynxlink/hp640_arm`；亦可为 `/data/biao.xia/...`）。  
> 本组：[`mkimage.py`](../mkimage.py)、[`scripts/mk_ka200_image.py`](../scripts/mk_ka200_image.py)、[`FLASH_PORTING.md`](FLASH_PORTING.md)、[`Test_ENV.md`](../Test_ENV.md)。
>
> **更新日期**：2026-07-14（Host 组包对齐 hp640_bl jumper；version / repack 澄清）

---

## 1. 级联命名（产品级 BL，不是 TF-A BL2）

`lynxi-bootrom` 是 **单镜像 BL1**（项目内 TF-A BL2 构建已关掉）。之后用 **BL21 / BL22 / BL23** 描述 IRAM / Flash 上的角色：

| 级 | 二进制 / 工程 | 角色 | 典型入口 |
|----|---------------|------|----------|
| **BL1** | `bl1.bin` — **lynxi-bootrom** | ROM：PLL/cache、读 `BOOT_SELECT`、按设备加载镜像、按 header 跳转 | 运行在 ROM；固件落点 IRAM0 `0x04000000` |
| **BL21** | `boot-wrapper.bin` — **lynxi-bootwrapper** | EL3 初始化、GIC/CCI；主核 `br spl_address`；从核 WFE 轮询 mbox `0x0401FFF0`（7 槽×8） | 落点 `0x04000000`，跳入 `+headersize` → **`0x04000020`** |
| **BL22** | **Flash**：`hp640_bl`（`CONFIG_HP640_JUMPER`）<br>**UART**：本 BSP 的 **RT-Thread**（或 hp640 非 jumper SPL） | 第二级 IRAM 业务 / 跳板 | RTT / 多数 SPL：**`0x04040020`**<br>jumper 文本基可 `0x04020020` |
| **BL23** | **hp640** 完整业务 SPL 或 **本 BSP RTT**（Host 升级后冷启） | 仅 **Flash 冷启动** 由 jumper 从 SPI 再拉起 | 拷到 `0x04040000`，跑 **`0x04040020`** |

> **UART 启机在 BL22 结束**（BL22 = RTT），**没有 BL23**。  
> **Flash 产品链**：BL22=`hp640_bl` → BL23=`hp640` **或** Host 升级后的 **RTT**（组包须对齐 jumper，见 §4.2 / §7）。

IRAM0 两半（与 `board.h` / UARTSETUP_BL22 一致）：

| 区间 | 地址 | 谁占用 |
|------|------|--------|
| 低 256KB | `0x04000000`–`0x0403FFFF` | BL21 boot-wrapper；第二次 UART 文件基址常落在 `0x04020000` |
| 高 256KB | `0x04040000`–`0x0407FFFF` | BL22 RTT / BL23；入口 **`0x04040020`** |

`BOOT_SELECT[31:30]`（`0x12500064`，见 bootrom `doc/boot_sel.md`）：

| 值 | 设备 |
|----|------|
| 0 | SPI NOR |
| 1 | eMMC |
| 2 | PCIe |
| 3 | **UART** 下载（调试，打印 `.U`） |

魔数未识别时，SPI 也会掉进 UART debug 模式。

---

## 2. BL1 如何启动到固件：两条冷启动链

### 2.1 Flash 冷启动（BL1 → BL21 → BL22 hp640_bl → BL23）

```
复位
  → BL1 (lynxi-bootrom)
       BOOT_SELECT = SPI
       探测 Flash 头魔数（scratch 常用 IRAM1 @0x100000000）
       MAGIC_LXKJ → boot_wrapper：按多文件头链式加载到 IRAM
         · 第 1 份 → 0x04000000   (= BL21 boot-wrapper)
         · 后续份按 dest（常 +0x20000 → 0x04020000）(= BL22 hp640_bl)
         · MAGIC_END 结束
       next_entry: ldrh [0x04000000+0x1C] → 跳 BL21 @0x04000020
  → BL21 (lynxi-bootwrapper)
       初始化后 br spl_address（config.S 默认 0x04040020；
       jumper 场景下 spl 可先落在 0x04020020）
  → BL22 hp640_bl  (CONFIG_HP640_JUMPER, lynxi_640_read)
       优先读升级区 Head640 @Flash 0xA6000，body @0xA7000
       非法/MD5 失败 → 工厂区 @0x64000/@0x65000
       拷到 IRAM 0x04040000，校验后跳 0x04040020
  → BL23 hp640 或 RTT（Host 升级写入的 body）
```

Flash 上与启动相关的布局（jumper，摘自 `hp640_arm/common/spl/spl.c`）：

| Flash | 含义 |
|-------|------|
| `[0, 0x64000)` | BL1 链式 / XIP bootcode（写保护，与 RTT `DRV` 保护一致） |
| `0x64000` / `0x65000` | 工厂 Head640 + body |
| `0xA6000` / `0xA7000` | **升级** Head640 区 + body（≤256KB）→ BL23 |

**两种「写升级区」路径不要混：**

| 路径 | 谁写 | `@0xA6000` | `@0xA7000`（body） |
|------|------|------------|-------------------|
| hp640 `save_update_code()` | SPL 本地 | 仅 `Head640{size,md5}`（20B） | **裸** SPL / 固件（**无 JKXL**） |
| Host `ka200_tools -u` | Tool + 运行中的 KA200 FlashWrite | `size+md5` + **pad 到 4K**（repack） | **整份组包文件**（含 JKXL + payload） |

`A6000 → A7000` 本身就是 **0x1000**：Host 路径用 repack 的 4K 头把 body 顶到 `@0xA7000`。  
**4K padding 是 `ka200_tools` 做的，不是 `mk_ka200_image.py`。**

jumper 核心逻辑（`lynxi_640_read`）：

```
读 Head640 @0xA6000 → size / md5
memcpy(body @0xA7000 → IRAM 0x04040000, len=size)
MD5 校验通过 → 跳 (dst + 0x20) = 0x04040020
```

> **Boot SSI / XIP**：`lynxi-bootwrapper` 不写 `0x12600024`。RTT 对齐 **lynxi-linux `lyn-sfc`** / **lynxi-uboot `designware_spi`**：`entry_point.S` 写 `0x98000000`（清 XIP），`drv_flash` 按 `BOOT_SELECT[29:28]` 选窗（勿改 640 jumper）。

因此：**body 文件偏移 `0x20` 起必须是链接入口代码**（`rtthread.bin[0]`，VMA `0x04040020`）。

参考：

- bootrom：`doc/boot_sel.md`、`bl1/aarch64/bl1_entrypoint.S`
- bootwrapper：`config.S`（`spl_address = 0x04040020`）
- hp640：`configs/hp640_jumper_defconfig`、`common/spl/spl.c` → `lynxi_640_read()`

### 2.2 UART 启动（当前默认开发：BL1 → BL21 → BL22 RTT）

```
复位 / BOOT_SELECT=UART 或 SPI 魔数未命中
  → BL1 进入 boot_debug_mode（串口 .U）
  → 第 1 次 Xmodem → IRAM 0x04000000 : boot-wrapper.bin     (= BL21)
  → 第 2 次 Xmodem → IRAM 0x04020000 : rtthread-header.bin
  → BL1 next_entry → BL21 @0x04000020
  → BL21 br spl_address → 0x04040020
  → BL22 = RT-Thread（本固件）
       无 BL23：RTT 即最终运行体
```

| | Flash（产品） | UART（本 RTT 默认） |
|--|----------------|---------------------|
| BL1 | bootrom | 同左 |
| BL21 | boot-wrapper | boot-wrapper（第 1 次 Xmodem） |
| BL22 | **hp640_bl**（跳板） | **RT-Thread** |
| BL23 | hp640 / Host 升级后的 RTT | **无** |

第二次文件用 `mkimage.py`（UARTSETUP）打成「头在 `0x04020000`、payload 在 `0x04040020`」的**带空洞**镜像（见 §4.1）。

### 2.3 当前 BSP 所处位置

| 能力 | 状态 |
|------|------|
| UART：BL1→BL21→**BL22 RTT** | ✅ 开发默认（`BSP_USING_HP232X_UARTSETUP_BL22`） |
| Host 在线升级写 Flash `@0xA6000` | ✅ Load + FlashWrite（见 FLASH_PORTING） |
| Flash 冷启：已有板卡上的 **hp640_bl** + Host 包作 BL23=RTT | ✅ **组包已对齐 jumper**（§4.2；须用新 `mk_ka200`，勿用旧 64B 头） |
| 本 BSP 内自建 jumper / 替代 BL22 | ❌ 未移植；Flash 链仍依赖板内 hp640_bl |

开发机日常仍以 UART 起机为主；**从老 hp640 在线升级到 RTT 后冷启**，依赖 §4.2 / §7。

---

## 3. 运行中的两条「升级/分发」路径（相对冷启动）

共用同一 `rtthread.bin`，组包不同。

| | **路径 A：串口 Xmodem 启机** | **路径 B：Host 在线升级** |
|--|--|--|
| **目的** | 走 §2.2，把 RTT 载入 IRAM 运行 | 写 SPI 升级区，供 **hp640_bl 冷启** |
| **组包** | `mkimage.py` → `rtthread-header.bin` | `mk_ka200_image.py` → `HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin` |
| **Header** | **32B**（BL1 布局 + UART 空洞） | **32B JKXL**（连续紧挨 payload，见 §4.2） |
| **前提** | 复位出 `.U` | 已有可跑的 BL22（hp640 或 RTT）+ heartbeat + 拓扑 |
| **消费** | BL1→BL21→BL22 RTT | `ka200_tools` repack → KA200 FlashWrite `@0xA6000` |

```
                    scons
                      │
                      ▼
                rtthread.bin  ←── 纯 payload（链接 @0x04040020）
                   │         │
        ┌──────────┘         └──────────────┐
        ▼                                   ▼
  mkimage.py（UARTSETUP）            mk_ka200_image.py（scons PostAction）
  rtthread-header.bin                HP232x_KA200_Serdes_Update_*_vX.Y.bin
  [32B][pad 0x20000][payload]        [32B JKXL][payload]  ← 无空洞
        │                                   │
        ▼                                   ▼
  UART Xmodem 第 2 次                 ka200_tools -u
                                       → repack: [size+md5|pad 4K][整文件]
                                       → FlashWrite @0xA6000
```

---

## 4. 组包格式对比

两套都用 **`0x4C584B4A`（JKXL）** + **`0x4A554D50`（PMUJ）**，布局不同，**不可混用**。  
Flash 上 jumper 认的 **Head640（size+MD5）** 又是第三种头（由 `ka200_tools` / `save_update_code` 写入）。

### 4.1 路径 A / UART：`mkimage.py` — 32B + 空洞

供 **BL1**；`0x1C` 为 **headersize**（`0x20`）。UARTSETUP 用 dest/next 在文件内插 padding。

```
Offset  Size  字段
0x00    4     MAGIC
0x04    4     FLAG
0x08    4     dest_addr low
0x0C    4     dest_addr high (=0)
0x10    4     file_size
0x14    4     next_offset
0x18    4     pad
0x1C    4     headersize (=0x20)   ← BL1 读这里
── file padding（UARTSETUP：0x20000）──
── rtthread.bin ──
```

| 参数 | 值 | 含义 |
|------|-----|------|
| `--dest-addr` | `0x04020000` | 第二次 Xmodem 落点 |
| `--next-offset` | `0x20020` | dest+next → **`0x04040020`** |
| 文件布局 | `[32B][pad 0x20000][payload]` | **不可**给 `ka200_tools -u` |

> **非 UART 的 `BSP_USING_HP232X_BL22`**：`dest=0x04040000`、`next=0` → **无空洞** `[32B][payload]`。几何上与 Host 包一致（入口在 `+0x20`），但仍是 BL1 头字段布局；Host 交付请用 `mk_ka200_image.py`（文件名/version/工具魔数约定）。

### 4.2 路径 B / Host：`mk_ka200_image.py` — **32B 连续**（对齐 jumper）

给 **`ka200_tools -u`**；校验魔数后 **repack**，再 FlashWrite。

```
Offset  Size  字段（小端，定长 0x20）
0x00    4     MAGIC (JKXL)
0x04    4     FLAG (PMUJ)
0x08    8     dest_addr（lo/hi，工具链字段；jumper 不读）
0x10    4     payload_size（rtthread.bin 长度）
0x14    4     next_offset
0x18    4     END_FLAG (!END)
0x1C    2     headersize (=0x20)
0x1E    2     version（如 0x0500 → v5.0）  ← 紧贴入口，供运行期心跳读取
── 无空洞 ──
0x20 …        rtthread.bin     ← jumper 跳 0x04040020 落在这里
```

合计 ≤256KB。默认 `VERSION=0x0500`，输出名 `HP232x_KA200_Serdes_Update_YYYYMMDD_v5.0.bin`。

#### 为何必须是 0x20 而不是旧 addheader 的 0x40

| 组包 | body `@0xA7000` 拷到 `0x04040000` 后 | jumper 入口 `+0x20` |
|------|--------------------------------------|---------------------|
| **旧 64B JKXL** | `0x04040020` = 头内 padding（全 0） | **挂死**（拷贝日志有、RTT 不起来） |
| **现 32B JKXL** | `0x04040020` = `rtthread.bin[0]` | ✅ |

旧 hp640 `addheader.py` 也是 64B + bin；Host `-u` 冷启同样有该风险。hp640 **本地** `save_update_code()` 写的是裸 body，无此问题。

#### `ka200_tools` repack（落盘后的真实 Flash 布局）

```
@0xA6000  [size 4B][md5 16B][pad → 4KB]     ← Head640 语义（size=组包文件总长）
@0xA7000  [32B JKXL][rtthread.bin…]         ← body = 组包文件全文
```

- Tool **只检查** 文件头魔数 `0x4C584B4A`，对整文件算 MD5；**不解析** dest / next / version。  
- 4K 头由 tool 添加；组包脚本 **不要**再造一层 4K。

### 4.3 对照

| 项 | `mkimage` UARTSETUP | `mk_ka200_image` | `mkimage` BL22 无空洞 | Head640 |
|----|---------------------|------------------|----------------------|---------|
| 用途 | UART 第二文件 | Host `-u` + 冷启 body | 概念对照 | jumper 校验 |
| 头长 | 32B + **128KB pad** | **32B 连续** | 32B 连续 | size+MD5（20B） |
| 谁解析 | BL1 | tool 只认魔数；jumper 认 Head640+body | BL1 | hp640_bl |
| 入口对齐 | pad 把 payload 放到 `0x04040020` | body`[+0x20]`=payload | 同左几何 | 拷到 `0x04040000` 后 `+0x20` |

---

## 5. 版本号：组包头 vs `ka200_tools` vs 运行时

| 环节 | 是否使用 header `version`（u16 @+0x1E） |
|------|----------------------------------------|
| 文件名 `..._v5.0.bin` | 组包脚本按 version 生成，给人看 |
| **`ka200_tools -u`** | **不用**（只透传整文件） |
| jumper / MD5 | **不用**（只用 Head640 size+md5） |
| RTT 心跳 `HP640_HeartBeatPackage.version` | **读内存** `*(KERNEL_VADDR_START - 2)` = `0x0404001E` |
| MSH `ver` | 写死字符串，与组包 version **无关** |

心跳代码（与 hp640 `CONFIG_SPL_TEXT_BASE - 2` 同惯例）：

```c
/* biz_emmc_biz.c */
heart_beat->version = *(unsigned short *)(uintptr_t)(KERNEL_VADDR_START - 2);
```

- **Flash 冷启 + 32B Host 包**：`0x0404001E` 恰为 header 的 version → Host 可收到 v5.0。  
- **UARTSETUP 启机**：`0x0404001E` 多半是空洞 padding → 心跳 version 常为 **0**（组包 version 未映射到入口前）。

---

## 6. 如何制作

### 6.1 编译（共用 payload）

```bash
cd /work/lynxlink/lynxi-rtt/bsp/lynxi/hp232x
scons -j$(nproc)
# → rtthread.bin / rtthread.elf
# → rtthread-header.bin          (mkimage / UARTSETUP)
# → HP232x_KA200_Serdes_Update_*_v5.0.bin  (mk_ka200，32B 头)
```

### 6.2 UART 启机（路径 A）

```bash
python3 mkimage.py rtthread.bin rtthread-header.bin \
  --dest-addr 0x04020000 --next-offset 0x20020 --elf rtthread.elf
# 测试机软链 → u-boot-spl.bin；另备 boot-wrapper.bin
```

### 6.3 Host 升级包（路径 B）

```bash
python3 scripts/mk_ka200_image.py rtthread.bin
# 确认：payload starts at file offset 0x20；hdr: 0x20
ka200_tools -u HP232x_KA200_Serdes_Update_YYYYMMDD_vX.Y.bin -l 0 -i 2 -k 30 -f
```

从 **老 hp640** 升级时：hp640 只做 FlashWrite，**不能**改 KA200 侧剥头逻辑，必须靠本组包几何正确，冷启才能进 RTT。

### 6.4 常见误用

| 错误 | 结果 |
|------|------|
| `rtthread-header.bin`（含 128KB 空洞）交给 `-u` | 体积/布局错误；jumper 入口不对 |
| **旧 64B** Host 包 | 拷到 IRAM 后 `+0x20` 为 0 → **挂死**（有 memcpy 日志无 RTT） |
| Host 包当 UART 第二文件 | BL1 布局不符 |
| 以为组包脚本负责 4K | 4K 仅 `ka200_tools` repack |

---

## 7. Flash 冷启进 RTT：问题回顾与判据

**现象**：Host 升 5.0 后复位，串口可见 `hp640_bl` / `lynxi_640_read` / `xip_memcpy ... len=232176`，之后无 RTT。

**根因**：body 为「64B JKXL + bin」时，`0x04040020` 落在 JKXL padding（全 0）。

**修复**：`mk_ka200_image.py` 改为 **32B 头 + 紧挨 `rtthread.bin`**（几何 ≡ BL22 无空洞 mkimage，交付形态仍用 Host 包）。

**验收**：冷启后有 RTT / heartbeat；可选确认心跳 version 为 `0x0500`（major=5）。

---

## 8. 相关文件与参考树

| 路径 | 作用 |
|------|------|
| **lynxi-bootrom** | BL1 设备选择、加载、`next_entry` |
| **lynxi-bootwrapper** | `spl_address=0x04040020` |
| **hp640_arm** `spl.c` | `lynxi_640_read` / Head640 `@0xA6000` |
| `mkimage.py` | UART /（可选）BL22 无空洞 32B |
| `scripts/mk_ka200_image.py` | Host 升级包：**32B JKXL + payload** |
| `ka200_tools` | `-u` / `repack_firmware`（4K Head640 + 整文件） |
| `biz/biz_emmc_biz.c` | 心跳 version ← `KERNEL_VADDR_START - 2` |
| [`FLASH_PORTING.md`](FLASH_PORTING.md) | FlashWrite / NC·WB |

---

**摘要**

- **UART 开发链**：BL1 → BL21 → **BL22 RTT**（`mkimage` 32B+**空洞**）。  
- **Flash 产品链**：BL1 → BL21 → **BL22 hp640_bl** → **BL23**（升级区 body）。  
- Host 落盘 = **`ka200_tools` 4K Head640** + **组包文件作 body**；组包须 **32B JKXL 紧接 payload**，勿用 64B 头。  
- **`ka200_tools` 不消费 version**；运行时心跳从入口前 2 字节读取（Flash 连续 32B 头时才等于组包 version）。
