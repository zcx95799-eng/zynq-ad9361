`timescale 1ns / 1ps

module PSK
(
    input           iw_clk,                     // 系统全局时钟
    input           iw_rst,                     // 高电平全局复位

    input           iw_wr_en_message,           // 帧配置写入使能，锁存调制模式参数
    input [1:0]     iw2_din_ModSch_message,      // 调制模式配置：00=BPSK，01=QPSK，10=OQPSK

    // 帧组装模块输入信号（扩频后二进制比特）
    input           iw_valid_sync_header_h,    // 同步头输出有效标志
    input           iw_valid_data_segment_h,    // 业务数据段输出有效标志

    input           iw_frame_assembly_without_data_segment,    // 同步头单bit扩频数据
    input   [1:0]   iw2_frame_assembly_only_data_segment,       // 数据段2bit I/Q扩频比特 {I_bit, Q_bit}
    output reg              ow_baseband_valid,   // I/Q基带波形同步有效标志
    // 调制滤波后基带输出，直接对接12bit DAC
    output reg signed [11:0]   ow12_out_baseband_I, // I路基带成型波形
    output signed [11:0]   ow12_out_baseband_Q  // Q路基带成型波形
);

// 寄存调制模式参数
reg [1:0]   r2_din_ModSch_message;

always @(posedge iw_clk) 
begin
    if (!iw_rst) 
    begin
        r2_din_ModSch_message <= 2'd0; // 默认BPSK模式
    end
    else if (iw_wr_en_message) 
    begin
        r2_din_ModSch_message <= iw2_din_ModSch_message; // 锁存当前帧调制方式              
    end
end

// I/Q通道送入FIR滤波器的8bit有符号符号电平输入
 reg signed [7:0] r8_fir_data_inI;
 reg signed [7:0] r8_fir_data_inQ;

always @(posedge iw_clk)
begin
    if (!iw_rst)
    begin
        r8_fir_data_inI <= 8'd0;
        r8_fir_data_inQ <= 8'd0; 
    end
    // 场景1：同步头阶段，统一采用BPSK映射，Q通道恒0
    else if(iw_valid_sync_header_h)
    begin 
        // 扩频比特1映射负电平-127，比特0映射正电平+127
        r8_fir_data_inI <= iw_frame_assembly_without_data_segment ?  -8'd127 : 8'd127;
        r8_fir_data_inQ <= 8'd0;  
    end 
    // 场景2：数据段 + BPSK调制，仅I路有效，Q=0
    else if(iw_valid_data_segment_h && (r2_din_ModSch_message==2'b00))
    begin
        r8_fir_data_inI <= iw2_frame_assembly_only_data_segment[1] ? -8'd127 : 8'd127;   
        r8_fir_data_inQ <= 8'd0;            
    end
    // 场景3：数据段 + QPSK/OQPSK调制，2bit组合映射4种正交I/Q电平
    else if(iw_valid_data_segment_h && (r2_din_ModSch_message!=2'b00))
    begin
        case(iw2_frame_assembly_only_data_segment) // 输入格式 {I_bit, Q_bit}
        2'b00: // I=0 Q=0
        begin
            r8_fir_data_inI <= 8'd127;
            r8_fir_data_inQ <= -8'd127;
        end
        2'b01: // I=0 Q=1
        begin
            r8_fir_data_inI <= 8'd127;
            r8_fir_data_inQ <= 8'd127;                
        end
        2'b10: // I=1 Q=0
        begin
            r8_fir_data_inI <= -8'd127;
            r8_fir_data_inQ <= -8'd127;                 
        end
        2'b11: // I=1 Q=1
        begin
            r8_fir_data_inI <= -8'd127;
            r8_fir_data_inQ <= 8'd127;                 
        end
        endcase            
    end
    // 空闲无数据阶段，I/Q置0
    else 
    begin
        r8_fir_data_inI <= 8'd0;
        r8_fir_data_inQ <= 8'd0;
    end
end

// FIR滤波器IP输出16bit有符号成型波形
 wire signed [15:0]  fir_tdataI;
 wire signed [15:0]  fir_tdataQ;
 wire                fir_tvalidI;
 wire                fir_tvalidQ;
// I路成型FIR滤波器实例
fir_BPSK uut2   (
    .aclk(iw_clk),                              // AXI滤波器时钟
    .aresetn(iw_rst),                          // 滤波器低电平复位，模块高复位取反对接

    .s_axis_data_tvalid(1'b1),                  // 持续有效，不间断送入符号电平
    .s_axis_data_tready(),                      // 滤波器就绪信号，本设计不做流量控制悬空
    .s_axis_data_tdata(r8_fir_data_inI),        // 8bit I路符号电平输入

    .m_axis_data_tvalid(fir_tvalidI),           // I路滤波输出有效标志
    .m_axis_data_tdata(fir_tdataI)              // 16bit滤波成型输出，固定13拍延迟
);                        

// Q路成型FIR滤波器实例，结构与I路完全一致
fir_BPSK uut3   (
    .aclk(iw_clk),                              
    .aresetn(iw_rst),                           

    .s_axis_data_tvalid(1'b1),                  
    .s_axis_data_tready(),                      
    .s_axis_data_tdata(r8_fir_data_inQ),        

    .m_axis_data_tvalid(fir_tvalidQ),           // Q路滤波输出有效标志
    .m_axis_data_tdata(fir_tdataQ)              
);

reg [28:0] valid_pipe;

always @(posedge iw_clk) begin
    if (!iw_rst)
        valid_pipe <= 29'd0;
    else
        valid_pipe <= {
            valid_pipe[27:0],
            iw_valid_sync_header_h | iw_valid_data_segment_h
        };
end


always @(*) begin
    ow12_out_baseband_I = fir_tdataI[11:0];
end
//assign ow12_out_baseband_I = fir_tdataI[11:0]; 
assign ow12_out_baseband_Q = fir_tdataQ[11:0];
always @(*) begin
     ow_baseband_valid =
       fir_tvalidI &
       fir_tvalidQ &
       valid_pipe[28];
end


//ila_0 your_instance_name (
//	.clk(iw_clk), // input wire clk


//	.probe0(ow12_out_baseband_I), // input wire [0:0]  probe0  
//	.probe1(fir_tvalidI), // input wire [1:0]  probe1 
//	.probe2(r8_fir_data_inI), // input wire [7:0]  probe2 
//	.probe3(r8_fir_data_inQ), // input wire [7:0]  probe3 
//	.probe4(fir_tdataI), // input wire [15:0]  probe4 
//	.probe5(fir_tdataQ), // input wire [15:0]  probe5
//	.probe6(iw_valid_sync_header_h), // input wire [11:0]  probe6 
//	.probe7(iw2_frame_assembly_only_data_segment[1]) // input wire [11:0]  probe7
//);

endmodule