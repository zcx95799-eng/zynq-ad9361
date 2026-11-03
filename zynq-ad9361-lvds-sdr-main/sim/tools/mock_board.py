"""Synthetic MONI board for local TCP integration (Python standard library only)."""
import argparse
import math
import select
import socket
import struct
import time


def frame(sequence, legacy=False):
    iq = bytearray()
    for index in range(1024):
        angle = 2 * math.pi * index / 32 + sequence * 0.12
        iq.extend(struct.pack("<hh", int(1400 * math.cos(angle)), int(1400 * math.sin(angle))))
    demod = struct.pack("<256I", *(sum(((index + pair + sequence) % 4) << (pair * 2)
                                     for pair in range(16))
                                 if legacy or sequence % 10 == 0 else 0
                                 for index in range(256)))
    fft = bytearray()
    for index in range(128 if legacy else 0):
        amplitude = int(800000 * math.exp(-((index - 32) / 2.5) ** 2 / 2) + 1000)
        fft.extend(struct.pack("<ii", amplitude, -amplitude // 3))
    payload = bytes(iq) + demod + bytes(fft)
    flags = 0 if legacy else (3 if sequence % 10 == 0 else 2)
    return struct.pack(">4sBBHII", b"MONI", 1, 4 if legacy else 5, flags, sequence & 0xFFFFFFFF, len(payload)) + payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--listen", action="store_true", help="Act as server to test the Qt TCP Client")
    parser.add_argument("--frames", type=int, default=0, help="0 sends until Ctrl+C")
    parser.add_argument("--rate", type=float, default=10)
    parser.add_argument("--fragment", action="store_true", help="Split writes inside header and payload")
    parser.add_argument("--legacy", action="store_true", help="Old IQ12 + demod + FFT layout")
    parser.add_argument("--raw", action="store_true", help="Send fixed payload only, without MONI header")
    args = parser.parse_args()
    if args.rate <= 0 or args.frames < 0:
        parser.error("rate must be positive and frames must be nonnegative")
    if args.listen:
        with socket.socket() as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((args.host, args.port))
            listener.listen(1)
            print(f"Listening on {args.host}:{args.port}", flush=True)
            connection, address = listener.accept()
            print(f"Accepted {address}", flush=True)
    else:
        connection = socket.create_connection((args.host, args.port), timeout=5)
    with connection:
        connection.settimeout(5)
        sequence = 0
        while not args.frames or sequence < args.frames:
            data = frame(sequence, args.legacy)
            if args.raw:
                data = data[16:]
            if args.fragment:
                connection.sendall(data[:7])
                time.sleep(0.002)
                connection.sendall(data[7:1031])
                connection.sendall(data[1031:])
            else:
                connection.sendall(data)
            if select.select([connection], [], [], 0)[0]:
                received = connection.recv(4096)
                if not received:
                    break
                print(f"PC command: {received.hex(' ')}", flush=True)
            sequence += 1
            time.sleep(1 / args.rate)
        print(f"Sent {sequence} synthetic frames", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except OSError as error:
        raise SystemExit(f"TCP error: {error}")
