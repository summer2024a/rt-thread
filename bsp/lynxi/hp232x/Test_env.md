# 测试环境

## 硬件环境
- 测试服务器192.168.49.81.帐号lynxi，密码1，具备sudo权限。
- 设备作为测试服务器上的ep设备，其串口接到测试服务器上，其设备为/dev/ttyUSB0，波特率115200。
- 当前rt-thread工程已经通过smb挂载到测试服务器上目录为/mnt/49.20/rt-thread。固件存放在/mnt/49.20/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin，设备升级的固件通过软链接的方式挂载到测试服务器上目录为/mnt/49.20/rt-thread/bsp/lynxi/hp232x/boot-wrapper.bin。如下
```
root@lynxi:/lib/firmware/lyn_drv# ls -l boot-wrapper.bin
lrwxrwxrwx 1 root root 57 6月  18 01:19 boot-wrapper.bin -> /mnt/49.20/rt-thread/bsp/lynxi/hp232x/rtthread-header.bin
```
- 固件升级命令为lynd_hp run -d 0 -r wdt -o5，需要sudo权限在测试服务器上执行。在当前vscode环境编译后在测试服务器上执行复位，并可完成固件的更行。

## 测试步骤
- 在vscode ide上编译
- 通过ssh协议连接测试服务器，一个线程读取串口数据，监控输出数据，另一个线程执行复位确保固件更新。
- 通过监控的串口信息，对比修改，再次分析修改后编译，循环测试，直到msh的shell启动

## 测试任务
- 整理测试方法，并更新为skill保存
- 在rtthread开始阶段需要按照bootwrap的执行流程完成el3切换到el2
- 完成多核，支持gic，多线程，启动到shell。
- 源码修改不要影响其他板卡的编译，如he200的编译
- iram0 后256KB不能使用， iram1前256kb不能使用
- 调试 rt_page_init MPR size 问题 (size值20000000000)
- 调试 rt_hw_mmu_setup 异常
