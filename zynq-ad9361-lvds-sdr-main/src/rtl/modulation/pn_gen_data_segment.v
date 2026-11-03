`timescale 1ns / 1ps

module PN_gen_data_segment
#
(
    parameter M1 = 21'b1_0110_1100_1110_1010_0001  // LFSR1固定上电/帧复位初始状态
)
(
    input       iw_clk,                // 系统全局工作时钟
    input       iw_rst,                // 高电平全局复位
    
    input       iw_pn_code_pulse,      // PN码片同步脉冲，脉冲到来时LFSR移位更新

    input       iw_wr_en_message,      // 帧配置写入使能，锁存当前帧LFSR2初始相位
    input [20:0] iw21_InitPhase,       // 可配置21bit LFSR2帧初始相位(I/Q通路区分用)

    output      ow_valid_pulse_p,       // 帧周期完成有效脉冲：计数满51码片时拉高1个码片周期    
    output      out                    // 复合PN码输出：两路LFSR最高比特异或结果
);

    localparam  SYM_RATE_80kbps = 8'd51; // 80kbps符号速率对应单符号PN码片总数51

    reg [3:0] r4_din_SymRate_message;    // 预留符号速率寄存寄存器(当前代码未使用)
    reg [20:0] r21_InitPhase;            // 锁存LFSR2帧初始相位寄存器

// 锁存外部输入的LFSR2自定义初始相位，iw_wr_en_message为帧启动配置信号
always @(posedge iw_clk) 
begin
    if (!iw_rst)
    begin
        r21_InitPhase <= 21'd0;
    end
    else if(iw_wr_en_message)
    begin
        r21_InitPhase <= iw21_InitPhase;
    end
end

// ===================== 两路独立21阶LFSR移位寄存器 =====================
reg [20:0] shift_reg1; // LFSR1：固定初始值、固定反馈多项式
reg [20:0] shift_reg2; // LFSR2：帧可配置初始相位、独立反馈多项式  

reg  r_valid_pulse1_p; // LFSR1帧计数完成脉冲标志
reg  r_valid_pulse2_p; // LFSR2帧计数完成脉冲标志
reg [10:0] r11_cnt;    // PN码片计数器，最大计数值50(0~50共51个码片)

// ===================== 码片计数逻辑：每收到iw_pn_code_pulse计数自增 =====================
always @(posedge iw_clk) 
begin
    if (!iw_rst) 
    begin
        r11_cnt <= 11'd0;
    end 
    else if(iw_pn_code_pulse) // 仅码片脉冲到来时更新计数
    begin 
        // 计数到50(SYM_RATE_80kbps-1)则归零，开启下一组51码片周期
        r11_cnt <= (r11_cnt == (SYM_RATE_80kbps-8'd1)) ? 11'd0 : (r11_cnt+11'd1);
    end
end

// ===================== LFSR1时序逻辑：固定初始序列、多项式X^21 + X^13 + X^5 + X^2 + 1 =====================
always @(posedge iw_clk) 
begin
    if (!iw_rst) 
    begin
        shift_reg1 <= M1;   // 复位加载固定初始状态M1
        r_valid_pulse1_p <= 1'b0;
    end 
    else if(iw_pn_code_pulse) // 码片脉冲触发移位迭代
    begin
        if((SYM_RATE_80kbps-8'd1)==r11_cnt) // 当前是第51个码片，周期结束
        begin
            shift_reg1 <= M1;              // 重置LFSR1到固定初始值
            r_valid_pulse1_p <= 1'b1;      // 拉高LFSR1周期完成脉冲
        end
        else // 未计满51个码片，正常LFSR移位迭代
        begin
            // 反馈抽头：bit20 ^ bit12 ^ bit4 ^ bit1，整体左移，反馈值填入最低位
            shift_reg1 <= {shift_reg1[19:0], shift_reg1[20] ^ shift_reg1[12] ^ shift_reg1[4] ^ shift_reg1[1]};                
            r_valid_pulse1_p <= 1'b0;
        end
    end
    else // 无码片脉冲，周期标志保持低
    begin
        r_valid_pulse1_p <= 1'b0;
    end
end

// ===================== LFSR2时序逻辑：可配置帧初始相位、多项式X^21 + X^14 + X^12 + X^7 + X^6 + X^4 + X^3 + X^2 + 1 =====================
always @(posedge iw_clk) 
begin
    if (!iw_rst) 
    begin
        shift_reg2 <= r21_InitPhase; // 复位加载锁存的帧自定义初始相位
        r_valid_pulse2_p <= 1'b0;
    end 
    else if(iw_pn_code_pulse) // 码片脉冲触发移位迭代
    begin
        if((SYM_RATE_80kbps-8'd1)==r11_cnt) // 第51个码片，单符号周期结束
        begin
            shift_reg2 <= r21_InitPhase;  // 重置LFSR2为本帧配置的初始相位
            r_valid_pulse2_p <= 1'b1;     // 拉高LFSR2周期完成脉冲
        end
        else // 正常移位迭代，独立反馈抽头
        begin
            // 反馈抽头：bit20 ^ bit13 ^ bit11 ^ bit6 ^ bit5 ^ bit3 ^ bit2 ^ bit1
            shift_reg2 <= {shift_reg2[19:0], shift_reg2[20] ^ shift_reg2[13] ^ shift_reg2[11] ^ shift_reg2[6] ^ shift_reg2[5] ^ shift_reg2[3] ^ shift_reg2[2] ^ shift_reg2[1]};                
            r_valid_pulse2_p <= 1'b0;
        end
    end
    else 
    begin
        r_valid_pulse2_p <= 1'b0;
    end
end

// ===================== 顶层输出赋值 =====================
// 两路LFSR周期完成脉冲逻辑或，作为统一帧结束标志输出
assign ow_valid_pulse_p = r_valid_pulse1_p | r_valid_pulse2_p;
// 复合PN码生成：两路21阶m序列最高输出比特异或，获得复合扩频码片
assign out = shift_reg1[20] ^ shift_reg2[20]; 

endmodule