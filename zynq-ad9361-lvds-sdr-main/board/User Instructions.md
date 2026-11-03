# 智能数字通信收发系统：board 上板交付

截图中的 `board/ # 上板工程、运行脚本与实测输出` 是目录内容约定。本目录按此约定整理了你当前的 Vivado、SDK、Qt 工程、可下载文件、运行脚本和已保存的真实 ILA 记录。

**现有原工程没有修改。本轮没有重新下载、复位板卡或触发新采样。** `measured/` 是原工程已经保存的硬件捕获；`validation/` 是本轮实际完成的离线检查。已有捕获不能证明完整256字通信无误，原因见后文。

## 1. 包含什么

```text
board/
  README.md
  project/
    vivado/AD9361_LVDS_PRJ/     Vivado XPR、RTL、BD、XDC、自定义IP和已有IP生成文件
    vivado/AGC.coe             保持原相对引用位置
    sdk/                      AD9361应用、BSP、原SDK硬件平台
  artifacts/
    fpga/                     当前实现的bit、ltx、hwdef、HDF、对应PS初始化文件
    sdk/                      原工程已有AD9361.elf、原SDK平台ps7_init
    qt/                       本轮离线编译的tcp.exe（运行仍需Qt6.10及MinGW运行库）
    reports/                  原实现资源、时序报告和实现日志
  host/qt/                    原Qt源码与tcp.pro
  scripts/                    下载、构建、收发、ILA导出和分析脚本
  measured/
    raw/                      两份真实.ila及其中原始CSV、VCD、probes.ltx
    figures/                  已制作的原截图拼接与采样重绘图片
    analysis/                 从原始CSV计算的统计、逐段数据字及中文说明
  validation/                 本轮离线检查和构建日志、来源及校验记录
  run_outputs/                新运行记录；随包的dry_run目录没有连接板卡
  manifest.sha256             随包文件校验清单
```

省略原工程的整个 `.runs/.cache/.Xil` 缓存。当前可用下载文件单独保存在 `artifacts/`；IP生成文件与系数文件已经保留，无需为了下载现成bit重新生成FIR。

## 2. 工程的实际作用

本工程使用 Zynq-7020（`xc7z020clg400-1`）和 AD9361，完成电脑业务数据下发、基带扩频发送、接收同步与解调，以及通过网口向 Qt 回传监测数据。

数据链路如下：

```text
PC业务数据 → TCP 5002 → ARM/lwIP → AXI DMA MM2S → PL业务缓存
          → 扩频/基带发送 → AD9361 LVDS/射频链路
          → 接收预处理/捕获/跟踪/解调/解扰
          → 监测帧组装 → AXI DMA S2MM → ARM/lwIP → PC TCP 5001 → Qt
```

关键入口：

| 内容 | 文件 |
|---|---|
| 顶层连接、业务入口、监测输出、发送I/Q的ILA | `project/vivado/AD9361_LVDS_PRJ/AD9361_LVDS_PRJ.srcs/sources_1/new/system_top.v` |
| 当前实际参与工程的发送模块 | 同目录 `trans_top1.v`，内部模块名为 `trans_top` |
| 接收与同步控制 | 同目录 `Receiver_Top.v`、`Sync_Cntl.v` |
| ARM初始化与射频参数 | `project/sdk/AD9361/src/main.c` |
| 业务TCP下发、DMA MM2S | 同目录 `payload_tcp_server.c` |
| 监测DMA接收与TCP回传 | 同目录 `tcp_transmission.c` |
| IP与端口配置 | 同目录 `network_config.h` |
| Qt入口与界面 | `host/qt/main.cpp`、`mainwindow.cpp` |

当前顶层为 `.USE_PS_PAYLOAD(1)`：业务载荷来自PC。不要把未启用的旧版 `trans_top.v` 当作实际发送代码。Qt具备预览入口；运行 `--demo` 或 `--preview` 得到的是预览数据，不能计入实测。

代码中的射频设置值：收发本振1.5GHz、AD9361采样率61.2MSPS、射频带宽20MHz、接收增益21dB、发送衰减20000mdB。这些是代码设置值，并非本轮仪表测量结果。

## 3. 环境与路径

- 已验证工具：Vivado 2019.1、Xilinx SDK/XSCT 2019.1、Python 3、Qt 6.10.0 MinGW 64位。
- 建议把压缩包解压到纯英文短路径，例如 `D:\board`。本机验证发现Xilinx 2019.1在中文路径下会发生路径转换问题，不能仅通过改COE解决。
- 下文命令在PowerShell执行，先进入 `D:\board`。工具路径与本机一致；若你的安装位置不同，只改命令里的工具路径或脚本参数。
- `AGC.coe` 位于Vivado项目父目录；FIR和PN的COE位于 `AD9361_LVDS_PRJ.ip_user_files/mem_init_files`。保持整个目录结构，系数内容不需要修改。
- 单独下载现成bit、ELF即可运行，不需要先重新综合。

## 4. 直接上板运行

1. 使用现有已确认可用的板卡电源、JTAG、网线及射频连接，选择JTAG启动方式。具体拨码位置以板卡型号和手册为准，本截图无法确定拨码。
2. 将直连板卡的电脑网卡设置为 `192.168.1.209`，子网掩码 `255.255.255.0`；板卡程序地址为 `192.168.1.10`。
3. 先启动一个监测接收端：Qt或下文Python接收器，二选一。两者不能同时占用5001端口。
4. 在SDK/Vivado中启动本地硬件服务，或在独立终端运行 `F:\vivado2\SDK\2019.1\bin\hw_server.bat` 并保持终端开启。若该端口已有硬件服务，无需重复启动。
5. 执行下列下载脚本。**此步骤会复位并重新配置所连接的板卡。** 脚本只在检测到唯一Cortex-A9核0时继续。

```powershell
cd D:\board
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_board.ps1
```

脚本默认下载 `artifacts/fpga/system_top.bit`，使用当前HDF内的 `ps7_init.tcl`，加载原工程已有的 `artifacts/sdk/AD9361.elf`。日志保存到 `run_outputs/<时间>_download/xsct.log`。

当前实现与原SDK硬件平台的PS初始化文件有少量差异，原SDK旧脚本单独保留为 `artifacts/sdk/ps7_init_original_sdk.tcl`。已离线核对DMA、GPIO、AD9361配置地址与BSP一致，但本轮未重新上板验证当前初始化文件和ELF的组合。

也可在XSCT直接运行（会复位、下载板卡）：

```powershell
& 'F:\vivado2\SDK\2019.1\bin\xsct.bat' .\scripts\download_run.tcl
```

不要误用 `project/sdk/dma_system_wrapper_hw_platform_0/` 中的旧bit替代 `artifacts/fpga/` 的当前bit。

## 5. 接收、下发与保存新实测

### 方法A：原Qt界面

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\start_qt.ps1
```

该启动器为本机Qt安装设置运行库搜索路径，打开随包的 `artifacts/qt/tcp.exe`。电脑监测端监听5001，业务发送目标为 `192.168.1.10:5002`。查看实时板卡数据时不要启用预览模式。若Windows弹出防火墙提示，应允许该接收程序在所用网络上接收连接。

### 方法B：Python记录原始监测流

在第一个PowerShell窗口启动接收端，再运行/重启板卡程序：

```powershell
python .\scripts\board_io.py receive --seconds 60
```

另一个窗口下发一次已知载荷：

```powershell
python .\scripts\board_io.py send --sequence 1
```

默认发送256个LE32数据字 `1,2,...,256`，共1024字节。发送自定义业务文件：

```powershell
python .\scripts\board_io.py send --sequence 2 --file D:\payload.bin
```

文件必须恰好1024字节。只检查封包、不联网：

```powershell
python .\scripts\board_io.py send --dry-run
```

发送记录包括 `payload.bin`、完整 `request.bin`、收到的 `reply.bin` 和 `result.json`。接收记录包括原始 `moni_stream.bin`、`frames.jsonl`、逐帧附加区及统计 `result.json`，均保存于新建的 `run_outputs/` 子目录。无数据或连接失败会明确记录，脚本不会生成假的实测成功结果。

**应答为0只说明MM2S传输完成、数据进入PL暂存，不能证明射频解调正确。** 必须再核对有效解调帧的1024字节附加区与所发载荷，才能判断业务数据是否一致。由于捕获条件和缓存时序仍需板测核对，不应仅凭Qt显示“完整帧”认定通信完全正确。

将对应实验的发送载荷和接收端标记为 `_demod.bin` 的有效解调附加区进行比对：

```powershell
python .\scripts\compare_payload.py .\run_outputs\某次_send\payload.bin .\run_outputs\某次_receive\00000000_seq0_demod.bin --output .\run_outputs\某次_receive\comparison.json
```

替换为真实文件名。报告给出逐字差异和不同位数；单帧比对不能当作长期信道BER，MONI序号也不保证等于TX请求序号，需自行按实验记录关联。

### 当前协议

| 方向 | 格式 |
|---|---|
| PC→板卡5002 | `TXD1` + BE32序号 + BE32长度1024 + 256个LE32数据字 |
| 板卡→PC应答 | `TXA1` + BE32序号 + BE32状态（0成功，1 DMA_ERROR，2 DMA_BUSY_TIMEOUT，3 FRAME_TIMEOUT） |
| 板卡→PC5001 | 16字节MONI头（`!4sBBHII`）+ 5120字节负载，version=1、type=5 |
| MONI负载 | 前4096字节为1024对LE16有符号I/Q，后1024字节为附加区 |

flags：`0x0001`完整解调帧有效，`0x0002`I/Q有效，`0x0100`发送参考分片，`0x0200`捕获FFT分片。后两类附加区不能当作业务解调帧。接收脚本已作相应区分，I/Q有效性以flags为准。

## 6. 保存与分析ILA

在Vivado中完成采集并上传数据后，在该Vivado窗口的Tcl Console执行（按实际解压路径）：

```tcl
source {D:/board/scripts/export_ila.tcl}
```

脚本导出当前Vivado内存中已有的所有ILA数据对象到新的 `run_outputs/<时间>_ila/`，包含 `.ila`、`.csv` 和对象名对照表。它不会重新触发采样，也不会调用上传命令停止正在进行的采集。新导出文件是否属于本次实验，应结合对象名和采集记录确认。

分析随包的两份旧捕获：

```powershell
python .\scripts\analyze_ila.py
```

分析自己新导出的数据时，显式指定输入和新的输出目录，以保留基线报告：

```powershell
python .\scripts\analyze_ila.py .\run_outputs\某次_ila\capture_01.ila --output .\run_outputs\某次_ila\analysis
```

当前实际信号映射：`system_top.v` 中 `ila_0 u_ila_axis` 观测发送侧12位I/Q，在这批保存记录里对应hw_ila_1；接收同步控制中的ILA记录解扰I/Q各1位与32位读出，在这批记录里对应hw_ila_2。ILA实例型号、RTL实例名与硬件窗口编号含义不同，应以配套LTX和CSV探针名为准。

## 7. 已有实测结果及其边界

原始来源为 `E:\2R2T_LVDS\AD9361_LVDS_PRJ\AD9361_LVDS_PRJ.hw\backup`，文件按字节复制并记录SHA256：

| 文件 | 原文件修改时间（原电脑+08:00时区） | 点数 | 触发位置 |
|---|---|---:|---:|
| `hw_ila_data_1.ila` | 2026-11-01 18:35:49 | 8192 | 4096 |
| `hw_ila_data_2.ila` | 2026-11-01 18:38:56 | 8192 | 4096 |

ILA1包含发送I/Q波形。ILA2的32位读出在采样4096～5111观察到连续1～254，每个数保持4拍；5112～5115为 `0xBC0000FF`，随后为0。捕获未包含读有效和地址，也没有保留当时的完整发送日志，因此不能直接判定最后一个0是否有效、256字是否齐全，或特殊字是否为正确业务内容。

两份捕获来自不同时间，不能计算收发端到端时延。当前bit的文件时间早于捕获时间，且探针信号相符；但没有保存对应的下载日志，不能仅凭这一点证明采集时加载的就是该bit。包内没有新增串口、射频仪表、SNR、BER等实测数据。

详细分析见 `measured/analysis/实测分析.md`，逐段数据字见 `hw_ila_data_2_word_runs.csv`。

`measured/figures/original_screenshots_two_rows.png` 是原截图拼接；`raw_data_redrawn_all_samples.png` 是原始采样重绘；`signal_overview_with_original_headers.png` 是原信号表头与重绘波形合成，三者不是同一种截图来源。

## 8. 已有实现资源和时序

来自当前 `impl_1` 的已有报告，本轮没有重新实现整机：

| 资源 | 使用量/总量 | 占用率 |
|---|---:|---:|
| LUT | 28050/53200 | 52.73% |
| 寄存器 | 35686/106400 | 33.54% |
| Block RAM Tile | 98/140 | 70.00% |
| DSP | 134/220 | 60.91% |
| I/O | 45/125 | 36.00% |

布线后报告WNS=1.641ns、WHS=0.012ns，TNS/THS=0，报告显示用户指定时序约束通过。这不等价于本轮重新做了硬件功能验证。

## 9. 可选重新构建与离线校验

源码和系数不需要为了使用本包而修改。需要重新编译时，脚本只作用于交付副本，新产物放 `build/`，不会覆盖 `artifacts/` 中的基线下载文件。

```powershell
python .\scripts\validate_bundle.py
python .\scripts\build_sdk.py
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_qt.ps1
& 'F:\vivado2\Vivado\2019.1\bin\vivado.bat' -mode batch -source .\scripts\build_vivado.tcl
```

SDK脚本沿用包内已有BSP库，重新编译11个应用C文件；不声称已重新生成BSP。新的ELF在 `build/sdk/AD9361.elf`，下载脚本默认仍使用原ELF。若要明确选择新ELF，可向XSCT脚本传第二个参数。

Vivado脚本重跑综合、实现及bit生成，不主动升级IP或改写系数配置。由于未携带整套运行缓存，首次打开可能提示GeneratedRun文件不存在；这与源文件缺失是不同问题。本轮已在Vivado2019.1离线打开副本并核对1205个文件引用，无缺失；没有运行这次整机重建脚本。

本轮已实际完成：SDK应用编译、Qt编译、Vivado项目打开和文件引用检查、HDF与bit一致性检查、Python封包与MONI分段解析检查、ILA原始数据分析、Tcl语法完整性检查。下载、射频收发和新ILA采集仍需在板卡上执行，脚本语法完整并不代表这些硬件步骤已经验证通过。

检查记录见 `validation/验证说明.md`。原工程关键文件hash快照在 `validation/source_snapshot.json`，原始捕获与文件时间也记录于其中。
