# HP232X 测试方法论

## 测试环境

### 硬件环境
- **测试服务器**: 192.168.49.81 (lynxi/1)
- **设备连接**: 串口 `/dev/ttyUSB0` @ 115200 baud
- **存储映射**:
  - 本地: `/work/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin`
  - 远程: `/mnt/49.20/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin`
  - 固件链接: `/lib/firmware/lyn_drv/boot-wrapper.bin`

## 自动化测试

### 标准测试流程
```bash
# 1. 本地编译
cd /work/rt-thread/bsp/lynxi/hp232x
rm -rf build && scons -j$(nproc)

# 2. 生成 PCIe Boot 固件
python3 mkimage.py rtthread.bin rtthread-header.bin

# 3. 运行远程测试
python3 remote_test.py
```

### remote_test.py 功能
1. 连接测试服务器 (SSH + 密码)
2. 验证固件文件和软链接状态
3. 清理串口占用进程
4. 后台启动串口监控 (20 秒超时)
5. 执行设备复位 (`sudo lynd_hp run -d 0 -r wdt -o5`)
6. 捕获串口输出 (实时打印)
7. 分析启动标志 (Banner, Shell, ERROR 等)

## 测试验证要点

### 启动标志检测
- ✅ Banner 显示: `RT-Thread Operating System`
- ��� GIC Monitor 线程: `[GIC] #N Tick=N ISR=N`
- ✅ pmon 线程: `X` (Started) + `.` (每 5 秒)

### 崩溃标志检测
- ❌ `0x23232323...` — 栈溢出 (LR 链被破坏)
- ❌ `0x100100000` — 非法内存访问 (BSS 未清零/堆越界)
- ❌ `PC alignment fault` — 栈帧损坏

### 调试标记解读
```
ECO POK!                    ← pre_entry.S EL3→EL2→EL1 降级
???E SCGIDXNARBMN234INSPE12MCHLOWIBK  ← HP232X 初始化标记（pre_entry.S + entry_point.S）
BOOT                        ← rt_hw_board_init 开始（board.c:305）
[board] IRAM1: ...          ← MMU 启用后日志
RT-Thread banner            ← rt_show_version()
[GIC Monitor] Thread created ← pmon_gic_init()
Hi, this is RT-Thread!      ← main()
X                           ← pmon_gic_thread started
. . . . . .                 ← pmon 线程持续运行
```

## 手动调试方法

### 编译验证
```bash
# 检查段大小
aarch64-none-elf-size rtthread.elf

# 检查段布局
aarch64-none-elf-objdump -h rtthread.elf

# 验证 header
hexdump rtthread-header.bin | head -3
# 预期: offset 0x1C = 0x20 (32 字节)
```

### SSH 连接
```bash
ssh lynxi@192.168.49.81
# 密码: 1

# 检查固件链接
ls -l /lib/firmware/lyn_drv/boot-wrapper.bin

# 执行复位
sudo lynd_hp run -d 0 -r wdt -o5
```

### 串口监控
```bash
# 使用 minicom
minicom -D /dev/ttyUSB0 -b 115200

# 或使用 screen
screen /dev/ttyUSB0 115200
```

## 常见问题排查

### 1. 编译失败
**检查**：工具链路径、SCons 配置
**解决**：`which aarch64-none-elf-gcc`

### 2. SSH 连接超时
**检查**：网络连接、SSH 服务状态
**解决**：`ping 192.168.49.81`

### 3. 串口无输出
**检查**：串口设备路径、权限
**解决**：`sudo pkill -9 -f 'ttyUSB'`

### 4. 固件大小超出
**检查**：rtconfig.h 禁用不必要组件
**解决**：禁用 DFS/POSIX/LWIP

### 5. 启动崩溃 `0x23232323...`
**根因**：IRAM0 临时栈太小（4KB），`rt_hw_board_init` 栈溢出
**解决**：pre_entry.S 中 `hp232x_temp_stack` 从 4KB 扩大到 16KB

### 6. 启动崩溃 `0x100100000`
**根因**：BSS 清零在 MMU 启用前执行，堆地址超出 IRAM1 物理范围
**解决**：见 HANDOFF_SLIM.md "重大修复" 章节

## 测试环境依赖

### Python 依赖
```bash
pip3 install pexpect paramiko
```

### 串口访问权限
```bash
# 清理占用进程
sudo pkill -9 -f 'ttyUSB0'
```

## 验证标准

### 成功标准
- ✅ Banner 完整显示
- ✅ pmon 线程创建成功 (`[GIC Monitor] Thread created`)
- ✅ pmon 线程持续运行 (`X` + `.` 标记)
- ✅ 无崩溃标记 (`0x2323...`, `0x100100000`)
- ✅ 内存约束满足（IRAM0≤256KB, IRAM1≤256KB）
- ✅ 60 秒长测无异常

### 当前状态 (2026-07-03)
- ✅ BL21 模式：earlycon_base 初始化修复，但 bootwrapper 版本不匹配导致 ECO/POK 后无输出
- ❌ BL22 模式：启动到 `ABCDEFGH`（MMU 页表配置完成），在启用 MMU 时挂死
- ⚠️ 需要 JTAG 调试 ESR_EL1/FAR_EL1 确定 MMU 启用失败的根因

### BL22 调试状态 (2026-07-03)
- **启动链**：bootcode → bootwrapper(pre_entry.S) → kernel(entry_point.S) → board.c
- **当前输出**：`IBKSUE` → `BOOT` → `ABCDEFGH` → **挂死**
- **已修复**：
  1. `earlycon_base` 从 .bss(IRAM1) 移到 .data(IRAM0) — 解决 BSS 未清零
  2. `rt_hw_earlycon_ioremap_early()` 移到 `rt_hw_board_init()` 顶部 — 解决 earlycon_base=0
  3. `MMU_TYPE_BLOCK = 0` — 修正 ARMv8 块描述符类型
  4. `MMU_MAP_K_RWCB_FIX` AP=1 — 修正权限（原 AP=0 = No access）
  5. TCR_EL1 T0SZ=16 — 修正 VA 大小为 48-bit
  6. 页表 NS=1 — 修正 Non-Secure 标记
  7. `remote_test.py` 从 header 读取 dest_addr 并正确对齐 spl_address
- **挂死原因**：`msr sctlr_el1, %0` 启用 MMU 后，CPU 取指失败。页表映射经分析应该正确（PMD[33] 映射 VA 0x04040000→PA 0x04040000 Normal+R/W+Executable），但实际取指异常。
- **下一步**：JTAG 调试查看 ESR_EL1（异常类型）和 FAR_EL1（故障地址）

## 参考资源
- [HANDOFF_SLIM.md](HANDOFF_SLIM.md) - 项目进展和修复记录
- [doc/](doc/) - 技术知识点文档
- [memory/](../../memory/) - 调试过程详细记录
