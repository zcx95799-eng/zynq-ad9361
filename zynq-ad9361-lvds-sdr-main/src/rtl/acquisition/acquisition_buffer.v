`timescale 1ns / 1ps

module Acquisition_Buffer
#(
    parameter                           BUFF_ADDR_WD        = 15,   // 缓冲区地址位宽，默认15 bit（深度 2^15 = 32768）
    parameter                           DATA_BUFF_DWIDTH    = 16    // 数据缓冲区位宽，默认16 bit
)
(
    input                               iw_Clk_p_g,                 // 系统工作时钟

    // ============================================================================
    // 写端口（数据通道与码通道共享写使能和写地址）
    // ============================================================================
    input                               buff_wen,                   // 写使能控制：1 = 写操作有效；0 = 无效
    input  [BUFF_ADDR_WD-1:0]           buff_waddr,                 // 数据与扩频码缓冲区共享的写地址
    input  [DATA_BUFF_DWIDTH-1:0]       dbuff_wdata,                // 写入数据缓冲区的数据（如 I/Q 采样或滤波后数据）
    input                               cbuff_wdata,                // 写入扩频码缓冲区的数据（1-bit 本地/接收 PN 码）
    
    // ============================================================================
    // 数据缓冲区（Data Buffer）独立读端口
    // ============================================================================
    input  [BUFF_ADDR_WD-1:0]           dbuff_raddr,                // 数据缓冲区读地址
    output reg [DATA_BUFF_DWIDTH-1:0]   dbuff_rdata,                // 数据缓冲区读出数据（延迟 1 个时钟周期）
    
    // ============================================================================
    // 扩频码缓冲区（Code Buffer）独立读端口
    // ============================================================================
    input  [BUFF_ADDR_WD-1:0]           cbuff_raddr,                // 扩频码缓冲区读地址
    output reg                          cbuff_rdata                 // 扩频码缓冲区读出数据（延迟 1 个时钟周期）

);

    // 计算缓冲区的最大存储深度：2^BUFF_ADDR_WD (当 BUFF_ADDR_WD=15 时，深度为 32768)
    localparam      MAX_BUFF_DEEPTH = (1<<(BUFF_ADDR_WD));
    
    // 综合约束属性 (*ram_style="block"*)：
    // 强制指示 Vivado/ISE 工具使用 FPGA 芯片内部的 Block RAM (BRAM) 硬件资源来实现内存，
    // 防止综合工具误将其映射为逻辑资源（LUT / Distributed RAM）。
    (*ram_style="block"*)reg [DATA_BUFF_DWIDTH-1:0] mem_data[MAX_BUFF_DEEPTH-1:0]; // 存储基带数据的 BRAM 阵列
    (*ram_style="block"*)reg                        mem_code[MAX_BUFF_DEEPTH-1:0]; // 存储 PN 码/同步码的 BRAM 阵列

    // ============================================================================
    // 写操作逻辑 (Write Operation)
    // 当 buff_wen 为高时，在同一个时钟上升沿将数据与 PN 码保存到相同的地址单元中
    // ============================================================================
    always @(posedge iw_Clk_p_g)
    begin
        if(buff_wen) 
        begin
            mem_data[buff_waddr] <= dbuff_wdata;    // 保存数据到 mem_data
            mem_code[buff_waddr] <= cbuff_wdata;    // 保存扩频码到 mem_code
        end
    end
    
    // ============================================================================
    // 读操作逻辑 (Read Operation) - 标准 BRAM 同步读，读 Latency = 1 clk
    // 数据通道与码通道的读地址相互独立，方便进行滑动相关运算
    // ============================================================================
    // 1. 读取基带数据
    always @(posedge iw_Clk_p_g)
    begin
        dbuff_rdata <= mem_data[dbuff_raddr];
    end
    
    // 2. 读取扩频码数据
    always @(posedge iw_Clk_p_g)
    begin
        cbuff_rdata <= mem_code[cbuff_raddr];
    end

endmodule
