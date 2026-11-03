`timescale 1ns / 1ps



module ad9361_lvds(

    // system signals
	input	wire            cfg_done        ,
	output	wire            data_clk        ,//(输出时钟信号)接收和发送数据的时钟
	// ad936x rx path signals
	input	wire 			rx_clk_in_p	    ,//接收9361数据的时钟信号(差分)
	input	wire 			rx_clk_in_n		,
	input	wire	[5:0] 	rx_data_in_p	,//接收来自9361的数据(每个data_clk边沿接收6bit数据)(差分)
	input	wire 	[5:0]	rx_data_in_n	,
	input	wire 			rx_frame_in_p	,//rx frame接收控制信号(差分)
	input	wire 			rx_frame_in_n	,
	// ad936x tx path signals
	output  wire 			tx_clk_out_p	,//发送数据时钟信号(差分)
	output  wire 			tx_clk_out_n	,
	output	wire 			tx_frame_out_p	,//tx frame 发送控制信号(差分)
	output	wire 			tx_frame_out_n	,
	output	wire 	[5:0]	tx_data_out_p	,//发送的数据(差分)
	output	wire 	[5:0]	tx_data_out_n	,
	// adc sample iq - 解析后的IQ数据
	output	reg 			adc_valid		,//接收数据valid信号, 详见时序图
	output 	reg 			adc_status		,//对rx frame信号的正确性检测 1:error 0:right
	output	reg 	[11:0]	adc_data_i1		,//解析后的接收数据:I1路
	output	reg 	[11:0]	adc_data_q1		,//解析后的接收数据:Q1路
	output	reg 	[11:0]	adc_data_i2		,//解析后的接收数据:I2路
	output	reg 	[11:0]	adc_data_q2		,//解析后的接收数据:Q2路
	// dac sample iq - 需要发送的IQ数据
	input 	wire 			dac_valid		,//发送数据valid信号, 详见时序图
	input 	wire 	[11:0]	dac_data_i1		,//DDS输入的I路数据
	input 	wire 	[11:0]	dac_data_q1		,//DDS输入的Q路数据
	input 	wire 	[11:0]	dac_data_i2		,//DDS输入的I路数据(目前输入的两路数据一样)
	input 	wire 	[11:0]	dac_data_q2		,//DDS输入的Q路数据
	//external ctrl signals
	input 	wire 			adc_mode_r1		,//==1 is r1 mode;  == 0 is r2 mode
	input 	wire 			dac_mode_r1	     //==1 is t1 mode;  == 0 is t2 mode
);

    wire data_clk_ibuf;
    //-------------------------------------------------------------------
	// 将输入的差分时钟转换为单端时钟
	//-------------------------------------------------------------------
    //1.将 rx_clk_in_p/n 差分信号转化为单端信号 data_clk_ibuf  1r1t:80m  2r2t: 160m
    IBUFDS #(
        .DIFF_TERM("TRUE"),    // Differential Termination 使能终端电阻100欧姆
        .IBUF_LOW_PWR("TRUE"),  // Low power="TRUE", Highest performance="FALSE" 
        .IOSTANDARD("DEFAULT")  // Specify the input I/O standard
    ) IBUFDS_data_clk_inst (
        .O(data_clk_ibuf), // Buffer output
        .I(rx_clk_in_p),   // Diff_p buffer input (connect directly to top-level port)
        .IB(rx_clk_in_n)   // Diff_n buffer input (connect directly to top-level port)
     );
    //2.时钟输出
    BUFG  data_clk1_bufg(.O(data_clk),.I(data_clk_ibuf));
    
    wire rx_frame_ibuf;
    wire rx_frame_idelay;
    wire rx_frame_iddr_p;
    wire rx_frame_iddr_n;
    //-------------------------------------------------------------------
	// 将输入的差分帧信号转换为单端帧信号
	//-------------------------------------------------------------------
    //1.将输入的差分信号 rx_frame_in_p/n 转换为单端信号 rx_frame_ibuf
    IBUFDS #(
        .DIFF_TERM("TRUE"),    // Differential Termination
        .IBUF_LOW_PWR("TRUE"),  // Low power="TRUE", Highest performance="FALSE" 
        .IOSTANDARD("DEFAULT")  // Specify the input I/O standard
    ) IBUFDS_rx_frame_inst (
        .O(rx_frame_ibuf), // Buffer output
        .I(rx_frame_in_p), // Diff_p buffer input (connect directly to top-level port)
        .IB(rx_frame_in_n) // Diff_n buffer input (connect directly to top-level port)
    );
    //3.将输入的串行信号:rx_frame_idelay 转换为并行信号:rx_frame_iddr_p/n
    IDDR #(
        .DDR_CLK_EDGE("SAME_EDGE_PIPELINED"), // "OPPOSITE_EDGE", "SAME_EDGE" or "SAME_EDGE_PIPELINED" 
        .INIT_Q1(1'b0),  // Initial value of Q1: 1'b0 or 1'b1 默认输出0
        .INIT_Q2(1'b0),  // Initial value of Q2: 1'b0 or 1'b1 默认输出0
        .SRTYPE("SYNC") // Set/Reset type: "SYNC" or "ASYNC" 
     ) IDDR_rx_frame_inst (
        .Q1(rx_frame_iddr_p), // 1-bit output for positive edge of clock
        .Q2(rx_frame_iddr_n), // 1-bit output for negative edge of clock
        .C(data_clk),         // 1-bit clock input
        .CE(1'b1),            // 1-bit clock enable input
        .D(rx_frame_ibuf),    // 1-bit DDR data input
        .R(1'b0),             // 1-bit reset
        .S(1'b0)              // 1-bit set
    );
    
    wire [5:0] 	rx_data_ibuf;
    wire [5:0] 	rx_data_idelay;
    wire [5:0] 	rx_data_iddr_p;
    wire [5:0] 	rx_data_iddr_n;
    //-------------------------------------------------------------------
	// 将输入的差分数据信号转换为单端数据信号
	//-------------------------------------------------------------------
	generate
    genvar rx_lane;
    for (rx_lane = 0; rx_lane < 6; rx_lane = rx_lane + 1) begin : gen_rx_lane
        //1.将输入的差分信号 rx_data_in_p/n 转换为单端信号 rx_data_ibuf
        IBUFDS #(
            .DIFF_TERM("TRUE"),        // Differential Termination
            .IBUF_LOW_PWR("TRUE"),     // Low power="TRUE", Highest performance="FALSE" 
            .IOSTANDARD("DEFAULT")     // Specify the input I/O standard
        ) IBUFDS_rx_data_inst (
            .O( rx_data_ibuf[rx_lane]),   // Buffer output
            .I( rx_data_in_p[rx_lane]),   // Diff_p buffer input (connect directly to top-level port)
            .IB(rx_data_in_n[rx_lane])    // Diff_n buffer input (connect directly to top-level port)
        );
        //3.将输入的串行信号:rx_data_idelay 转换为并行信号:rx_data_iddr_p/n
        IDDR #(
            .DDR_CLK_EDGE("SAME_EDGE_PIPELINED"), // "OPPOSITE_EDGE", "SAME_EDGE" or "SAME_EDGE_PIPELINED" 
            .INIT_Q1(1'b0),                         // Initial value of Q1: 1'b0 or 1'b1
            .INIT_Q2(1'b0),                         // Initial value of Q2: 1'b0 or 1'b1
            .SRTYPE("SYNC")                        // Set/Reset type: "SYNC" or "ASYNC" 
        ) IDDR_rx_data_inst (
            .Q1(rx_data_iddr_p[rx_lane]),               // 1-bit output for positive edge of clock
            .Q2(rx_data_iddr_n[rx_lane]),               // 1-bit output for negative edge of clock
            .C(data_clk),                         // 1-bit clock input
            .CE(1'b1),                            // 1-bit clock enable input
            .D(rx_data_ibuf[rx_lane]),                // 1-bit DDR data input
            .R(1'b0),                             // 1-bit reset
            .S(1'b0)                              // 1-bit set
        );
    end
    endgenerate
    
	reg  [3:0] 	 rx_frame_shift;
    reg  [23:0]	 rx_data_shift;   //接收数据解析
    //-------------------------------------------------------------------
    // 对 frame 和 data 信号进行拼接
    //-------------------------------------------------------------------
    // 1.rx frame 信号的拼接为: rx_frame_shift
    // 1r1t: xx11->1100->0011->1100->0011->1100(有效)->0011->1100(有效)->0011->1100(有效)
    // 2r2t: xx11->1111->1100->0000(有效)->0011->1111->1100->0000(有效)->0011->1111->1100->0000(有效)
    always @(posedge data_clk) begin
        rx_frame_shift <= {rx_frame_shift[1:0],rx_frame_iddr_p,rx_frame_iddr_n};
    end
    // 2.rx data 信号的拼接为 rx_data_shift
    // 1r1t：{x,x,IM,QM}->{IM,QM,IL,QL}(有效）->{IL,QL,IM,QM}->{IM,QM,IL,QL}(有效)->{IL,QL,IM,QM}->{IM,QM,IL,QL}(有效)
    // 2r2t: {x,x,I1M,QIM}->{I1M,Q1M,I1L,Q1L}(可解析通道1)->{I1L,Q1L,I2M,Q2M}->{I2M,Q2M,I2L,Q2L}(可解析通道2)->{I2L,Q2L,I1M,QIM}->{I1M,Q1M,I1L,Q1L}(可解析通道1)
    always @(posedge data_clk) begin
        rx_data_shift <= {rx_data_shift[11:0],rx_data_iddr_p,rx_data_iddr_n};
    end
    
    reg 		 rx_valid_r1=0;  // 1r1t mode data valid
    reg 		 rx_error_r1=0;  // frame error
    reg  [11:0]	 rx_data_i_r1=0; // 1r1t mode data i 
    reg  [11:0]	 rx_data_q_r1=0; // 1r1t mode data q
    //-------------------------------------------------------------------
    // 单通道接收数据解析
    //-------------------------------------------------------------------
	// 1.rx valid 1r1t 每两个 data_clk 内有效一次
    always @(posedge data_clk) begin
        if(rx_frame_shift == 4'b1100) begin
            rx_valid_r1 <= 1'b1;
        end
        else begin
            rx_valid_r1 <= 1'b0;
        end
    end
	// 2.判断:rx_frame_shift 拼接结果是否出错(1r1t mode: 1100 or 0011)
    always @(posedge data_clk) begin
        rx_error_r1 <= (rx_frame_shift==4'b1100 || rx_frame_shift == 4'b0011)?1'b0:1'b1;
    end
    // 3.rx data 数据解析,rx_frame_shift==1100时, rx_data_shift == {IM[23:18],QM[17:12],IL[11:6],QL[5:0]}
    // I路数据:{IM,IL} Q路数据:{QM,QL}
    always @(posedge data_clk) begin
        if(rx_frame_shift==4'b1100) begin
            rx_data_i_r1 <= {rx_data_shift[23:18],rx_data_shift[11:6]}; //rx_data_i_r1 = {IM,IL}
            rx_data_q_r1 <= {rx_data_shift[17:12],rx_data_shift[5:0]};  //rx_data_q_r1 = {QM,QL}
        end
    end
    
    reg 		 rx_valid_r2=0;  // 2r2t mode data valid
    reg 		 rx_error_r2=0;	 // frame error
    reg [11:0]	 rx_data_i1_r2=0;// 2r2t mode data i1
    reg [11:0]	 rx_data_q1_r2=0;// 2r2t mode data q1
    reg [11:0]	 rx_data_i2_r2=0;// 2r2t mode data i2
    reg [11:0]	 rx_data_q2_r2=0;// 2r2t mode data q2
    //-------------------------------------------------------------------
    // 双通道接收数据解析
    //-------------------------------------------------------------------
    // 1.rx valid 2r2t 每4个 data_clk 内有效一次
    always @(posedge data_clk) begin
        if(rx_frame_shift==4'b0000) begin
            rx_valid_r2 <= 1'b1;
        end
        else begin
            rx_valid_r2 <= 1'b0;
        end
    end
    // 2.判断:rx_frame_shift 拼接结果是否出错(2r2t mode: 0011 or 1111 or 1100 or 0000)
    always @(posedge data_clk ) begin
        rx_error_r2 <= (rx_frame_shift == 4'b0011 || rx_frame_shift ==4'b1111 || rx_frame_shift == 4'b1100 || rx_frame_shift == 4'b0000)?1'b0:1'b1;
    end
    // 3.rx_frame_shift == 1111,通道1,rx_data_shift=={I1M,Q1M,I1L,Q1L}
    always @(posedge data_clk) begin
        if(rx_frame_shift == 4'b1111) begin
            rx_data_i1_r2 <= {rx_data_shift[23:18],rx_data_shift[11:6]};
            rx_data_q1_r2 <= {rx_data_shift[17:12],rx_data_shift[5:0]};
        end
    end
    // 4.rx_frame_shift == 0000,通道2,rx_data_shift=={I2M,Q2M,I2L,Q2L}
    always @(posedge data_clk) begin
        if(rx_frame_shift == 4'b0000) begin
            rx_data_i2_r2 <= {rx_data_shift[23:18],rx_data_shift[11:6]};
            rx_data_q2_r2 <= {rx_data_shift[17:12],rx_data_shift[5:0]};
        end
    end
     
    //-------------------------------------------------------------------
    // 保存对接收到的数据进行解析后的结果
    //-------------------------------------------------------------------
    //r1 r2 mux, 通道选择,当 adc_mode_r1==1时, 为1r1t
    always @(posedge data_clk) begin
    if(adc_mode_r1 == 1'b1) begin//1R1T
        adc_valid   <= rx_valid_r1;
        adc_status  <= rx_error_r1;
        adc_data_i1 <= rx_data_i_r1;
        adc_data_q1 <= rx_data_q_r1;
        adc_data_i2 <= 'd0;
        adc_data_q2 <= 'd0;
    end
    else begin//2R2T
        adc_valid   <= rx_valid_r2;
        adc_status  <= rx_error_r2;
        adc_data_i1 <= rx_data_i1_r2;
        adc_data_q1 <= rx_data_q1_r2;
        adc_data_i2 <= rx_data_i2_r2;
        adc_data_q2 <= rx_data_q2_r2;
    end
    end

//tx
reg 		 tx_data_cnt_flag=0;
reg  [1:0]	 tx_data_cnt=0;
reg  [11:0]	 tx_data_i1_d=0;
reg  [11:0]	 tx_data_q1_d=0;
reg  [11:0]	 tx_data_i2_d=0;
reg  [11:0]	 tx_data_q2_d=0;
reg  [5:0]	 tx_data_p;
reg  [5:0]	 tx_data_n;
reg 		 tx_frame;
wire [3:0]	 tx_data_sel;

wire 		 tx_clk;
wire 		 tx_frame_out;
wire [5:0]	 tx_data_out;
// ----------------------------------------------------- dac code --------------------------------------------------------------------------- //
//1r1t mode 2r2t mode
// 发送标志 tx_data_cnt_flag,置1时开启发送数据
always @(posedge data_clk ) begin
//// 1r1t,发送完成,此时计数值 tx_data_cnt==1,清除发送数据标志位
	if(dac_valid == 1'b0 && tx_data_cnt_flag == 1'b1 && tx_data_cnt == 2'b1 && dac_mode_r1 == 1'b1)begin
		tx_data_cnt_flag <= 1'b0;
	end
//// 2r2t,发送完成，此时计数值 tx_data_cnt==3,清除发送数据标志位
	else if(dac_valid == 1'b0 && tx_data_cnt_flag == 1'b1 && tx_data_cnt == 2'd3 && dac_mode_r1 == 1'b0)begin
		tx_data_cnt_flag <= 1'b0;
	end
//// 需要发送的数据准备传入时,dac_valid == 1, 发送标志位置1
	else if(dac_valid == 1'b1) begin
		tx_data_cnt_flag <= 1'b1;
	end
end
//发送计数值:tx_data_cnt,1r1t 时，tx_data_cnt == 1完成一次发送
////2r2t时, tx_data_cnt == 3完成一次发送
always @(posedge data_clk ) begin
	// 1r1t模式，2个 data_clk 后, 完成发送，计数值 tx_data_cnt==1,清除发送计数值
	if(tx_data_cnt_flag == 1'b1 && tx_data_cnt == 2'b1 && dac_mode_r1 == 1'b1) begin //1r1t clear 
		tx_data_cnt <= 'd0;
	end
	// 2r2t模式，4个 data_clk 后, 完成发送，计数值 tx_data_cnt==3，清除发送计数值
	else if(tx_data_cnt_flag == 1'b1 && tx_data_cnt == 2'd3 && dac_mode_r1 == 1'b0)begin //2r2t clear
		tx_data_cnt <= 'd0;
	end
	// 在发送标志 tx_data_cnt_flag==1期间, tx_data_cnt 累加进行发送状态计数
	else if(tx_data_cnt_flag == 1'b1 ) begin
		tx_data_cnt <= tx_data_cnt + 1'b1;
	end
end
//当 dac_valid==1时,接收来自外部的信号(在后面的代码解析后发送给9361)
always @(posedge data_clk) begin
	if(dac_valid == 1'b1 ) begin
		tx_data_i1_d <= dac_data_i1; //sin1
		tx_data_q1_d <= dac_data_q1; //cos1
		tx_data_i2_d <= dac_data_i2; //sin2
		tx_data_q2_d <= dac_data_q2; //cos2
	end
end
// 拼接各个信号, 用于确定发送状态
assign tx_data_sel = {dac_mode_r1,tx_data_cnt_flag,tx_data_cnt};
// 对DDS传入的数据进行解析
// 1R1T,  tx_data_p==IM, tx_data_n=QM,tx_data_p==IL,tx_data_n=QL,
// 2R2T,  tx_data_p==I1M,tx_data_n=Q1M,tx_data_p==I1L,tx_data_n=Q1L,
//        tx_data_p==I2M,tx_data_n=Q2M,tx_data_p==I2L,tx_data_n=Q2L,
always @(posedge data_clk) begin
	case(tx_data_sel) 
	4'b1100: begin //1r1t IM,QM
		tx_frame  <= 1'b1;
		tx_data_p <= tx_data_i1_d[11:6];
		tx_data_n <= tx_data_q1_d[11:6];
	end
	4'b1101: begin//1r1t IL,QL 
		tx_frame  <= 1'b0;
		tx_data_p <= tx_data_i1_d[5:0];
		tx_data_n <= tx_data_q1_d[5:0];
	end
	4'b0100:begin//2r2t  I1M,Q1M
		tx_frame  <= 1'b1;
		tx_data_p <= tx_data_i1_d[11:6];
		tx_data_n <= tx_data_q1_d[11:6];
	end
	4'b0101:begin//2r2t I1L,Q1L
		tx_frame  <= 1'b1;
		tx_data_p <= tx_data_i1_d[5:0];
		tx_data_n <= tx_data_q1_d[5:0];
	end
	4'b0110:begin//2r2t I2M,Q2M
		tx_frame  <= 1'b0;
		tx_data_p <= tx_data_i2_d[11:6];
		tx_data_n <= tx_data_q2_d[11:6];
	end
	4'b0111:begin//2r2t I2L,Q2L
		tx_frame  <= 1'b0;
		tx_data_p <= tx_data_i2_d[5:0];
		tx_data_n <= tx_data_q2_d[5:0];
	end
	default : begin
		tx_frame  <= 1'b0;
		tx_data_p <= 0;
		tx_data_n <= 0;		
	end
	endcase
end

//tx frame signal --- begin
	//1.对输入的 tx_frame 信号转换为 tx_frame_out 与tx_frame反向
   ODDR #(
      .DDR_CLK_EDGE("SAME_EDGE"), // "OPPOSITE_EDGE" or "SAME_EDGE" 
      .INIT(1'b0),                // Initial value of Q: 1'b0 or 1'b1
      .SRTYPE("SYNC")             // Set/Reset type: "SYNC" or "ASYNC" 
   ) ODDR_frame_inst (
      .Q(tx_frame_out), // 1-bit DDR output
      .C(data_clk),     // 1-bit clock input
      .CE(1'b1),        // 1-bit clock enable input
      .D1(tx_frame),    // 1-bit data input (positive edge)
      .D2(tx_frame),    // 1-bit data input (negative edge)
      .R(1'b0),         // 1-bit reset
      .S(1'b0)          // 1-bit set
   );
   //2. 对输入的单端 tx_frame_out 转换为差分信号:tx_frame_out_p/n
   OBUFDS #(
      .IOSTANDARD("DEFAULT"), // Specify the output I/O standard
      .SLEW("SLOW")           // Specify the output slew rate
   ) OBUFDS_frame_inst (
      .O(tx_frame_out_p),     // Diff_p output (connect directly to top-level port)
      .OB(tx_frame_out_n),    // Diff_n output (connect directly to top-level port)
      .I(tx_frame_out)        // Buffer input
   );
//tx frame signal --- end

//tx data signal --- begin
generate
    genvar tx_lane;
    for (tx_lane = 0; tx_lane < 6; tx_lane = tx_lane + 1) begin : gen_tx_lane
		//1.把输入的并行数据 tx_data_p/n 转换为串行输出: tx_data_out
	   ODDR #(
	      .DDR_CLK_EDGE("SAME_EDGE"), // "OPPOSITE_EDGE" or "SAME_EDGE" 
	      .INIT(1'b0),    			  // Initial value of Q: 1'b0 or 1'b1
	      .SRTYPE("SYNC")             // Set/Reset type: "SYNC" or "ASYNC" 
	   ) ODDR_tx_data_inst (
	      .Q(tx_data_out[tx_lane]),  // 1-bit DDR output
	      .C(data_clk),        // 1-bit clock input
	      .CE(1'b1),           // 1-bit clock enable input
	      .D1(tx_data_p[tx_lane]),   // 1-bit data input (positive edge)
	      .D2(tx_data_n[tx_lane]),   // 1-bit data input (negative edge)
	      .R(1'b0),            // 1-bit reset
	      .S(1'b0)             // 1-bit set
	   );
		//2.对输入的单端 tx_data_out 转换为差分信号:tx_data_out_p/n
	   OBUFDS #(
	      .IOSTANDARD("DEFAULT"), // Specify the output I/O standard
	      .SLEW("SLOW")           // Specify the output slew rate
	   ) OBUFDS_tx_data_inst (
	      .O(tx_data_out_p[tx_lane]),   // Diff_p output (connect directly to top-level port)
	      .OB(tx_data_out_n[tx_lane]),  // Diff_n output (connect directly to top-level port)
	      .I(tx_data_out[tx_lane])      // Buffer input
	   );		
	end
endgenerate
//tx data signal --- end

//tx_clk signal --- begin
	//1.输出的时钟:tx_clk 与 data_clk 反相(取决于D1,D2) D1:0  D2:1
   ODDR #(
      .DDR_CLK_EDGE("SAME_EDGE"), // "OPPOSITE_EDGE" or "SAME_EDGE" 
      .INIT(1'b0),                // Initial value of Q: 1'b0 or 1'b1
      .SRTYPE("SYNC")             // Set/Reset type: "SYNC" or "ASYNC" 
   ) ODDR_data_clk_inst (
      .Q(tx_clk),       // 1-bit DDR output
      .C(data_clk),     // 1-bit clock input
      .CE(1'b1),        // 1-bit clock enable input
      .D1(1'b0),        // 1-bit data input (positive edge)
      .D2(1'b1),        // 1-bit data input (negative edge)
      .R(1'b0),         // 1-bit reset
      .S(1'b0)          // 1-bit set
   );
    //2.将时钟 tx_clk 转化为差分时钟输出:tx_clk_out_p/n
   OBUFDS #(
      .IOSTANDARD("DEFAULT"), // Specify the output I/O standard
      .SLEW("SLOW")           // Specify the output slew rate
   ) OBUFDS_data_clk_inst (
      .O(tx_clk_out_p),    // Diff_p output (connect directly to top-level port)
      .OB(tx_clk_out_n),   // Diff_n output (connect directly to top-level port)
      .I(tx_clk)          // Buffer input
   );
//tx_clk signal --- end
endmodule
