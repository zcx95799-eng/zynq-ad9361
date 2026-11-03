`timescale 1ns / 1ps

module Tracking_Coherent_Integration_Top
#
(
    parameter   GLOBAL_CLK_FRE_HZ               = 32'd120000000,    // 输入时钟120MHz,即采样率 
    parameter   COHERENT_INTEGRATION_TIME_MS    = 1,                // 相干积分1ms
    parameter   COHERENT_INTEGRATION_TIME_US    = 250,              // 相干积分250us
    
    parameter   INPUT_DATA_WIDTH                = 20,               // 输入的DDC信号宽度
    parameter   CORR_OUTPUT_DATA_WIDTH          = 40                // 输出的相干积分结果宽度
)
(
    input                                       iw_Clk_p_g,
    input                                       iw_Rst_n_g,
    
    input                                       iw_Integration_Pulse,
    
    input signed [INPUT_DATA_WIDTH-1:0]         iw_IF_mix_Q_i, 
    input signed [INPUT_DATA_WIDTH-1:0]         iw_IF_mix_Q_q,
    
    input                                       iw_Coherent_Integration_Data_E,
    input                                       iw_Coherent_Integration_Data_P,
    input                                       iw_Coherent_Integration_Data_L,
    
    output                                      ow_Integration_Result_Valid,

    output signed [CORR_OUTPUT_DATA_WIDTH-1:0]  ow_Integration_Result_I_E,     
    output signed [CORR_OUTPUT_DATA_WIDTH-1:0]  ow_Integration_Result_I_P,     
    output signed [CORR_OUTPUT_DATA_WIDTH-1:0]  ow_Integration_Result_I_L,     
    output signed [CORR_OUTPUT_DATA_WIDTH-1:0]  ow_Integration_Result_Q_E,     
    output signed [CORR_OUTPUT_DATA_WIDTH-1:0]  ow_Integration_Result_Q_P,     
    output signed [CORR_OUTPUT_DATA_WIDTH-1:0]  ow_Integration_Result_Q_L    
);

    Tracking_Coherent_Integration
                                #
                                (
                                    .GLOBAL_CLK_FRE_HZ(GLOBAL_CLK_FRE_HZ),                          // 输入时钟120MHz,即采样率 
                                  //.COHERENT_INTEGRATION_TIME_MS(COHERENT_INTEGRATION_TIME_MS),    // 相干积分1ms    
                                    .COHERENT_INTEGRATION_TIME_US(COHERENT_INTEGRATION_TIME_US),
                                    
                                    .INPUT_DATA_WIDTH(INPUT_DATA_WIDTH),                            // 输入的DDC信号宽度
                                    .CORR_OUTPUT_DATA_WIDTH(CORR_OUTPUT_DATA_WIDTH)                 // 输出的相干积分结果宽度
                                )
                                Tracking_Coherent_Integration_I_E
                                (
                                    .iw_Clk_p_g(iw_Clk_p_g),
                                    .iw_Rst_n_g(iw_Rst_n_g),
                                    
                                    .iw_Integration_Pulse(iw_Integration_Pulse),                    // 注意:相干积分脉冲是P支路同步脉冲
                                    
                                    .iw_Input_Data(iw_IF_mix_Q_i),
                                    .iw_Input_Code(iw_Coherent_Integration_Data_E),
                                    
                                    .ow_Integration_Result_Valid(),
                                    .ow_Integration_Result(ow_Integration_Result_I_E)  
                                );

    Tracking_Coherent_Integration
                                #
                                (
                                    .GLOBAL_CLK_FRE_HZ(GLOBAL_CLK_FRE_HZ),                          // 输入时钟120MHz,即采样率 
                                  //.COHERENT_INTEGRATION_TIME_MS(COHERENT_INTEGRATION_TIME_MS),    // 相干积分1ms    
                                    .COHERENT_INTEGRATION_TIME_US(COHERENT_INTEGRATION_TIME_US),
                                    
                                    .INPUT_DATA_WIDTH(INPUT_DATA_WIDTH),                            // 输入的DDC信号宽度
                                    .CORR_OUTPUT_DATA_WIDTH(CORR_OUTPUT_DATA_WIDTH)                 // 输出的相干积分结果宽度
                                )
                                Tracking_Coherent_Integration_I_P
                                (
                                    .iw_Clk_p_g(iw_Clk_p_g),
                                    .iw_Rst_n_g(iw_Rst_n_g),
                                    
                                    .iw_Integration_Pulse(iw_Integration_Pulse),
                                    
                                    .iw_Input_Data(iw_IF_mix_Q_i),
                                    .iw_Input_Code(iw_Coherent_Integration_Data_P),
                                    
                                    .ow_Integration_Result_Valid(ow_Integration_Result_Valid),
                                    .ow_Integration_Result(ow_Integration_Result_I_P)  
                                ); 

    Tracking_Coherent_Integration
                                #
                                (
                                    .GLOBAL_CLK_FRE_HZ(GLOBAL_CLK_FRE_HZ),                          // 输入时钟120MHz,即采样率 
                                  //.COHERENT_INTEGRATION_TIME_MS(COHERENT_INTEGRATION_TIME_MS),    // 相干积分1ms    
                                    .COHERENT_INTEGRATION_TIME_US(COHERENT_INTEGRATION_TIME_US),
                                    
                                    .INPUT_DATA_WIDTH(INPUT_DATA_WIDTH),                            // 输入的DDC信号宽度
                                    .CORR_OUTPUT_DATA_WIDTH(CORR_OUTPUT_DATA_WIDTH)                 // 输出的相干积分结果宽度
                                )
                                Tracking_Coherent_Integration_I_L
                                (
                                    .iw_Clk_p_g(iw_Clk_p_g),
                                    .iw_Rst_n_g(iw_Rst_n_g),
                                    
                                    .iw_Integration_Pulse(iw_Integration_Pulse),                    // 注意:相干积分脉冲是P支路同步脉冲
                                    
                                    .iw_Input_Data(iw_IF_mix_Q_i),
                                    .iw_Input_Code(iw_Coherent_Integration_Data_L),
                                    
                                    .ow_Integration_Result_Valid(),
                                    .ow_Integration_Result(ow_Integration_Result_I_L)  
                                );  

    Tracking_Coherent_Integration
                                #
                                (
                                    .GLOBAL_CLK_FRE_HZ(GLOBAL_CLK_FRE_HZ),                          // 输入时钟120MHz,即采样率 
                                  //.COHERENT_INTEGRATION_TIME_MS(COHERENT_INTEGRATION_TIME_MS),    // 相干积分1ms    
                                    .COHERENT_INTEGRATION_TIME_US(COHERENT_INTEGRATION_TIME_US),
                                    
                                    .INPUT_DATA_WIDTH(INPUT_DATA_WIDTH),                            // 输入的DDC信号宽度
                                    .CORR_OUTPUT_DATA_WIDTH(CORR_OUTPUT_DATA_WIDTH)                 // 输出的相干积分结果宽度
                                )
                                Tracking_Coherent_Integration_Q_E
                                (
                                    .iw_Clk_p_g(iw_Clk_p_g),
                                    .iw_Rst_n_g(iw_Rst_n_g),
                                    
                                    .iw_Integration_Pulse(iw_Integration_Pulse),                    // 注意:相干积分脉冲是P支路同步脉冲
                                    
                                    .iw_Input_Data(iw_IF_mix_Q_q),
                                    .iw_Input_Code(iw_Coherent_Integration_Data_E),
                                    
                                    .ow_Integration_Result_Valid(),
                                    .ow_Integration_Result(ow_Integration_Result_Q_E)  
                                );
                                
    Tracking_Coherent_Integration
                                #
                                (
                                    .GLOBAL_CLK_FRE_HZ(GLOBAL_CLK_FRE_HZ),                          // 输入时钟120MHz,即采样率 
                                  //.COHERENT_INTEGRATION_TIME_MS(COHERENT_INTEGRATION_TIME_MS),    // 相干积分1ms    
                                    .COHERENT_INTEGRATION_TIME_US(COHERENT_INTEGRATION_TIME_US),
                                    
                                    .INPUT_DATA_WIDTH(INPUT_DATA_WIDTH),                            // 输入的DDC信号宽度
                                    .CORR_OUTPUT_DATA_WIDTH(CORR_OUTPUT_DATA_WIDTH)                 // 输出的相干积分结果宽度
                                )
                                Tracking_Coherent_Integration_Q_P
                                (
                                    .iw_Clk_p_g(iw_Clk_p_g),
                                    .iw_Rst_n_g(iw_Rst_n_g),
                                    
                                    .iw_Integration_Pulse(iw_Integration_Pulse),
                                    
                                    .iw_Input_Data(iw_IF_mix_Q_q),
                                    .iw_Input_Code(iw_Coherent_Integration_Data_P),
                                    
                                    .ow_Integration_Result_Valid(),
                                    .ow_Integration_Result(ow_Integration_Result_Q_P)  
                                );                                

    Tracking_Coherent_Integration
                                #
                                (
                                    .GLOBAL_CLK_FRE_HZ(GLOBAL_CLK_FRE_HZ),                          // 输入时钟120MHz,即采样率 
                                  //.COHERENT_INTEGRATION_TIME_MS(COHERENT_INTEGRATION_TIME_MS),    // 相干积分1ms    
                                    .COHERENT_INTEGRATION_TIME_US(COHERENT_INTEGRATION_TIME_US),
                                    
                                    .INPUT_DATA_WIDTH(INPUT_DATA_WIDTH),                            // 输入的DDC信号宽度
                                    .CORR_OUTPUT_DATA_WIDTH(CORR_OUTPUT_DATA_WIDTH)                 // 输出的相干积分结果宽度
                                )
                                Tracking_Coherent_Integration_Q_L
                                (
                                    .iw_Clk_p_g(iw_Clk_p_g),
                                    .iw_Rst_n_g(iw_Rst_n_g),
                                    
                                    .iw_Integration_Pulse(iw_Integration_Pulse),                    // 注意:相干积分脉冲是P支路同步脉冲
                                    
                                    .iw_Input_Data(iw_IF_mix_Q_q),
                                    .iw_Input_Code(iw_Coherent_Integration_Data_L),
                                    
                                    .ow_Integration_Result_Valid(),
                                    .ow_Integration_Result(ow_Integration_Result_Q_L)  
                                );

endmodule
