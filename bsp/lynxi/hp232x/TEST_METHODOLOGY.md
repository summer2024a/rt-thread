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

# 2. 生成PCIe Boot固件
python3 mkimage.py rtthread.bin rtthread-header.bin

# 3. 运行远程测试
python3 remote_test.py
```

### remote_test.py功能
1. 连接测试服务器 (SSH + 密码)
2. 验证固件文件和软链接状态
3. 清理串口占用进程
4. 后台启动串口监控 (20秒超时)
5. 执行设备复位 (`sudo lynd_hp run -d 0 -r wdt -o5`)
6. 捕获串口输出 (实时打印)
7. 分析启动标志 (Banner, Shell, ERROR等)

## 测试验证要点

### 启动标志检测
- ✅ Banner显示: `RT-Thread Operating System`
- ✅ GIC初始化: `GICD_CTLR: 0x12`
- ⚠️ Shell提示: `msh />`

### 调试标记解读
```
???E3SCGIDXNARBMN234NSPE12MCIBK
  E3: CurrentEL=3确认 ✅
  SC: SCR_EL3配置前后 ✅
  GID: GIC Distributor初始化 ✅
  XNARBM: GIC配置标记 ✅
  234N: Redistributor唤醒 ✅
  SPE12MC: ICC系统寄存器配置 ✅
  IBK: entry_point.S标记 ✅
```

## 手动调试方法

### 编译验证
```bash
# 检查段大小
aarch64-none-elf-size rtthread.elf

# 检查段布局
aarch64-none-elf-objdump -h rtthread.elf

# 验证header
hexdump rtthread-header.bin | head -3
# 预期: offset 0x1C = 0x20 (32字节)
```

### SSH连接
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
# 使用minicom
minicom -D /dev/ttyUSB0 -b 115200

# 或使用screen
screen /dev/ttyUSB0 115200
```

## 常见问题排查

### 1. 编译失败
**检查**：工具链路径、SCons配置
**解决**：`which aarch64-none-elf-gcc`

### 2. SSH连接超时
**检查**：网络连接、SSH服务状态
**解决**：`ping 192.168.49.81`

### 3. 串口无输出
**检查**：串口设备路径、权限
**解决**：`sudo usermod -aG dialout $USER`

### 4. 固件大小超出
**检查**：rtconfig.h禁用不必要组件
**解决**：禁用DFS/POSIX/LWIP

## 测试环境依赖

### Python依赖
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
- ✅ Banner完整显示
- ✅ 无异常标记（无'??'异常）
- ✅ 内存约束满足（IRAM0≤256KB, IRAM1≤256KB）

### 当前状态
- ✅ Banner显示成功
- ⚠️ Shell未显示（待调试）

## 参考资源

- [README.md](README.md) - BSP概述
- [HANDOFF_SLIM.md](HANDOFF_SLIM.md) - 项目进展
- [doc/](doc/) - 技术知识点文档