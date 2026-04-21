# AUP-ZU3 资源最大化与高级功能学习测试指南

生成时间：2026-04-17（UTC）  
目标：在当前 AUP-ZU3 开发板环境下，给出可落地的“资源最大化利用 + 高级功能学习测试”路径。

## 1. 本次现场检查结论

### 1.1 你指定的路径执行结果
- 执行 `/home/petalinux/elec3607-lab/codex_test` 返回 `Is a directory`。
- 该路径当前是空目录（暂无可执行入口脚本）。

### 1.2 当前系统实测状态（板上）
- OS: `Linux 6.6.10-xilinx-v2024.1`（aarch64）
- CPU: `4x Cortex-A53`，最高频率 `1199999 kHz`
- 内存: `7.8 GiB`（可用约 `6.3 GiB`）
- 根分区: `/dev/root` 使用率约 `86%`（剩余约 `600 MB`）
- FPGA: `/sys/class/fpga_manager/fpga0` 存在，状态 `operating`
- UIO: `uio0..uio4`，其中 `uio0..uio3 = axi-pmon`，`uio4 = PWM`
- 可见设备节点: `i2c-0..4`、`mmcblk0`、`ttyPS0`、`/dev/dri/card0`、`/dev/mali`
- 网络: `eth0` 正常 UP
- 关键内核驱动迹象:
  - `xilinx-zynqmp-dpdma ... probed`
  - `zynqmp-display ... probed`
  - `fpga_manager fpga0 ... registered`

### 1.3 现场可用工具（节选）
- 已有: `python3`, `gcc`, `g++`, `make`, `i2cdetect`, `gst-launch-1.0`, `gst-inspect-1.0`
- 缺少: `perf`, `stress-ng`, `fio`, `iperf3`, `can-utils`, `v4l2-ctl`, `vivado`, `vitis`

---

## 2. AUP-ZU3 可重点榨取的资源（按价值排序）

依据官方资料，AUP-ZU3（XCZU3EG）可形成“异构协同”学习路线：
- PS 侧：`4x A53 + 2x R5F`，适合 Linux + 实时控制协同
- PL 侧：约 `154K logic cells`、`360 DSP slices`，适合加速器/信号处理
- 图形与多媒体：`Mali-400MP`、`DisplayPort`、音频编解码
- 存储与互联：最高 `8GB DDR4`、USB3、GbE、I2C/SPI/CAN（SoC能力）、MIPI 相机接口
- 板级扩展：Pmod、Grove、Raspberry Pi 接口、XADC

核心建议：
- 把 A53 用于“控制 + 调度 + 数据搬运”。
- 把 PL 用于“高吞吐、低时延内核”。
- 把 R5F 用于“强实时闭环任务”（后续通过 OpenAMP/remoteproc 接入）。
- 用 `axi-pmon` + DMA 跑量化指标，建立“优化前后可对比”的工程闭环。

---

## 3. 三层测试路线（从今天就能做，到系统级高级实验）

## 3.1 第 0 层：立即可跑（不改镜像）

### A. 资源基线快照（建议先做）
```sh
uname -a
lscpu
free -h
df -h /
ls /sys/class/fpga_manager
for i in /sys/class/uio/uio*; do echo "[$i] $(cat $i/name)"; done
i2cdetect -l
```
通过标准：以上命令稳定输出，作为后续优化对照基线。

### B. A53 多核计算上限探索
```sh
# 查看可调频点
cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies

# 固定到最高频（当前 governor=userspace）
echo 1199999 | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed

# 4 线程 openssl 压测（有 openssl 时）
openssl speed -multi 4 sha256
```
通过标准：频率可锁到高档位；4线程吞吐显著高于单线程。

### C. I2C/外设总线连通性
```sh
i2cdetect -l
# 示例：扫描某条总线（按实际连接选择）
sudo i2cdetect -y 0
sudo i2cdetect -y 1
```
通过标准：可稳定访问总线，不出现总线挂死。

### D. FPGA manager 基础可用性确认
```sh
cat /sys/class/fpga_manager/fpga0/name
cat /sys/class/fpga_manager/fpga0/state
```
通过标准：状态保持 `operating`，重启后仍正常。

### E. AXI PMON 可观测能力确认（关键）
当前发现 4 组 PMON：
- `uio0 addr=0xFFA00000`
- `uio1 addr=0xFD0B0000`
- `uio2 addr=0xFD490000`
- `uio3 addr=0xFFA10000`

建议下一步：写一个最小 `mmap + 读寄存器` 工具，先验证计数器在有负载时会变化。
通过标准：至少 1 组 PMON 计数可随压力变化。

---

## 3.2 第 1 层：中级（需要改 bitstream/设备树）

### A. PS-PL DMA 吞吐实验（最重要）
目标：验证 DDR -> AXI DMA -> PL 处理核 -> DDR 的吞吐和时延。
- 设计：AXI DMA + 自定义 AXI-Stream 内核（先 loopback 再加算子）
- 观测：PMON（总线流量/突发效率）+ 应用层吞吐
- 输出指标：
  - 单次延迟（us）
  - 稳态吞吐（MB/s）
  - A53 占用率（%）

通过标准：在可接受 CPU 占用下，PL 路径吞吐明显优于纯 CPU 实现。

### B. 显示链路与音频联动
当前内核日志里 DP 子系统已 probe，但存在 codec 时钟告警；建议在新镜像中补齐时钟/设备树配置后进行：
- DRM/KMS 显示输出
- 音频回放/录制
- 端到端延迟测量（图像 + 声音）

通过标准：稳定显示 + 无爆音卡顿 + 可重复回放。

### C. 扩展口场景化测试（Pmod/Grove/RPi）
- Pmod 传感器采集 -> PL 预处理 -> A53 显示/上报
- Grove 模拟量经 XADC 采样 -> 阈值检测 -> GPIO/中断响应

通过标准：形成一个闭环 demo（采集、计算、输出）。

---

## 3.3 第 2 层：高级（系统协同）

### A. A53 + R5F 异构协同（OpenAMP/remoteproc）
目标：
- R5F 负责硬实时环（控制周期、确定性）
- A53 Linux 负责人机界面、网络、模型更新

建议实验：
- RPMsg 往返时延与抖动统计
- A53 高负载下，R5 控制环稳定性评估

通过标准：高负载背景下仍满足实时抖动约束。

### B. PYNQ Overlay 快速迭代
- 用 PYNQ + Notebook 快速验证 PL IP 原型
- 原型稳定后再迁移到 Vitis/PetaLinux 生产链路

通过标准：同一算法能完成 “Notebook 原型 -> Linux 应用落地” 的迁移。

### C. 性能闭环（推荐作为课程/项目最终产出）
对每个版本都记录：
- 功能正确性
- 吞吐/时延
- CPU 占用
- 功耗/温升（如后续补传感器）
- 稳定性（长时间运行）

通过标准：形成可复现实验报告和版本对比图表。

---

## 4. 建议的学习与测试节奏（4 周）

### 第 1 周：基线与观测
- 完成第 0 层全部命令
- 跑通 PMON 读数脚本
- 固化“测量模板”（统一记录格式）

### 第 2 周：PL 加速最小闭环
- 做 AXI DMA loopback
- 增加一个简单算子（如 FIR/FFT/阈值）
- 输出吞吐/时延对比（CPU vs PL）

### 第 3 周：多媒体或传感器场景
- 选一条业务链路：DP/音频 或 Pmod/Grove/XADC
- 做端到端稳定性测试（>1h）

### 第 4 周：异构协同与总结
- 接入 R5F（如课程允许）
- 完成 A53+R5+PL 任务分工
- 形成最终报告（指标 + 架构 + 结论）

---

## 5. 你这台板子的“下一步优先动作”

1. 先补一个 `codex_test` 入口脚本（当前目录为空），把“基线采集命令”自动化。
2. 安装缺失测试工具（`perf`/`fio`/`iperf3`/`can-utils`），提升量化能力。
3. 立刻启动“AXI PMON + DMA”实验，这是最能体现 ZU3 价值的路径。

---

## 6. 参考链接（官方）

- AMD AUP-ZU3 页面：
  - https://www.amd.com/en/corporate/university-program/aup-boards/realdigital-aup-zu3.html
- RealDigital AUP-ZU3 页面：
  - https://www.realdigital.org/hardware/aup-zu3
- AUP-ZU3 Reference Manual（PDF）：
  - https://www.realdigital.org/downloads/3cd0cbae4d00eaf94270e5792c746d4c.pdf
- PYNQ 支持板卡与镜像：
  - https://www.pynq.io/boards.html
- AUP-ZU3 PYNQ 页面：
  - https://xilinx.github.io/AUP-ZU3/
- AMD AXI Performance Monitor IP：
  - https://www.amd.com/en/products/adaptive-socs-and-fpgas/intellectual-property/axi_perf_mon.html

