`timescale 1ns / 1ps
`define SIM_USE3 //定义SIM_USE,如果取消定义,注释此句即可

module Acquisition_Peak_Detection2
#
(
    parameter   DOWN_SAMPLE_OUTPUT_RATE_KHZ     = 32'd12000,        // 降采样之后数据输出速率,单位KHz
    parameter   COHERENT_INTEGRATION_TIME_US    = 250,
    parameter   NUM_NO_CORRELATOR_ACQ           = 1,                // 进行非相干累加次数,NUM_NO_CORRELATOR_ACQ 的取值必须使 FFT_CNT_3 是整数
    parameter   PMF_FFT_N                       = 128,              // FFT点数
    parameter   FFT_OUTPUT_DATA_WIDTH           = 32,               // FFT输出位宽
    parameter   FFT_CNT_3                       = (((DOWN_SAMPLE_OUTPUT_RATE_KHZ*COHERENT_INTEGRATION_TIME_US)/1000))/NUM_NO_CORRELATOR_ACQ/20
)
(
    input                                       iw_Clk_p_g,
    input                                       iw_Rst_n_g,
    input                                       iw_Reset_h,

    input                                       iw_edone,
    input                                       iw_done,
    // Passive telemetry taps; the acquisition arithmetic and decisions are unchanged.
    input                                       iw_fft_last,
    output wire [FFT_OUTPUT_DATA_WIDTH*2-1:0]    ow_monitor_power,
    output wire                                 ow_monitor_valid,
    output wire [log2(PMF_FFT_N)-2:0]            ow_monitor_index,
    output wire                                 ow_monitor_last,
    output wire                                 ow_monitor_new_peak,
    output wire                                 ow_monitor_reset,
    output wire                                 ow_monitor_success,
    input                                       iw_dv,
    input [log2(PMF_FFT_N)-2:0]                 iw_xk_index,
    input [FFT_OUTPUT_DATA_WIDTH-1:0]           iw_xk_re,
    input [FFT_OUTPUT_DATA_WIDTH-1:0]           iw_xk_im,

    input [31:0]                                iw32_Threshold_Factor,

    output                                      ow_Peak_Detection_End_h,    // 峰值搜索完毕,高有效
    output                                      ow_ACQ_Success_h,           // 捕获成功,高有效

    output [log2(PMF_FFT_N)-2:0]                ow_xk_index_Peak,           // 峰值XK,指示多普勒频移,计数范围0~PMF_FFT_N-1
    output [log2(20)-1:0]                       ow_FFT_Cnt_1_Peak,          // 峰值FFT_Cnt_1,,指示码相位,计数范围1~20
    output [log2(NUM_NO_CORRELATOR_ACQ):0]      ow_FFT_Cnt_2_Peak,          // 峰值FFT_Cnt_2,,指示码相位,计数范围1~NUM_NO_CORRELATOR_ACQ
    output [log2(FFT_CNT_3)-1:0]                ow_FFT_Cnt_3_Peak           // 峰值FFT_Cnt_3,,指示码相位,计数范围1~FFT_CNT_3
                                                                            // 注意:这种方法记录下的 r_FFT_Cnt_1_Peak 和真实的码相位位置有误差
                                                                            //      误差不超过 NUM_NO_CORRELATOR_ACQ×(PN_CODE_RATE_HZ/DOWN_SAMPLE_OUTPUT_RATE_HZ)码片
                                                                            //      为了使最终的跟踪环路能稳定跟踪,不要使 NUM_NO_CORRELATOR_ACQ×(PN_CODE_RATE_HZ/DOWN_SAMPLE_OUTPUT_RATE_HZ)的值超过1个码片
                                                                            //      比如:PN_CODE_RATE_HZ=12000000,DOWN_SAMPLE_OUTPUT_RATE_HZ=2046000
                                                                            //      (PN_CODE_RATE_HZ/DOWN_SAMPLE_OUTPUT_RATE_HZ)=0.1705?码片,NUM_NO_CORRELATOR_ACQ取值＜6
);

//*************************************************************************************************************
    function integer log2(input integer n);
        integer i;
        for( i=0; 2**i<=n; i=i+1)
            log2 = i + 1;
    endfunction
//*************************************************************************************************************
//复位逻辑

    reg  r_Reset_h_Sync1;
    reg  r_Reset_h_Sync2;
    wire w_Reset_h;

    assign w_Reset_h = r_Reset_h_Sync1&(~r_Reset_h_Sync2); // 取出 iw_Reset_h 上升沿

    always @(posedge iw_Clk_p_g)
    begin
        r_Reset_h_Sync1 <= iw_Reset_h;
        r_Reset_h_Sync2 <= r_Reset_h_Sync1;
    end

//*************************************************************************************************************

    // 相关峰所在码相位由三个计数段组成,分别是 r_FFT_Cnt_1,r_FFT_Cnt_2,r_FFT_Cnt_3
    // 一共有 DOWN_SAMPLE_OUTPUT_RATE_KHZ×COHERENT_INTEGRATION_TIME_MS=12000个相位检测点,每个FFT结果对应一个检测点
    // r_FFT_Cnt_1 计数范围是1~20
    // r_FFT_Cnt_2 计数范围是1~NUM_NO_CORRELATOR_ACQ
    // r_FFT_Cnt_3 计数范围是1~FFT_CNT_3
    // 当前峰值所在相位点构成公式:((r_FFT_Cnt_3-1)×NUM_NO_CORRELATOR_ACQ+(r_FFT_Cnt_2-1))×20+r_FFT_Cnt_1
    // 比如:FFT_CNT_3=50,   NUM_NO_CORRELATOR_ACQ=4
    //      r_FFT_Cnt_3=2,  r_FFT_Cnt_2=1,              r_FFT_Cnt_1=60
    // 表示在第((2-1)×4+(1-1))×60+60=360个FFT计算结果(相位点)得到相关峰
    reg [log2(20)-1:0]      r_FFT_Cnt_1;    // 计数范围1~20
    reg [log2(NUM_NO_CORRELATOR_ACQ):0] r_FFT_Cnt_2;    // 计数范围1~NUM_NO_CORRELATOR_ACQ
    reg [log2(FFT_CNT_3)-1:0]           r_FFT_Cnt_3;    // 计数范围1~FFT_CNT_3

    always@( posedge iw_Clk_p_g)
    begin
        if(!iw_Rst_n_g)
        begin
            r_FFT_Cnt_1 <= 20;
            r_FFT_Cnt_3 <= FFT_CNT_3;
        end
        else if(w_Reset_h)
        begin
            r_FFT_Cnt_1 <= 20;
            r_FFT_Cnt_2 <= NUM_NO_CORRELATOR_ACQ;
            r_FFT_Cnt_3 <= FFT_CNT_3;
        end
        else if(iw_edone)
        begin
            r_FFT_Cnt_1 <= (r_FFT_Cnt_1==20) ?                                                                                                      1'b1 : (r_FFT_Cnt_1+1'b1);
            r_FFT_Cnt_2 <= (r_FFT_Cnt_1==20) ? ( r_FFT_Cnt_2==NUM_NO_CORRELATOR_ACQ ?                                           1'b1:(r_FFT_Cnt_2+1'b1)) : r_FFT_Cnt_2;
            r_FFT_Cnt_3 <= (r_FFT_Cnt_1==20) ? ( r_FFT_Cnt_2==NUM_NO_CORRELATOR_ACQ ?  (r_FFT_Cnt_3==FFT_CNT_3 ? 1'b1:(r_FFT_Cnt_3+1'b1)) : r_FFT_Cnt_3) : r_FFT_Cnt_3;
        end
        else
        begin
            r_FFT_Cnt_1 <= r_FFT_Cnt_1;
            r_FFT_Cnt_2 <= r_FFT_Cnt_2;
            r_FFT_Cnt_3 <= r_FFT_Cnt_3;
        end
    end

//*************************************************************************************************************
//延迟

    reg                         r_edone;
    reg                         r_done;
    reg [5:0]                   r_dv_Sync;

    reg [log2(PMF_FFT_N)-2:0]   r_xk_index_Sync1;
    reg [log2(PMF_FFT_N)-2:0]   r_xk_index_Sync2;
    reg [log2(PMF_FFT_N)-2:0]   r_xk_index_Sync3;

    always@( posedge iw_Clk_p_g)
    begin
        r_dv_Sync <= {r_dv_Sync[4:0],iw_dv};
    end

    always@( posedge iw_Clk_p_g)
    begin
        r_xk_index_Sync1 <= iw_xk_index;
        r_xk_index_Sync2 <= r_xk_index_Sync1;
        r_xk_index_Sync3 <= r_xk_index_Sync2;
    end

//*************************************************************************************************************
//缓存

    localparam MEM_DATA_WIDTH = log2(NUM_NO_CORRELATOR_ACQ)+FFT_OUTPUT_DATA_WIDTH*2+1;  // 缓存位宽
    localparam MEM_DEEPTH     = PMF_FFT_N;                                              // 缓存深度,应该不需要乘 20    

    (*ram_style="block"*)reg [MEM_DATA_WIDTH-1:0] mem[MEM_DEEPTH-1:0];

    wire                        mem_wen;    // 1=写操作有效: 0=写操作无效
    reg [log2(MEM_DEEPTH)-1:0]  mem_waddr;  // 写操作地址
    wire [MEM_DATA_WIDTH-1:0]   mem_wdata;  // 写入的数据

    reg [log2(MEM_DEEPTH)-1:0]  mem_raddr;  // 读操作地址
    reg [MEM_DATA_WIDTH-1:0]    mem_rdata;  // 读出的数据
    reg [MEM_DATA_WIDTH-1:0]    mem_rdata_sync1;

    assign mem_wen = r_dv_Sync[2];  // 产生 mem_wen 信号

    // 写入
    always @(posedge iw_Clk_p_g)
    begin
        if(mem_wen)
        begin
            mem[mem_waddr] <= mem_wdata;
        end
    end

    // 读出,一个潜伏期
    always @(posedge iw_Clk_p_g)
    begin
        mem_rdata <= mem[mem_raddr];
    end

    always @(posedge iw_Clk_p_g)
    begin
        if(r_FFT_Cnt_2==1'b1)
            mem_rdata_sync1 <= {(MEM_DATA_WIDTH){1'b0}};    // 非相干积分,第1次,加0
        else
            mem_rdata_sync1 <= mem_rdata;                   // 之后,加上次存储的数据
    end
//*************************************************************************************************************
//计算缓存的读/写地址

    // 计算写地址
    always @(posedge iw_Clk_p_g)
    begin
        if(!iw_Rst_n_g)
        begin
            mem_waddr <= {(log2(MEM_DEEPTH)){1'b0}};
        end
        else if(w_Reset_h)
        begin
            mem_waddr <= {(log2(MEM_DEEPTH)){1'b0}};
        end
        else if(r_dv_Sync[2])
        begin
            mem_waddr <= mem_waddr==(MEM_DEEPTH-1) ? {(log2(MEM_DEEPTH)){1'b0}} : mem_waddr+1'b1;
        end
        else
        begin
            mem_waddr <= mem_waddr;
        end
    end

    // 计算读地址
    always @(posedge iw_Clk_p_g)
    begin
        if(!iw_Rst_n_g)
        begin
            mem_raddr <= {(log2(MEM_DEEPTH)){1'b0}};
        end
        else if(w_Reset_h)
        begin
            mem_raddr <= {(log2(MEM_DEEPTH)){1'b0}};
        end
        else if(iw_dv)
        begin
            mem_raddr <= mem_raddr==(MEM_DEEPTH-1) ? {(log2(MEM_DEEPTH)){1'b0}} : mem_raddr+1'b1;
        end
        else
        begin
            mem_raddr <= mem_raddr;
        end
    end

//*************************************************************************************************************

    reg [FFT_OUTPUT_DATA_WIDTH*2-1:0]   r_fft_result_real_sqr;
    reg [FFT_OUTPUT_DATA_WIDTH*2-1:0]   r_fft_result_imag_sqr;
    (*mark_debug = "true"*) reg [FFT_OUTPUT_DATA_WIDTH*2-1:0]   r_fft_result_sqr; // debug模拟显示仅支持位宽不超过64Bits的总线,65bits不支持模拟显示

    // r_fft_result_real_sqr 与 r_dv_Sync[0]对齐
    // r_fft_result_imag_sqr 与 r_dv_Sync[0]对齐
    // r_fft_result_sqr      与 r_dv_Sync[1]对齐
    always@( posedge iw_Clk_p_g)
    begin
        r_fft_result_real_sqr <= {{(FFT_OUTPUT_DATA_WIDTH){iw_xk_re[FFT_OUTPUT_DATA_WIDTH-1]}},iw_xk_re}*{{(FFT_OUTPUT_DATA_WIDTH){iw_xk_re[FFT_OUTPUT_DATA_WIDTH-1]}},iw_xk_re};
        r_fft_result_imag_sqr <= {{(FFT_OUTPUT_DATA_WIDTH){iw_xk_im[FFT_OUTPUT_DATA_WIDTH-1]}},iw_xk_im}*{{(FFT_OUTPUT_DATA_WIDTH){iw_xk_im[FFT_OUTPUT_DATA_WIDTH-1]}},iw_xk_im};
        r_fft_result_sqr      <= ({r_fft_result_real_sqr[FFT_OUTPUT_DATA_WIDTH*2-1],r_fft_result_real_sqr}+{r_fft_result_imag_sqr[FFT_OUTPUT_DATA_WIDTH*2-1],r_fft_result_imag_sqr});
    end

    reg [MEM_DATA_WIDTH-1:0] r_Sum;

    assign mem_wdata = r_Sum;   // 加和写入缓存

    always@( posedge iw_Clk_p_g)
    begin
        r_Sum <= mem_rdata_sync1+r_fft_result_sqr;  // r_Sum 与 r_dv_Sync[2]对齐
    end

    reg                                 r_Max_Point_Sum;
    reg         r_Max_Point_Single;
    wire                                w_Max_Point_Single = r_Max_Point_Single;
    reg [MEM_DATA_WIDTH-1:0]            r_Peak_Sum;
    reg [MEM_DATA_WIDTH-1:0]            r_Peak_Single;

    reg [log2(PMF_FFT_N)-2:0]           r_xk_index_Peak;    // 峰值XK,指示多普勒频移,计数范围0~PMF_FFT_N-1
    reg [log2(20)-1:0]                  r_FFT_Cnt_1_Peak;   // 峰值FFT_Cnt_1,,指示码相位,计数范围1~20
    reg [log2(NUM_NO_CORRELATOR_ACQ):0] r_FFT_Cnt_2_Peak;   // 峰值FFT_Cnt_2,,指示码相位,计数范围1~NUM_NO_CORRELATOR_ACQ
    reg [log2(FFT_CNT_3)-1:0]           r_FFT_Cnt_3_Peak;   // 峰值FFT_Cnt_3,,指示码相位,计数范围1~FFT_CNT_3

    wire [log2(PMF_FFT_N)-2:0]           w_xk_index_Peak  = r_xk_index_Peak;    // 峰值XK,指示多普勒频移,计数范围0~PMF_FFT_N-1
    wire [log2(20)-1:0]                  w_FFT_Cnt_1_Peak = r_FFT_Cnt_1_Peak;   // 峰值FFT_Cnt_1,,指示码相位,计数范围1~20
    wire [log2(NUM_NO_CORRELATOR_ACQ):0] w_FFT_Cnt_2_Peak = r_FFT_Cnt_2_Peak;   // 峰值FFT_Cnt_2,,指示码相位,计数范围1~NUM_NO_CORRELATOR_ACQ
    wire [log2(FFT_CNT_3)-1:0]           w_FFT_Cnt_3_Peak = r_FFT_Cnt_3_Peak;   // 峰值FFT_Cnt_3,,指示码相位,计数范围1~FFT_CNT_3

    wire w_Max_Point_h = r_Max_Point_Sum;

    //更新:非相干积分峰值的出现具有滞后性,直接用实时峰值判断
    always@( posedge iw_Clk_p_g)
    begin
        if(!iw_Rst_n_g)
        begin
            r_Max_Point_Sum <= 1'b0;
            r_Peak_Sum <= {(MEM_DATA_WIDTH){1'b0}};
        end
        else if(w_Reset_h)
        begin
            r_Max_Point_Sum <= 1'b0;
            r_Peak_Sum <= {(MEM_DATA_WIDTH){1'b0}};
        end
        else if(r_dv_Sync[2])
        begin
            if(r_Sum>=r_Peak_Sum)
            begin
                r_Max_Point_Sum <= 1'b1;
                r_Peak_Sum <= r_Sum;
            end
            else
            begin
                r_Max_Point_Sum <= 1'b0;
                r_Peak_Sum <= r_Peak_Sum;
            end
        end
        else
        begin
            r_Max_Point_Sum <= 1'b0;
            r_Peak_Sum <= r_Peak_Sum;
        end
    end

    // 更新:得到原始FFT结果最大值,这才是正确的相位点
    always@( posedge iw_Clk_p_g)
    begin
        if(!iw_Rst_n_g)
        begin
            r_Max_Point_Single <= 1'b0;
            r_Peak_Single <= {(MEM_DATA_WIDTH){1'b0}};

            r_xk_index_Peak  <= {(PMF_FFT_N-1){1'b0}};
            r_FFT_Cnt_1_Peak <= {(20){1'b0}};
            r_FFT_Cnt_2_Peak <= {(NUM_NO_CORRELATOR_ACQ+1){1'b0}};
            r_FFT_Cnt_3_Peak <= {(FFT_CNT_3){1'b0}};
        end
        else if(w_Reset_h)
        begin
            r_Max_Point_Single <= 1'b0;
            r_Peak_Single <= {(MEM_DATA_WIDTH){1'b0}};

            r_xk_index_Peak  <= {(PMF_FFT_N-1){1'b0}};
            r_FFT_Cnt_1_Peak <= {(20){1'b0}};
            r_FFT_Cnt_2_Peak <= {(NUM_NO_CORRELATOR_ACQ+1){1'b0}};
            r_FFT_Cnt_3_Peak <= {(FFT_CNT_3){1'b0}};
        end
        else if(r_dv_Sync[1])
        begin
            if(r_fft_result_sqr>=r_Peak_Single)
            begin
                r_Max_Point_Single <= 1'b1;
                r_Peak_Single <= r_fft_result_sqr;

                r_xk_index_Peak  <= r_xk_index_Sync2;
                r_FFT_Cnt_1_Peak <= r_FFT_Cnt_1;
                r_FFT_Cnt_2_Peak <= r_FFT_Cnt_2;
                r_FFT_Cnt_3_Peak <= r_FFT_Cnt_3;
            end
            else
            begin
                r_Max_Point_Single <= 1'b0;
                r_Peak_Single <= r_Peak_Single;

                r_xk_index_Peak  <= r_xk_index_Peak;
                r_FFT_Cnt_1_Peak <= r_FFT_Cnt_1_Peak;
                r_FFT_Cnt_2_Peak <= r_FFT_Cnt_2_Peak;
                r_FFT_Cnt_3_Peak <= r_FFT_Cnt_3_Peak;
            end
        end
        else
        begin
            r_Max_Point_Single <= 1'b0;
            r_Peak_Single <= r_Peak_Single;

            r_xk_index_Peak  <= r_xk_index_Peak;
            r_FFT_Cnt_1_Peak <= r_FFT_Cnt_1_Peak;
            r_FFT_Cnt_2_Peak <= r_FFT_Cnt_2_Peak;
            r_FFT_Cnt_3_Peak <= r_FFT_Cnt_3_Peak;
        end
    end

//*************************************************************************************************************

    localparam NOISE_CNT = 1024;   // 用于计算噪声均值的统计值数量,该值越小,实时性越好,该值最好是2的幂次,否则噪声均值偏小
    
    reg [FFT_OUTPUT_DATA_WIDTH*2-1:0]   r_Noise_Max;
    reg [FFT_OUTPUT_DATA_WIDTH*2-1:0]   r_fft_result_sqr_acc;    
    reg [15:0]  r16_Cnt;
    reg         r_Noise_valid;
    reg         r_Noise_valid2;
    reg         r_Noise_valid2_sync;

    always@( posedge iw_Clk_p_g)
    begin
        if(!iw_Rst_n_g)
        begin
            r16_Cnt   <= 16'd0;

            r_Noise_Max         <= {(FFT_OUTPUT_DATA_WIDTH*2){1'b0}};
            r_fft_result_sqr_acc<= {(FFT_OUTPUT_DATA_WIDTH*2){1'b0}};

            r_Noise_valid <= 1'b0;
            r_Noise_valid2<= 1'b0;            
        end
        else if(w_Reset_h||r_Max_Point_Single) // 一旦出现最大值就重新统计噪声
        begin
            r16_Cnt   <= 16'd0;

            r_Noise_Max         <= {(FFT_OUTPUT_DATA_WIDTH*2){1'b0}};
            r_fft_result_sqr_acc<= {(FFT_OUTPUT_DATA_WIDTH*2){1'b0}};

            r_Noise_valid2<= 1'b0;

            if(r_Max_Point_Single)
            begin
                r_Noise_valid <= 1'b1;
            end
            else 
            begin
                r_Noise_valid <= 1'b0;
            end
        end
        else if(r16_Cnt==(16'd2048+NOISE_CNT))
        begin
            r16_Cnt   <= 16'd0;

            r_Noise_Max         <= r_fft_result_sqr_acc>>(log2(NOISE_CNT)+1); // 理论应该是 r_fft_result_sqr_acc>>(log2(NOISE_CNT)-1)
                                                                            // 这里多移动几位,是为了更好的压低噪声
            r_fft_result_sqr_acc<= {(FFT_OUTPUT_DATA_WIDTH*2){1'b0}};           

            if(r_Noise_valid)
            begin
                r_Noise_valid <= 1'b0;
                r_Noise_valid2<= 1'b1;
            end
            else 
            begin
                r_Noise_valid <= r_Noise_valid;
                r_Noise_valid2<= 1'b0;
            end
        end       
        else if(r_dv_Sync[1])
        begin
            r16_Cnt <= r16_Cnt+16'd1;

            r_Noise_Max <= r_Noise_Max;
            
            if(r16_Cnt>=16'd2048)
                r_fft_result_sqr_acc <= r_fft_result_sqr_acc+r_fft_result_sqr;
            else 
                r_fft_result_sqr_acc <= r_fft_result_sqr_acc;

            r_Noise_valid <= r_Noise_valid;
            r_Noise_valid2<= 1'b0;
        end
    end

    always@( posedge iw_Clk_p_g)
    begin
        r_Noise_valid2_sync <= r_Noise_valid2;
    end
//*************************************************************************************************************
//输出码相位,XK,噪声估计值,同步信号

    reg [31:0]                  r32_Threshold_Factor;
    reg [MEM_DATA_WIDTH-1:0]    r_P_Threshold_Factor_Noise_Max;

    reg                         r_Peak_Detection_End_h;
    reg                         r_ACQ_Success_h;


    always@( posedge iw_Clk_p_g)
    begin
        r32_Threshold_Factor <= iw32_Threshold_Factor;
    end

    always@( posedge iw_Clk_p_g)
    begin
        r_P_Threshold_Factor_Noise_Max <= r32_Threshold_Factor*r_Noise_Max;
    end

    reg r_selflock;
 
    wire dete_end_flg = (r_FFT_Cnt_1==20)&&(r_FFT_Cnt_2==NUM_NO_CORRELATOR_ACQ)&&(r_FFT_Cnt_3==FFT_CNT_3)&&(iw_edone);

    always@( posedge iw_Clk_p_g)
    begin
        if(!iw_Rst_n_g)
        begin
            r_Peak_Detection_End_h <= 1'b0;
            r_ACQ_Success_h <= 1'b0;

            r_selflock <= 1'b0;
        end
        else if(w_Reset_h)
        begin
            r_Peak_Detection_End_h <= 1'b0;
            r_ACQ_Success_h <= 1'b0;

            r_selflock <= 1'b0;
        end
        else
        begin
            if((r_Noise_valid2_sync)&&(r_Peak_Single>=r_P_Threshold_Factor_Noise_Max)&&(r_P_Threshold_Factor_Noise_Max!={(MEM_DATA_WIDTH+32){1'b0}}))    // 捕获成功
            begin
                r_Peak_Detection_End_h <= 1'b1;             // 只持续一个时钟周期,但会有效多次,只处理一次即可
                r_ACQ_Success_h  <= 1'b1;                   // 只持续一个时钟周期,但会有效多次,只处理一次即可,注意:上电之后的第1次有效是假的

                r_selflock <= r_selflock;
            end
            else if(dete_end_flg&&(~r_selflock))            // 完成一次完整周期的码相位检测,并且没有出现相关峰,跳过初始的无效期,相关峰信号太弱或门限设置过高,捕获不成功,换星
            begin
                r_Peak_Detection_End_h <= 1'b0;    
                r_ACQ_Success_h  <= 1'b0;

                r_selflock <= 1'b1;
            end
            else if(dete_end_flg&&(r_selflock))
            begin
                r_Peak_Detection_End_h <= 1'b1;             // 只持续一个时钟周期
                r_ACQ_Success_h  <= 1'b0;

                r_selflock <= r_selflock;
            end
            else
            begin
                r_Peak_Detection_End_h <= 1'b0;    
                r_ACQ_Success_h  <= 1'b0;

                r_selflock <= r_selflock;
            end
        end
    end

//*************************************************************************************************************
// 输出接口

    assign ow_Peak_Detection_End_h = r_Peak_Detection_End_h;
    assign ow_ACQ_Success_h = r_ACQ_Success_h;

    assign ow_xk_index_Peak  = r_xk_index_Peak;
    assign ow_FFT_Cnt_1_Peak = r_FFT_Cnt_1_Peak;
    assign ow_FFT_Cnt_2_Peak = r_FFT_Cnt_2_Peak;
    assign ow_FFT_Cnt_3_Peak = r_FFT_Cnt_3_Peak;

//*************************************************************************************************************

//ila_2 Acquisition_Peak_ILA (
    //.clk    (iw_Clk_p_g),

   // .probe0 (iw_Rst_n_g),                       // 1
    //.probe1 (w_Reset_h),                        // 1
    //.probe2 (iw_dv),                            // 1
    //.probe3 (iw_edone),                         // 1
    //.probe4 (iw_xk_index),                      // 7

    //.probe5 (r_FFT_Cnt_1),                      // 5
    //.probe6 (r_FFT_Cnt_2),                      // 2
    //.probe7 (r_FFT_Cnt_3),                      // 8

    //.probe8 (mem_wen),                          // 1
    //.probe9 (mem_waddr),                        // 8
    //.probe10(r_dv_Sync),                        // 6

    //.probe11(r_fft_result_sqr),                 // 64
    //.probe12(r_Peak_Single),                    // 66
    //.probe13(r_Noise_Max),                      // 64
    //.probe14(r_P_Threshold_Factor_Noise_Max),   // 66

    //.probe15(r_Max_Point_Single),               // 1
    //.probe16(r_Noise_valid2_sync),              // 1
    //.probe17(dete_end_flg),                     // 1
    //.probe18(ow_Peak_Detection_End_h),          // 1
    //.probe19(ow_ACQ_Success_h)                  // 1
//);




    // The consumer samples these on the SAME edge as the existing peak compare.
    // Success uses the previous peak, exactly as the original decision below.
    reg [1:0] monitor_last_delay;
    always @(posedge iw_Clk_p_g) begin
        if (!iw_Rst_n_g || w_Reset_h) monitor_last_delay <= 0;
        else monitor_last_delay <= {monitor_last_delay[0], iw_fft_last && iw_dv};
    end
    assign ow_monitor_reset = !iw_Rst_n_g || w_Reset_h;
    assign ow_monitor_power = r_fft_result_sqr;
    assign ow_monitor_valid = r_dv_Sync[1] && !ow_monitor_reset;
    assign ow_monitor_index = r_xk_index_Sync2;
    assign ow_monitor_last = monitor_last_delay[1] && ow_monitor_valid;
    assign ow_monitor_new_peak = ow_monitor_valid && (r_fft_result_sqr >= r_Peak_Single);
    assign ow_monitor_success = !ow_monitor_reset && r_Noise_valid2_sync &&
        (r_Peak_Single >= r_P_Threshold_Factor_Noise_Max) &&
        (r_P_Threshold_Factor_Noise_Max != 0);
endmodule
