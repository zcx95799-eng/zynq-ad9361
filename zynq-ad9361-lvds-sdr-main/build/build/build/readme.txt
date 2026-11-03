# build目录使用说明
1. 环境要求
   - Vivado版本：2019.1
   - 支持系统：Windows、Linux
2. 复现步骤
   Windows：直接双击 runme.bat，自动调用Vivado执行 system_top.tcl
   Linux：终端进入本目录，执行命令：sh runme.sh
3. 脚本功能
   system_top.tcl：创建工程、导入src下RTL源码、执行综合、布局布线、生成bit文件、导出DRC/资源/时序等报告
4. 运行输出产物
   - system_top.bit：FPGA比特流文件
   - *.rpt：DRC、资源、时钟、IO等设计报告
   - runme.log：完整编译运行日志，报错可在此查看
5. 注意事项
   - 脚本采用相对路径，请勿随意改动整个项目文件夹层级
   - 请使用 Vivado 2019.1 版本，版本不一致可能出现IP、语法兼容性问题
   - 编译完成后，bit、报告文件会生成在当前build目录