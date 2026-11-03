"""Generate a pure sine in the Qt monitor's default signed I16/Q16 format."""
import math
import struct
from pathlib import Path


def main():
    output = Path(__file__).resolve().parents[1] / "test_data" / "sine"
    output.mkdir(parents=True, exist_ok=True)
    samples = 1024
    amplitude = 12000
    period = 64
    values = [round(amplitude * math.sin(2 * math.pi * n / period)) for n in range(samples)]
    iq = b"".join(struct.pack("<hh", value, 0) for value in values)
    payload = iq + bytes(1024)  # No demodulation result; reserve the default section.
    packet = struct.pack(">4sBBHII", b"MONI", 1, 5, 2, 0, len(payload)) + payload

    # Validate boundaries and byte order independently of the generated labels.
    assert len(iq) == 4096 and len(packet) == 5136
    decoded = struct.unpack("<2048h", iq)
    assert decoded[::2] == tuple(values) and not any(decoded[1::2])
    assert values[0] == 0 and values[16] == 12000 and values[48] == -12000
    assert values[:64] == values[64:128] and sum(values) == 0
    assert struct.unpack(">4sBBHII", packet[:16]) == (b"MONI", 1, 5, 2, 0, 5120)

    (output / "sine_i16q16.bin").write_bytes(iq)
    (output / "sine_moni.bin").write_bytes(packet)
    (output / "sine_i16q16.hex").write_text(
        "\n".join(iq[n:n + 32].hex(" ").upper() for n in range(0, len(iq), 32)) + "\n",
        encoding="utf-8",
    )
    rows = ["index\tI_int16\tQ_int16\tI_LE_Q_LE"]
    rows += [f"{n}\t{value}\t0\t{iq[n * 4:n * 4 + 4].hex(' ').upper()}" for n, value in enumerate(values)]
    (output / "sine_values.txt").write_text("\n".join(rows) + "\n", encoding="utf-8")
    tx_payload = iq[:1024]  # 256 LE32 words; I in low half, Q in high half.
    tx_frame = struct.pack(">4sII", b"TXD1", 1, 1024) + tx_payload
    assert len(tx_frame) == 1036 and struct.unpack(">4sII", tx_frame[:12]) == (b"TXD1", 1, 1024)
    (output / "sine_tx_payload.bin").write_bytes(tx_payload)
    (output / "sine_tx_payload.hex").write_text(
        "\n".join(tx_payload[n:n + 32].hex(" ").upper() for n in range(0, 1024, 32)) + "\n", encoding="utf-8")
    (output / "sine_tx_values.txt").write_text("\n".join(rows[:257]) + "\n", encoding="utf-8")
    (output / "sine_txd1.bin").write_bytes(tx_frame)
    (output / "README.txt").write_text(
        "正弦测试数据（合成）\n"
        "板卡 5002 请使用新版 tcp_duplex_txd1.exe，默认发送协议 TXD1/TXA1。\n"
        "推荐点击“生成正弦测试”，或载入 sine_tx_payload.bin / .hex / sine_tx_values.txt。\n"
        "这三个文件转换后均为 1024 B，256 组 I16/Q16，4 周期。Qt 自动添加 TXD1 帧头，\n"
        "完整请求 1036 B，序号自动递增；收到 TXA1 result=0 才确认 PL 提交成功。\n"
        "sine_txd1.bin：包含 TXD1 帧头，序号=1，共 1036 B。新版载入时取出载荷并自动重新编号。\n"
        "测试的每个 LE32 字低16位为 I、高16位为 Q；PL 对这些字的实际解释需依据硬件逻辑。\n"
        "循环发送等待上一帧回执；板端错误停止循环，超时 6 秒断开发送连接，接收继续。\n\n"
        "下面的旧版 4096 B IQ 文件用于 Qt 接收测试，不适合板卡 5002 的固定 1024 B 载荷：\n"
        "I[n] = round(12000 * sin(2*pi*n/64)), Q[n] = 0\n"
        "1024 组采样，16 个周期，幅度 +/-12000，16 位有符号整数。\n"
        "按 20 MS/s 解释时，信号频率为 312.5 kHz，实数正弦频谱在 +/-312.5 kHz。\n"
        "每组 4 B：I 的低字节、高字节，Q 的低字节、高字节（小端）。\n\n"
        "sine_i16q16.bin：4096 B 纯 I/Q 数据。向 Qt 接收端发送时选择原始字节协议。\n"
        "sine_i16q16.hex：对应 HEX 文本，新版载入文件会解码文本为实际字节。\n"
        "sine_values.txt：全部采样的十进制数值及小端字节。\n\n"
        "sine_moni.bin：5136 B，含 16 B MONI 帧头 + 4096 B I/Q + 1024 B 零解调区。\n"
        "用于发送到 Qt 接收端：新顶层布局、MONI 帧头、I16/Q16、1024 个 IQ 样本、\n"
        "1024 B 解调、无 FFT；实时报波形应显示 I 正弦、Q 为零。\n"
        "type=5，flags=0x0002（IQ 完整、无完整解调帧），序号=0。\n"
        "同一文件重复发送不会递增序号，接收计数中的序号跳变会增加。\n"
        "发送页的字节曲线显示 0-255 原始字节值；要看正弦采样曲线，请用接收端实时可视化页。\n",
        encoding="utf-8",
    )
    print(f"Generated and verified sine data: {output}")
    print("I peak: +12000/-12000; 1024 samples; I/Q raw: 4096 B; MONI: 5136 B")
    print("Board 5002: 256 samples; TX payload: 1024 B; TXD1 frame: 1036 B")


if __name__ == "__main__":
    main()
