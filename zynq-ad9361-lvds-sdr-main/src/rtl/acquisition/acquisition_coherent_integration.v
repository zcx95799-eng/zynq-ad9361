`timescale 1ns / 1ps

module Acquisition_Coherent_Integration
#
(
    parameter   INPUT_DATA_WIDTH                = 20,   // 输入的DDC信号宽度
    parameter   MAX_ACCUM_POINT                 = 600,  // 最大累加点数
    parameter   CORR_OUTPUT_DATA_WIDTH          = 16    // 输出的相干积分结果宽度
)
(
    input                                       iw_Clk_p_g,
    input                                       iw_En_h,
    
    input                                       iw_Integration_Pulse,
    
    input signed [INPUT_DATA_WIDTH-1:0]         iw_Input_Data,
    input                                       iw_Input_Code,
    
    output                                      ow_Integration_Result_Valid,
    output signed [CORR_OUTPUT_DATA_WIDTH-1:0]  ow_Integration_Result  
);
    
//*************************************************************************************************************
    function integer log2(input integer n);
        integer i;     
        for( i=0; 2**i<=n; i=i+1) 
            log2 = i + 1;
    endfunction    
//*************************************************************************************************************
    
    localparam          ACCMU_REG_WD  = ((log2(MAX_ACCUM_POINT) + INPUT_DATA_WIDTH));
    localparam          SGNEXP_DWIDTH = log2(MAX_ACCUM_POINT);
    localparam signed   TRUNCATE_WD   = ($signed(ACCMU_REG_WD-CORR_OUTPUT_DATA_WIDTH)>0) ? (ACCMU_REG_WD-CORR_OUTPUT_DATA_WIDTH) : 0;   // 输出结果需要截断位数,$signed函数很重要,否则modelsim仿真错误
    localparam signed   EXPANDED_WD   = ($signed(CORR_OUTPUT_DATA_WIDTH-ACCMU_REG_WD)>0) ? (CORR_OUTPUT_DATA_WIDTH-ACCMU_REG_WD) : 0;   // 输出结果需要扩展位数,$signed函数很重要,否则modelsim仿真错误

    reg signed [ACCMU_REG_WD-1:0] corr_result_reg;
  
    assign ow_Integration_Result_Valid = iw_Integration_Pulse;
    
    always @(posedge iw_Clk_p_g) 
    begin
        if(!iw_En_h)
        begin
            corr_result_reg <= {ACCMU_REG_WD{1'b0}};
        end
        else if(iw_Integration_Pulse)   // ow_Integration_Result 和 iw_Integration_Pulse 对齐
        begin
            if(iw_Input_Code)
                corr_result_reg <=-{{SGNEXP_DWIDTH{iw_Input_Data[INPUT_DATA_WIDTH-1]}},iw_Input_Data};
            else
                corr_result_reg <= {{SGNEXP_DWIDTH{iw_Input_Data[INPUT_DATA_WIDTH-1]}},iw_Input_Data};
        end
        else 
        begin
            if(iw_Input_Code)
                corr_result_reg <= corr_result_reg-{{SGNEXP_DWIDTH{iw_Input_Data[INPUT_DATA_WIDTH-1]}},iw_Input_Data};
            else
                corr_result_reg <= corr_result_reg+{{SGNEXP_DWIDTH{iw_Input_Data[INPUT_DATA_WIDTH-1]}},iw_Input_Data};
        end 
    end
    
    wire signed [ACCMU_REG_WD-1:0] corr_result;
    
    assign corr_result = corr_result_reg;
 
    // 处理扩位或者截位的情况
    generate
        if (EXPANDED_WD>0) 
            assign ow_Integration_Result = {{EXPANDED_WD{corr_result[ACCMU_REG_WD-1]}},corr_result};              // 此句没有错误,但modelsim报错,前面定义加上$signed,报错消失
        else if (TRUNCATE_WD==0) 
            assign ow_Integration_Result = corr_result;
        else 
            assign ow_Integration_Result = ((corr_result[ACCMU_REG_WD-1:CORR_OUTPUT_DATA_WIDTH-1]=={(TRUNCATE_WD+1){1'b0}}) || 
                                            (corr_result[ACCMU_REG_WD-1:CORR_OUTPUT_DATA_WIDTH-1]=={(TRUNCATE_WD+1){1'b1}}))
                                          ?  corr_result[CORR_OUTPUT_DATA_WIDTH-1:0]:{corr_result[ACCMU_REG_WD-1],{(CORR_OUTPUT_DATA_WIDTH-1){~corr_result[ACCMU_REG_WD-1]}}};
    endgenerate 
       
endmodule