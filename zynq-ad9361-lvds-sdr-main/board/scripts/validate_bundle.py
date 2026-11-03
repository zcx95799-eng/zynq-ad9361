"""Offline consistency checks; no hardware or network connections."""
from datetime import datetime
import hashlib
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET
import zipfile
from board_io import MonitorParser, HEADER

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1024*1024), b''):
            h.update(data)
    return h.hexdigest()


def main():
    checks = []
    def check(name, condition, detail=None):
        checks.append(dict(name=name, status='PASS' if condition else 'FAIL', detail=detail))
    project = ROOT / 'project/vivado/AD9361_LVDS_PRJ'
    paths = []
    for f in ET.parse(project / 'AD9361_LVDS_PRJ.xpr').findall('.//File'):
        name = f.attrib['Path']
        for variable, value in {'$PSRCDIR':project / 'AD9361_LVDS_PRJ.srcs',
                                '$PIPUSERFILESDIR':project / 'AD9361_LVDS_PRJ.ip_user_files',
                                '$PPRDIR':project}.items():
            name = name.replace(variable, str(value))
        paths.append(Path(name))
    missing = [str(p) for p in paths if not p.is_file()]
    check('XPR file references', not missing, dict(count=len(paths), missing=missing))
    with zipfile.ZipFile(ROOT / 'artifacts/fpga/system.hdf') as hdf:
        check('HDF embedded bit equals delivered bit',
              hashlib.sha256(hdf.read('system_top.bit')).hexdigest() == digest(ROOT / 'artifacts/fpga/system_top.bit'))
        check('PS init extracted from current HDF',
              hdf.read('ps7_init.tcl') == (ROOT / 'artifacts/fpga/ps7_init.tcl').read_bytes())
    for source, kind in [('project/sdk/AD9361/src', '*.c'), ('host/qt', '*.cpp')]:
        check(source + ' exists', bool(list((ROOT / source).glob(kind))))
    # Fragmented TCP stream plus back-to-back frames, metadata types and invalid flags.
    payload = struct.pack('<2048h', *([1, -1]*1024)) + bytes(1024)
    frames = [HEADER.pack(b'MONI', 1, 5, flag, seq, 5120) + payload
              for seq, flag in enumerate([2, 3, 0x102, 0x202])]
    parser = MonitorParser(); got = []
    stream = b'noise' + b''.join(frames)
    for offset in range(0, len(stream), 37):
        got.extend(parser.feed(stream[offset:offset+37]))
    check('MONI fragmentation / concatenation / resync',
          [f[0] for f in got] == [2, 3, 0x102, 0x202] and parser.discarded == 5 and not parser.buffer)
    parser = MonitorParser()
    invalid = HEADER.pack(b'MONI', 1, 5, 0x103, 99, 5120) + payload
    got = parser.feed(invalid + frames[1])
    check('Reject reference fragment with demod-valid flag', len(got) == 1 and got[0][0] == 3)
    # Verify original E-drive files if still accessible; these checks are optional after relocation.
    snapshot = json.loads((ROOT / 'validation/source_snapshot.json').read_text(encoding='utf-8'))
    changes, compared, unavailable = [], 0, 0
    for name, expected in snapshot['unchanged_source_sha256'].items():
        path = Path(name)
        if not path.exists():
            unavailable += 1; continue
        compared += 1
        if digest(path) != expected:
            changes.append(name)
    check('Original source files unchanged', not changes, dict(compared=compared, unavailable=unavailable, changed=changes))
    for n in (1, 2):
        capture = ROOT / f'measured/raw/hw_ila_data_{n}.ila'
        with zipfile.ZipFile(capture) as archive:
            check(capture.name + ' archive CRC', archive.testzip() is None)
        expected = next(x['sha256'] for x in snapshot['captures'] if x['packaged'].endswith(capture.name))
        check(capture.name + ' original hash', digest(capture) == expected)
    manifest = ROOT / 'manifest.sha256'
    if manifest.exists():
        bad = []
        for line in manifest.read_text(encoding='utf-8').splitlines():
            expected, name = line.split('  ', 1)
            path = ROOT / name
            if not path.is_file() or digest(path) != expected:
                bad.append(name)
        check('Delivered file manifest', not bad, bad)
    report = dict(kind='offline_validation', generated=datetime.now().astimezone().isoformat(),
                  status='PASS' if all(c['status'] == 'PASS' for c in checks) else 'FAIL', checks=checks,
                  hardware_download_executed=False, new_board_measurement=False)
    (ROOT / 'validation/offline_validation.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    for c in checks:
        print(c['status'], c['name'])
    raise SystemExit(0 if report['status'] == 'PASS' else 1)


if __name__ == '__main__':
    main()
