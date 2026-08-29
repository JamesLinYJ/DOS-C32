<!-- SPDX-License-Identifier: GPL-2.0-only -->

[English](README.md)

# DOS-C32

DOS-C32 是一个正在开发中的 32 位操作系统，通过受保护的 i386 内核提供
MS-DOS 兼容环境。它从传统 BIOS 启动，并在类型明确的执行边界内运行未经
修改的 16 位 DOS COM/MZ 程序。UEFI 和针对特定应用程序的兼容性特殊处理
不在项目范围内。

本仓库采用 `GPL-2.0-only` 许可证。Microsoft Diagnostics、Windows 文件和
其他专有测试材料均不包含在仓库中。

## 当前状态

系统镜像可以通过 SeaBIOS 启动，进入受保护的 32 位 i386 内核，挂载自身的
FAT16 卷并启动内置命令环境。启动测试也会执行正式的 VM86 服务路径。目前
已经实现类型明确的 DOS ABI、内存管理、PSP/JFT/SFT、COM/MZ 加载与重定位、
EXEC0/EXEC1 准备流程，以及部分 INT 21h 服务；这些功能均有主机测试和启动
测试覆盖。

当前版本还不能完全替代 DOS。正式命令环境尚未覆盖全部 COMMAND 内置命令
和完整的 DOS 程序生命周期；部分 DOS 与 BIOS 服务、IRQ/设备支持、TSR
行为、EMS/VCPI，以及 Windows 3.2 的保护模式执行仍未完成。真实 COM/MZ
程序已经使用正式的 EXEC 和 x86 应用执行路径，不再依赖仅供测试使用的
加载器。

基于测试证据的完成情况记录在
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)，有序开发计划见
[PLAN.md](PLAN.md)。

## 构建与验证

所需工具包括支持 32 位目标的 GCC、GNU binutils、`dosfstools`、`mtools`，
以及 QEMU 的 i386 系统模拟器。

```sh
make image
make check
make run
```

在 macOS 上，QEMU 默认使用可见窗口启动并分配 256 MiB 内存。可以在不修改
内核代码的情况下调整内存，例如执行 `QEMU_MEMORY_MIB=64 make run`。配置的
早期映射范围只是容量上限；每次启动都会根据经过验证的 BIOS E820 内存图，
确定实际映射高水位、可用页面池、保留区间和 XMS 报告值。

输出镜像位于 `build/msdos-c32.img`。`make check` 还会分别以 32 位和 64 位
内部数据模型编译可移植边界，检查禁用 API 和针对验收程序的特征判断，验证
FAT16 镜像，并在 QEMU 中启动隔离的自检镜像。

如需代码级调试：

```sh
make run-gdb
# 在另一个终端中执行：
gdb -x debug/kernel.gdb
```

日志和断点说明见 [docs/DEBUGGING.md](docs/DEBUGGING.md)。

## 设计规则

- MS-DOS 程序可见行为是兼容性契约。内部设计不得替换其 ABI 或错误语义。
- 应用指针始终使用明确的 16:16 地址或固定宽度的应用线性地址。内核身份、
  偏移量和持久句柄使用 64 位值；原生指针绝不进入 DOS ABI 结构。
- x86 应用的硬件访问按资源授权。共享或危险 I/O 必须经过中介处理，程序名
  不能扩大访问权限。
- 内部虚拟机管理器对普通 DOS 和 Windows 软件透明。它不会增加私有的应用
  可见 ABI；普通用户提示只描述应用程序和系统保护，不暴露虚拟化后端。
- 信任边界必须使用有界 API、溢出检查、不可变身份、代次句柄、可逆准备流程
  和失败后保持关闭的中毒状态。
