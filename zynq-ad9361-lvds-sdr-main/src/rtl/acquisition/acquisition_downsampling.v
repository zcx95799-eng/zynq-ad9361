`timescale 1ns / 1ps

module Acquisition_Downsampling
#
(
    parameter INPUT_DATA_WIDTH  = 32'd20,   // 输入数据位宽，默认20bit
    parameter OUTPUT_DATA_WIDTH = 32'd20,   // 输出数据位宽，默认20bit     
    parameter SUM_NUM           = 32'd10    // 降采样倍数/累加数据个数（例如每10个点输出1个点）
)
(
    input                                   iw_Clk_p_g,             // 输入高频采样时钟
    input                                   iw_Rst_n_g,             // 异步复位，低电平有效
    
    (* mark_debug = "true" *) input   signed  [INPUT_DATA_WIDTH-1:0]  iw_Input_Data,          // 有符号输入采样数据

    (* mark_debug = "true" *) input                                   iw_Downsample_Pulse,    // 降采样触发脉冲，指示一个降采样周期的开始
    
    output                                  ow_Output_Valid_Pulse,  // 降采样数据有效脉冲（延时1周期同步输出）                   
    (* mark_debug = "true" *) output  signed  [OUTPUT_DATA_WIDTH-1:0] ow_Output_Data          // 降采样截位后的输出数据
);

//*********************************************************************************
    // 内部函数：计算 2^i >= n 的最小位宽
    function integer log2(input integer n);
        integer i;     
        for( i=0; 2**i<=n; i=i+1) 
            log2 = i + 1;
    endfunction    
//*********************************************************************************

    // 1. 计算需要截断/右移的 bit 数：N = log2(SUM_NUM)
    //    当 SUM_NUM = 10 时，log2(10) = 4，即移位 4 bit（相当于除以 16）
    localparam  REMOVED_BITS = log2(SUM_NUM);   

    // 2. 计算累加器位宽 = 输入位宽 + 增益扩展位宽，防止多点累加溢出
    localparam	SUM_DWIDTH   = (INPUT_DATA_WIDTH + REMOVED_BITS);	

    (* mark_debug = "true" *) reg	[REMOVED_BITS-1:0]	cont;   // 辅助计数器（代码中仅递增，未参与控制条件）
    (* mark_debug = "true" *) reg	[SUM_DWIDTH-1:0]	sum;    // 累加器寄存器

    // ============================================================================
    // 1. 积分累加逻辑（Accumulator）
    // ============================================================================
    always @(posedge iw_Clk_p_g)
    begin
        if(~iw_Rst_n_g) 
        begin
            cont <= {REMOVED_BITS{1'b0}};
            sum	 <= {SUM_DWIDTH{1'b0}};
        end
        // 降采样脉冲到来：锁存当前输入的第1个采样点，并重置累加器
        else if(iw_Downsample_Pulse) 
        begin
            cont <= {REMOVED_BITS{1'b0}};
            // 输入数据进行符号位扩展后赋予累加器
            sum  <= {{REMOVED_BITS{iw_Input_Data[INPUT_DATA_WIDTH-1]}}, iw_Input_Data};   
        end
        // 无脉冲时：对后续输入的点连续累加
        else
        begin
            cont <= cont + 1'b1;
            sum  <= sum + {{REMOVED_BITS{iw_Input_Data[INPUT_DATA_WIDTH-1]}}, iw_Input_Data};
        end
    end

    // ============================================================================
    // 2. 输出数据与有效指示打拍逻辑（Dump & Align）
    // ============================================================================
    (* mark_debug = "true" *) reg         r_Downsample_Pulse_Sync;
    (* mark_debug = "true" *) reg signed  [OUTPUT_DATA_WIDTH-1:0] r_Output_Data;

    assign ow_Output_Valid_Pulse = r_Downsample_Pulse_Sync;
    assign ow_Output_Data        = r_Output_Data;
    
    // 降采样有效脉冲打一拍输出，与锁存的数据输出保持时序同步
    always @(posedge iw_Clk_p_g)
    begin
        r_Downsample_Pulse_Sync <= iw_Downsample_Pulse;
    end
    
    // 降采样脉冲到来时，将上一周期的累加和算术右移 REMOVED_BITS 位（有符号除法），锁存到输出寄存器
    always @(posedge iw_Clk_p_g)
    begin
        if(iw_Downsample_Pulse)
            r_Output_Data <= (sum >>> REMOVED_BITS);  // >>> 表示有符号算术右移，保留正负符号位
        else
            r_Output_Data <= r_Output_Data;
    end

endmodule
