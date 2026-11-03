
`timescale 1ns / 1ps

module Acquisition_Downsampling_Top
#(
    parameter GLOBAL_CLK_FRE_HZ                         = 32'd120000000,                // 输入时钟120MHz,即采样率
    parameter INPUT_DATA_WIDTH                          = 32'd16,                       // 输入数据位宽
                                                  
    parameter DOWN_SAMPLE_OUTPUT_RATE_HZ                = 32'd12000000,                 // 降采样之后数据输出�?�率
    parameter DOWN_SAMPLE_OUTPUT_DATA_WIDTH             = 32'd32                        // 降采样之后数据输出位�?,等于 INPUT_DATA_WIDTH+LOCAL_CARRIER_NCO_OUTPUT_WIDTH
)       
(       
    input                                               iw_Clk_p_g,                     // 输入采样时钟
    input                                               iw_Rst_n_g,                                      
            
    //input   signed [INPUT_DATA_WIDTH-1:0]               iw_Input_Data,  

    input   signed [DOWN_SAMPLE_OUTPUT_DATA_WIDTH-1:0]  iw_I,
    input   signed [DOWN_SAMPLE_OUTPUT_DATA_WIDTH-1:0]  iw_Q,
        
    input                                               iw_Downsample_Pulse,            // 降采样脉�?,也是输出数据有效信号,与输出数据对�?
                
    output                                              ow_Output_Valid_Pulse,          // 输出数据有效信号,与输出数据对�?,本质上就是iw_Downsample_Pulse
    // 降采样输�?
   output  signed [DOWN_SAMPLE_OUTPUT_DATA_WIDTH-1:0]  ow_Downsample_Output_I,         // 降采样输出I支路数据
    output  signed [DOWN_SAMPLE_OUTPUT_DATA_WIDTH-1:0]  ow_Downsample_Output_Q          // 降采样输出Q支路数据

);

    localparam SUM_NUM = GLOBAL_CLK_FRE_HZ/DOWN_SAMPLE_OUTPUT_RATE_HZ;
    
    Acquisition_Downsampling #(
                                .INPUT_DATA_WIDTH(DOWN_SAMPLE_OUTPUT_DATA_WIDTH),
                                .OUTPUT_DATA_WIDTH(DOWN_SAMPLE_OUTPUT_DATA_WIDTH),
                                .SUM_NUM(SUM_NUM)
                            )
                            Acquisition_Downsampling_Inst1(
                                                            .iw_Clk_p_g     (iw_Clk_p_g),           
                                                            .iw_Rst_n_g     (iw_Rst_n_g), 
                                                            
                                                            .iw_Input_Data  (iw_I),

                                                            .iw_Downsample_Pulse(iw_Downsample_Pulse),
                                                            
                                                            .ow_Output_Valid_Pulse(ow_Output_Valid_Pulse),
                                                            .ow_Output_Data (ow_Downsample_Output_I)
                                                        );
                                                        
    Acquisition_Downsampling #(
                                .INPUT_DATA_WIDTH(DOWN_SAMPLE_OUTPUT_DATA_WIDTH),
                                .OUTPUT_DATA_WIDTH(DOWN_SAMPLE_OUTPUT_DATA_WIDTH),
                                .SUM_NUM(SUM_NUM)
                            )
                            Acquisition_Downsampling_Inst2(
                                                            .iw_Clk_p_g     (iw_Clk_p_g),           
                                                            .iw_Rst_n_g     (iw_Rst_n_g), 
                                                            
                                                            .iw_Input_Data  (iw_Q),  

                                                            .iw_Downsample_Pulse(iw_Downsample_Pulse),
                                                            
                                                            .ow_Output_Valid_Pulse(),
                                                            .ow_Output_Data (ow_Downsample_Output_Q)
                                                        );
                                                        
endmodule
