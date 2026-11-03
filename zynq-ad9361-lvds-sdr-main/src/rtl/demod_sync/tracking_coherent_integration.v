`timescale 1ns / 1ps

module Tracking_Coherent_Integration
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
    
  //localparam          MAX_ACCUM_POINT = (GLOBAL_CLK_FRE_HZ*COHERENT_INTEGRATION_TIME_MS)/1000;    // 默认相干积分时间是1ms
    localparam [63:0]   MAX_ACCUM_POINT = (GLOBAL_CLK_FRE_HZ*COHERENT_INTEGRATION_TIME_US/1000)/1000;    // 14 默认相干积分时间是1ms
    localparam          ACCMU_REG_WD    = ((log2(MAX_ACCUM_POINT) + INPUT_DATA_WIDTH));
    localparam          SGNEXP_DWIDTH   = log2(MAX_ACCUM_POINT);
    localparam signed   TRUNCATE_WD     = ($signed(ACCMU_REG_WD-CORR_OUTPUT_DATA_WIDTH)>0) ? (ACCMU_REG_WD-CORR_OUTPUT_DATA_WIDTH) : 0;   // 输出结果需要截断位数,$signed函数很重要,否则modelsim仿真错误
    localparam signed   EXPANDED_WD     = ($signed(CORR_OUTPUT_DATA_WIDTH-ACCMU_REG_WD)>0) ? (CORR_OUTPUT_DATA_WIDTH-ACCMU_REG_WD) : 0;   // 输出结果需要扩展位数,$signed函数很重要,否则modelsim仿真错误

    reg signed [ACCMU_REG_WD-1:0] corr_result_reg;

    localparam VAL_MAX = {1'b0, {(INPUT_DATA_WIDTH-1){1'b1}}};
    localparam VAL_MIN = {1'b1, {(INPUT_DATA_WIDTH-1){1'b0}}};
   wire signed [INPUT_DATA_WIDTH-1:0] w_Input_Data_De;
    assign w_Input_Data_De = (iw_Input_Code)?((iw_Input_Data==VAL_MIN)?VAL_MAX:(-iw_Input_Data)):iw_Input_Data;     //解扩后的输入

    assign ow_Integration_Result_Valid = iw_Integration_Pulse;
    
    always @(posedge iw_Clk_p_g) 
    begin
        if(!iw_Rst_n_g)
        begin
            corr_result_reg <= {ACCMU_REG_WD{1'b0}};
        end
        else if(iw_Integration_Pulse)   // ow_Integration_Result 和 iw_Integration_Pulse 对齐
        begin
           corr_result_reg <= w_Input_Data_De;
        end
        else 
        begin
            corr_result_reg <= corr_result_reg+{{SGNEXP_DWIDTH{w_Input_Data_De[INPUT_DATA_WIDTH-1]}},w_Input_Data_De};
        end 
    end

    
    (*KEEP="TRUE"*) wire signed [ACCMU_REG_WD-1:0] corr_result;
    assign corr_result = corr_result_reg;

    //截掉符号位一定要限幅
    localparam DROP_HIGH_WD = 0;//log2(MAX_ACCUM_POINT)-12;    //截掉高位位宽 4bit
    //localparam DROP_HIGH_WD = 3;  //log2(MAX_ACCUM_POINT)-12;    //截掉高位位宽 
    localparam signed VAL_MAX_T = {{(DROP_HIGH_WD+1){1'b0}}, {(ACCMU_REG_WD-DROP_HIGH_WD-1){1'b1}}};
    localparam signed VAL_MIN_T = {{(DROP_HIGH_WD+1){1'b1}}, {(ACCMU_REG_WD-DROP_HIGH_WD-1){1'b0}}};
    wire signed [ACCMU_REG_WD-1:0] drop_high_corr_result = (corr_result>VAL_MAX_T)?VAL_MAX_T:((corr_result<VAL_MIN_T)?VAL_MIN_T:corr_result);
    // 处理扩位或者截位的情况,这里的截位与捕获模块的截位不同,这里的截位是取高位,相当于求平均
    generate
        if (EXPANDED_WD>0) 
            assign ow_Integration_Result = {{EXPANDED_WD{corr_result[ACCMU_REG_WD-1]}},corr_result};              // 此句没有错误,但modelsim报错,前面定义加上$signed,报错消失
        else if (TRUNCATE_WD==0) 
            assign ow_Integration_Result = corr_result;
        else 
            assign ow_Integration_Result = drop_high_corr_result[ACCMU_REG_WD-1-DROP_HIGH_WD:ACCMU_REG_WD-CORR_OUTPUT_DATA_WIDTH-DROP_HIGH_WD];  // 输入4bit,截取高 CORR_OUTPUT_DATA_WIDTH 位,即求平均,长时间观察调试发现,符号位最大可以在ACCMU_REG_WD-1-3位置
    endgenerate 
endmodule

