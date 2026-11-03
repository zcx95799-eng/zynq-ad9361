`timescale 1ns / 1ps

module Tracking_DDC
#
(
    parameter   INPUT_DATA_WIDTH                        = 32'd16,           // 输入信号位宽,决定了本模块乘法器IP参数
    parameter   LOCAL_CARRIER_NCO_OUTPUT_WIDTH          = 32'd4,            // 本地载波NCO输出数字载波位宽,由IP决定,也决定了本模块乘法器IP参数

    parameter   OUTPUT_DATA_WIDTH                       = 32'd20            // 输出信号位宽,20=INPUT_DATA_WIDTH+LOCAL_CARRIER_NCO_OUTPUT_WIDTH
)                       
(                       
    input                                               iw_Clk_p_g,         // 输入采样时钟
    input                                               iw_Rst_n_g,                                      
                        
    input   signed [INPUT_DATA_WIDTH-1:0]               iw_Input_Data_I,  
    input   signed [INPUT_DATA_WIDTH-1:0]               iw_Input_Data_Q, 
    input   signed [LOCAL_CARRIER_NCO_OUTPUT_WIDTH-1:0] iw_TrackingCarrier_Cos,
    input   signed [LOCAL_CARRIER_NCO_OUTPUT_WIDTH-1:0] iw_TrackingCarrier_Sin,
    
    output  signed [OUTPUT_DATA_WIDTH-1:0]              IF_mix_Q_i,         // 测距支路的同相支路
    output  signed [OUTPUT_DATA_WIDTH-1:0]              IF_mix_Q_q         // 测距支路的正交支路
    //output  signed [OUTPUT_DATA_WIDTH-1:0]              IF_mix_I_i          // 数传支路的同相支路
    
);
//*************************************************************************************************************

    localparam   Multi_MSB = INPUT_DATA_WIDTH+LOCAL_CARRIER_NCO_OUTPUT_WIDTH;

//*************************************************************************************************************

    wire [Multi_MSB-1:0]    I_IF_mix_i_tmp; // I*I'
    wire [Multi_MSB-1:0]    I_IF_mix_q_tmp; // I*Q'
    wire [Multi_MSB-1:0]    Q_IF_mix_i_tmp; // Q*-Q'
    wire [Multi_MSB-1:0]    Q_IF_mix_q_tmp; // Q*I'

    reg signed [OUTPUT_DATA_WIDTH:0] IF_mix_Q_i_tmp;
    reg signed [OUTPUT_DATA_WIDTH:0] IF_mix_Q_q_tmp;

    assign IF_mix_Q_i = IF_mix_Q_i_tmp[OUTPUT_DATA_WIDTH:1];
    assign IF_mix_Q_q = IF_mix_Q_q_tmp[OUTPUT_DATA_WIDTH:1];

    always @(posedge iw_Clk_p_g)
    begin
        IF_mix_Q_i_tmp <=   {{(OUTPUT_DATA_WIDTH-Multi_MSB+1){I_IF_mix_i_tmp[Multi_MSB-1]}},I_IF_mix_i_tmp} + {{(OUTPUT_DATA_WIDTH-Multi_MSB+1){Q_IF_mix_q_tmp[Multi_MSB-1]}},Q_IF_mix_q_tmp};   // 复运算结果I,测距支路的同相支路
        IF_mix_Q_q_tmp <=   {{(OUTPUT_DATA_WIDTH-Multi_MSB+1){I_IF_mix_q_tmp[Multi_MSB-1]}},I_IF_mix_q_tmp} + {{(OUTPUT_DATA_WIDTH-Multi_MSB+1){Q_IF_mix_i_tmp[Multi_MSB-1]}},Q_IF_mix_i_tmp};   // 复运算结果Q,测距支路的正交支路
    end

    localparam TRACKING_CARRIER_MAX = {1'b0, {(LOCAL_CARRIER_NCO_OUTPUT_WIDTH-1){1'b1}}};
    localparam TRACKING_CARRIER_MIN = {1'b1, {(LOCAL_CARRIER_NCO_OUTPUT_WIDTH-1){1'b0}}};
    (*KEEP="TRUE"*) wire signed [LOCAL_CARRIER_NCO_OUTPUT_WIDTH-1:0] w_TrackingCarrier_Sin_op  = (iw_TrackingCarrier_Sin == TRACKING_CARRIER_MIN)?TRACKING_CARRIER_MAX:-iw_TrackingCarrier_Sin;

    // 下面的运算是:(I+jQ)*(cos±jsin)
// 错误：.clk(iw_Clk_p_g)
// 正确：.CLK(iw_Clk_p_g)
// I*cos
TrackingDDC_Multiple I_ddc_multi_i_inst
(
    .CLK( iw_Clk_p_g            ),
    .A  ( iw_Input_Data_I       ),
    .B  ( iw_TrackingCarrier_Cos),
    .P  ( I_IF_mix_i_tmp        )
);

// I*-sin
TrackingDDC_Multiple I_ddc_multi_q_inst
(
    .CLK( iw_Clk_p_g            ),
    .A  ( iw_Input_Data_I       ),
    .B  (w_TrackingCarrier_Sin_op),
    .P  ( I_IF_mix_q_tmp        )
);
    
// Q*cos
TrackingDDC_Multiple Q_ddc_multi_i_inst
(
    .CLK( iw_Clk_p_g            ),
    .A  ( iw_Input_Data_Q       ),
    .B  ( iw_TrackingCarrier_Cos),
    .P  ( Q_IF_mix_i_tmp        )
);

// Q*sin
TrackingDDC_Multiple Q_ddc_multi_q_inst 
(
    .CLK( iw_Clk_p_g            ),
    .A  ( iw_Input_Data_Q       ),
    .B  ( iw_TrackingCarrier_Sin),
    .P  ( Q_IF_mix_q_tmp        )
);
    
endmodule
