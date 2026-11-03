"""Compare a saved TX payload with one valid MONI demod appendix (1024 bytes)."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import struct


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('tx', type=Path, help='Saved payload.bin from the corresponding send run')
    p.add_argument('rx', type=Path, help='Received *_demod.bin, with demod-valid flag set')
    p.add_argument('--output', type=Path, required=True, help='New JSON report path')
    a = p.parse_args()
    tx, rx = a.tx.read_bytes(), a.rx.read_bytes()
    if len(tx) != 1024 or len(rx) != 1024:
        p.error('Both inputs must be exactly 1024 bytes')
    if not a.rx.name.endswith('_demod.bin'):
        p.error('Use an actual received *_demod.bin, not reference/FFT/idle data')
    tw, rw = struct.unpack('<256I', tx), struct.unpack('<256I', rx)
    mismatches = [dict(index=i, tx=f'0x{x:08X}', rx=f'0x{y:08X}')
                  for i, (x, y) in enumerate(zip(tw, rw)) if x != y]
    report = dict(generated=datetime.now().astimezone().isoformat(), kind='saved_payload_comparison',
                  tx=str(a.tx), rx=str(a.rx), tx_sha256=hashlib.sha256(tx).hexdigest(),
                  rx_sha256=hashlib.sha256(rx).hexdigest(), words=256,
                  mismatched_words=len(mismatches), different_bits=sum(bin(x ^ y).count('1') for x, y in zip(tx, rx)),
                  status='IDENTICAL' if tx == rx else 'DIFFERENT', mismatches=mismatches,
                  note='User must associate this RX frame with the corresponding TX experiment. This single comparison is not a channel BER measurement.')
    a.output.parent.mkdir(parents=True, exist_ok=True)
    with a.output.open('x', encoding='utf-8') as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print(report['status'], 'different words:', len(mismatches), 'different bits:', report['different_bits'])
    raise SystemExit(0 if tx == rx else 1)


if __name__ == '__main__':
    main()
