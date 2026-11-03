`timescale 1ns / 1ps

module scrambler 
(
    input   iw_clk_p_g,                // 系统全局时钟
    input   iw_Rst_l,                  // 高电平全局复位

    // ========== 【发送加扰通路接口】 ==========
    input   iw_Data_Valid_h,           // 待加扰原始串行数据有效标志
    input   iw_Data,                   // 待加扰原始输入比特

   output  ow_Scrambler_Valid_h,      // 加扰后数据输出有效标志
   output  ow_Scrambler_Out,          // 加扰完成串行比特输出

    // ========== 【接收解扰通路接口】 ==========
    input   iw_Scrambler_Valid_h,      // 外部输入加扰比特有效标志
    input   iw_Scrambler_In,           // 外部输入加扰后的串行比特

     output  ow_Descrambler_Valid_h,    // 解扰还原数据有效标志
     output  ow_Descrambler_Out         // 解扰还原后的原始比特输出
);

// ====================== 发送端：加扰逻辑 ======================
wire        w_tmp;                     // LFSR反馈异或中间值
reg [7:0]   r8_lfsr_reg1;              // 加扰通路8位线性移位寄存器
reg         r_Scrambler_Valid_h;       // 加扰输出有效寄存器
reg         r_Scrambler_Out;           // 加扰输出比特寄存器

// 顶层输出赋值
assign  ow_Scrambler_Valid_h = r_Scrambler_Valid_h;
assign  ow_Scrambler_Out     = r_Scrambler_Out;

// 8阶LFSR反馈多项式：bit0 ^ bit2 ^ bit4 ^ bit7
assign w_tmp = r8_lfsr_reg1[0]^r8_lfsr_reg1[2]^r8_lfsr_reg1[4]^r8_lfsr_reg1[7];

always @(posedge iw_clk_p_g ) 
begin
    if (!iw_Rst_l) 
    begin
        r8_lfsr_reg1 <= 8'b1111_1111; // 复位LFSR初始值全1，规避全0死锁
        
        r_Scrambler_Valid_h <= 1'b0;
        r_Scrambler_Out     <= 1'b0;
    end
    else if (iw_Data_Valid_h) // 有有效输入比特，执行加扰与LFSR移位
    begin
        // LFSR左移1位，反馈值填入最低位
        r8_lfsr_reg1 <= {r8_lfsr_reg1[6:0],w_tmp};     
        // 加扰核心：原始数据比特 ^ LFSR最高位随机序列
        r_Scrambler_Out <= iw_Data ^ r8_lfsr_reg1[7];
        r_Scrambler_Valid_h <= 1'b1; // 标记当前输出为有效加扰比特
    end
    else // 无有效输入，重置LFSR，输出清零
    begin
        r8_lfsr_reg1 <= 8'b1111_1111;
        
        r_Scrambler_Valid_h <= 1'b0;
        r_Scrambler_Out     <= 1'b0;
    end
end

// ====================== 接收端：解扰逻辑 ======================
wire        w_tmp2;                    // 解扰通路LFSR反馈异或中间值
reg [7:0]   r8_lfsr_reg2;             // 解扰通路独立8位LFSR寄存器
reg         r_Scrambler_Valid_h2;     // 解扰输出有效寄存器
reg         r_Scrambler_Out2;         // 解扰输出比特寄存器

// 顶层解扰输出赋值
assign  ow_Descrambler_Valid_h = r_Scrambler_Valid_h2;
assign  ow_Descrambler_Out     = r_Scrambler_Out2;

// 与加扰通路完全相同的LFSR反馈多项式，保证随机序列一致
assign w_tmp2 = r8_lfsr_reg2[0]^r8_lfsr_reg2[2]^r8_lfsr_reg2[4]^r8_lfsr_reg2[7];

always @(posedge iw_clk_p_g ) 
begin
    if (!iw_Rst_l) 
    begin
        r8_lfsr_reg2 <= 8'b1111_1111; // 解扰LFSR复位初始值与加扰端一致
        
        r_Scrambler_Valid_h2 <= 1'b0;
        r_Scrambler_Out2     <= 1'b0;
    end
    else if (iw_Scrambler_Valid_h) // 收到有效加扰比特，执行解扰
    begin
        r8_lfsr_reg2 <= {r8_lfsr_reg2[6:0],w_tmp2};     
        // 解扰核心：加扰比特再次异或同规则LFSR序列，还原原始数据
        r_Scrambler_Out2 <= iw_Scrambler_In ^ r8_lfsr_reg2[7];
        r_Scrambler_Valid_h2 <= 1'b1;
    end
    else // 无加扰输入，重置解扰LFSR
    begin
        r8_lfsr_reg2 <= 8'b1111_1111;
        
        r_Scrambler_Valid_h2 <= 1'b0;
        r_Scrambler_Out2     <= 1'b0;
    end
end

endmodule