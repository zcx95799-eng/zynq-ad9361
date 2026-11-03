`timescale 1ns / 1ps


module Tracking_Code_Loop_Filter
#
(
    parameter   CORR_OUTPUT_DATA_WIDTH          = 32'd19,       // 相干积分位宽,37=log2(120_000)+INPUT_DATA_WIDTH+LOCAL_CARRIER_NCO_OUTPUT_WIDTH
        
    parameter   DLL_COEFFICIENT_WIDTH           = 32,           // 环路滤波器系数位宽,提前算好
        
    parameter   PN_CODE_NCO_PHASE_WIDTH         = 32'd64        // 伪码NCO相位控制字位宽
)       
(       
    input                                       iw_Clk_p_g,
    input                                       iw_Rst_n_g,
            
    input [DLL_COEFFICIENT_WIDTH-1:0]           iw_DLL_C1,
    input [DLL_COEFFICIENT_WIDTH-1:0]           iw_DLL_C2,
            
    input                                       iw_Code_Error_Rdy_h,
    input  signed [CORR_OUTPUT_DATA_WIDTH-1:0]  iw_Code_Error, 

	output                                      ow_Code_Loop_Output_Valid,       
	output [PN_CODE_NCO_PHASE_WIDTH-1:0]        ow_Code_Loop_Output,
    output [PN_CODE_NCO_PHASE_WIDTH-1:0]        ow_Code_Doppler
);
//*************************************************************************************************************
    function integer log2(input integer n);
        integer i;     
        for( i=0; 2**i<=n; i=i+1) 
            log2 = i + 1;
    endfunction    
//*************************************************************************************************************
        
    wire signed [CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH-1:0] DLL_C1_Product;  
    wire signed [CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH-1:0] DLL_C2_Product;    
    Multiplier_s38xs26 Multi_DLL_C1 (
        .CLK  (iw_Clk_p_g),
        //.SCLR (~iw_Rst_n_g),
        .A    (iw_Code_Error),
        .B    (iw_DLL_C1[31:0]),
        .P    (DLL_C1_Product)
    );

    Multiplier_s38xs26 Multi_DLL_C2 (
        .CLK  (iw_Clk_p_g),
        //.SCLR (~iw_Rst_n_g),
        .A    (iw_Code_Error),
        .B    (iw_DLL_C2[31:0]),
        .P    (DLL_C2_Product)
    );
//*************************************************************************************************************

   // reg [15:0] r_Code_Error_Rdy_h_Sync;
    
    //always @(posedge iw_Clk_p_g) begin
       // if (!iw_Rst_n_g)
           // r_Code_Error_Rdy_h_Sync <= 16'd0;
        //else
           // r_Code_Error_Rdy_h_Sync <= {
              //  r_Code_Error_Rdy_h_Sync[14:0],
              //  iw_Code_Error_Rdy_h
          //  };
   // end
        reg [15:0] r_Code_Error_Rdy_h_Sync;
        
        always@(posedge iw_Clk_p_g)
        begin
            r_Code_Error_Rdy_h_Sync <= {r_Code_Error_Rdy_h_Sync[14:0],iw_Code_Error_Rdy_h};
        end

    reg signed [CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH-1:0] DLL_s0;
    reg signed [CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH-1:0] DLL_s0_Cpy;
    reg signed [CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH-1:0] DLL_s1;

    reg [1:0] state;

    reg signed [CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH-1:0] r_Code_Doppler;

    reg signed [CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH-1:0] r_DLL_C1_Product;
    reg signed [CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH-1:0] r_DLL_C2_Product;  
    
    always@(posedge iw_Clk_p_g )
    begin
        if(!iw_Rst_n_g)
        begin
            DLL_s1          <= {(CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH){1'b0}};
            DLL_s0          <= {(CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH){1'b0}};
            DLL_s0_Cpy      <= {(CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH){1'b0}};
            
            r_Code_Doppler  <= {(CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH){1'b0}};
            state           <= 2'd0;
            
            r_DLL_C1_Product<= {(CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH){1'b0}};
            r_DLL_C2_Product<= {(CORR_OUTPUT_DATA_WIDTH+DLL_COEFFICIENT_WIDTH){1'b0}};
        end
        else if(state==2'd0)
        begin 
            if(r_Code_Error_Rdy_h_Sync[0])  // 乘法器延迟1个时钟
            begin
                state <= 2'd1;
                
                r_DLL_C1_Product <= DLL_C1_Product;//>>>30;
                r_DLL_C2_Product <= DLL_C2_Product;//>>>20;
            end
        end 
        else if(state==2'd1)
        begin
            if(r_Code_Error_Rdy_h_Sync[1])
            begin     
                DLL_s0      <= DLL_s0 + r_DLL_C1_Product;   // 更新DLL_s0
                DLL_s0_Cpy  <= DLL_s0;                      // 拷贝DLL_s0,用于下一步运算
            end
            else if(r_Code_Error_Rdy_h_Sync[2])
            begin
                r_Code_Doppler <=  DLL_s0 + DLL_s0_Cpy; // 环路结构:二阶环,双线性变换
            end
            else if(r_Code_Error_Rdy_h_Sync[3])
            begin
                DLL_s1  <= r_Code_Doppler + r_DLL_C2_Product;
            end
            else if(r_Code_Error_Rdy_h_Sync[4])
            begin
                state <= 2'd0;
            end
        end
    end

//*************************************************************************************************************

    assign ow_Code_Loop_Output_Valid = r_Code_Error_Rdy_h_Sync[4];
    assign ow_Code_Doppler           = r_Code_Doppler[PN_CODE_NCO_PHASE_WIDTH-1:0];
    assign ow_Code_Loop_Output       = DLL_s1>>>20;                      

//*************************************************************************************************************    
endmodule


