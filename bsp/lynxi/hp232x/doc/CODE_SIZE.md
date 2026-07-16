# HP232x `.text` 体积拆分（~243 KB）

> 基准：`scons` 产物 `rtthread.elf` / `rtthread.map`（2026-07-16）。  
> 链接脚本把 **`.text` + `.rodata*`** 合进同一输出段 `.text`（见 `link.lds`），故「代码段 243 KB」含只读数据（字符串、CRC 表、FINSH 符号说明等）。

```
.text @ 0x04040800   249216 B = 243.38 KB
.data                  3032 B =   2.96 KB
.head                   824 B =   0.80 KB
IRAM0 文件合计 ≈ 247 KB（预算 256 KB）
```

复查：

```bash
cd bsp/lynxi/hp232x && scons -j$(nproc)
# Section Analysis 中的 .text；或：
aarch64-none-elf-size -A rtthread.elf | grep '\.text'
```

---

## 1. 按组件占比

| 部分 | 约 KB | 约占 `.text` | 说明 |
|------|------:|-------------:|------|
| **Newlib / libgcc** | **58.0** | **23.8%** | 最大头；浮点 `printf`/`strtod`/`dtoa` 等 |
| **BSP drivers** | **41.7** | **17.1%** | eMMC + Flash + board/MMU + I2C… |
| **RTT kernel (src+klibc)** | **41.2** | **16.9%** | 调度/IPC/线程 + `rt_vsnprintf_std` |
| **biz** | **27.8** | **11.4%** | Host 任务循环 / exec / finsh / log |
| **RTT libcpu aarch64** | **24.2** | **10.0%** | GIC/MMU/exception/vector |
| **RTT mm** | **20.9** | **8.6%** | 主要是 `mm_aspace` + `mm_page` |
| **RTT finsh/msh** | **10.6** | **4.3%** | shell + 内建 cmd |
| **RTT device framework** | **6.3** | **2.6%** | serial / ipc / smp_call |
| **applications** | **4.7** | **1.9%** | `main` / `cmd` / pmon |
| **RTT utilities** | **4.4** | **1.8%** | Ymodem 等 |
| **RTT libc glue** | **2.0** | **0.8%** | newlib syscalls 等 |
| ***fill* / 对齐** | **~2.1** | **0.9%** | |

**归并视角**

| 归并 | 约 KB | 占比 |
|------|------:|-----:|
| RTT 内核树（src+libcpu+mm+finsh+drivers+libc+util） | ~110 | ~45% |
| Newlib/libgcc | ~58 | ~24% |
| 本板 BSP drivers + biz + app | ~74 | ~30% |

---

## 2. BSP drivers（~42 KB）

| 文件 / 组 | 约 KB |
|-----------|------:|
| `drv_emmc_core.o` | 12.3 |
| `drv_flash.o` | 11.5 |
| board + `hp232x_mmu` | 7.8 |
| PVT / APU / efuse | 5.0 |
| I2C + GPIO MCU | 3.6 |
| UART / timer / clk | 1.5 |

未链入（宏关闭）：`drv_pcie`、`utilities/zmodem`。

---

## 3. biz（~28 KB）

| 文件 / 组 | 约 KB |
|-----------|------:|
| `biz_emmc_exec` + `biz_emmc_biz` | 10.0 |
| `biz_finsh_cmds` | 5.6 |
| exec 模块（apu/misc/stress/selftest/dump） | 3.7 |
| `biz_subsys` | 3.3 |
| `biz_log` + `biz_i2c_proxy` + ipc/config | 4.0 |
| `biz_crc32`（含 256 字节表 → rodata） | 1.2 |

---

## 4. Newlib 大头（~58 KB，裁剪优先）

| 对象 | 约 B | 备注 |
|------|-----:|------|
| `svfprintf` / `vfiprintf` | ~20K | 格式化输出 |
| `strtod` / `strtodg` / `dtoa` / `mprec` | ~26K | **浮点转换** |
| 其余 string/stdio/libgcc | ~12K | |

`RT_KLIBC_USING_VSNPRINTF_STANDARD` 已用 RTT 自有 `rt_vsnprintf`，但 newlib 仍因其它引用（如 `sprintf`/`strtoul`/stdio）拉入浮点路径。后续若要挤 IRAM0，优先：避免拉 `printf` 浮点、或改用 nano/newlib-nano、或砍掉未用 stdio。

---

## 5. RTT 内核大对象（参考）

| 对象 | 约 KB |
|------|------:|
| `mm_aspace.o` | 14.0 |
| `rt_vsnprintf_std.o` | 8.3 |
| `ipc.o` | 7.0 |
| `finsh/cmd.o` | 5.9 |
| `gicv3.o` | 5.7 |
| `mm_page.o` | 5.2 |
| `rt_vsscanf.o` | 4.5 |
| `ymodem.o` | 3.6 |

`BSP_HP232X_MM_MINIMAL` 已去掉部分 MM；`mm_aspace` 仍是最大单文件之一。

---

## 6. 与镜像约束的关系

| 项 | 值 |
|----|-----|
| IRAM0 预算 | 256 KB（BL22 后半） |
| 当前 `.head+.text+.data` | ~247 KB |
| **余量** | **~9 KB** |
| IRAM1 BSS | ~105 KB / 256 KB |

体积敏感改动（CRC 表、FINSH 命令、ymodem、newlib 引用）需同步看 Section Analysis，避免顶满 IRAM0。
