"""Rebuild the unchanged SDK application against the packaged BSP libraries."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from datetime import datetime

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', type=Path, default=Path(r'F:\vivado2\SDK\2019.1'))
    a = p.parse_args()
    bindir = a.sdk / 'gnu/aarch32/nt/gcc-arm-none-eabi/bin'
    gcc = bindir / 'arm-none-eabi-gcc.exe'
    size = bindir / 'arm-none-eabi-size.exe'
    if not gcc.is_file():
        p.error('Compiler not found; pass --sdk with your SDK 2019.1 directory.')
    src = ROOT / 'project/sdk/AD9361/src'
    bsp = ROOT / 'project/sdk/AD9361_bsp/ps7_cortexa9_0'
    out = ROOT / 'build/sdk'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'Xilinx.spec').write_text('*startfile:\ncrti%O%s crtbegin%O%s\n', encoding='ascii')
    flags = ['-mcpu=cortex-a9', '-mfpu=vfpv3', '-mfloat-abi=hard']
    objects = []
    report = dict(kind='offline_build', started=datetime.now().astimezone().isoformat(), commands=[])
    with (out / 'build.log').open('w', encoding='utf-8') as log:
        def run(cmd):
            report['commands'].append([str(x) for x in cmd])
            result = subprocess.run(cmd, cwd=out, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            output = result.stdout.decode('utf-8', errors='replace')
            log.write(output); log.flush()
            print(output, end='', flush=True)
            if result.returncode:
                raise RuntimeError(f'Command failed ({result.returncode}); see {out / "build.log"}')
        try:
            for f in sorted(src.glob('*.c')):
                obj = out / (f.stem + '.o')
                run([gcc, *flags, '-Wall', '-O0', '-g3', '-c', '-fmessage-length=0',
                     '-I' + str(bsp / 'include'), '-o', obj, f])
                objects.append(obj)
            if len(objects) != 11:
                raise RuntimeError(f'Expected 11 SDK C sources, found {len(objects)}')
            run([gcc, *flags, '-Wl,-build-id=none', '-specs=Xilinx.spec',
                 '-Wl,-T', '-Wl,' + str(src / 'lscript.ld'), '-L' + str(bsp / 'lib'),
                 '-o', out / 'AD9361.elf', *objects, '-lm',
                 '-Wl,--start-group,-lxil,-lgcc,-lc,--end-group',
                 '-Wl,--start-group,-lxil,-llwip4,-lgcc,-lc,--end-group'])
            run([size, out / 'AD9361.elf'])
            report['status'] = 'PASS'
        except Exception as e:
            report.update(status='FAIL', error=str(e))
            raise
        finally:
            report['finished'] = datetime.now().astimezone().isoformat()
            (out / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('Offline SDK build PASS:', out / 'AD9361.elf')


if __name__ == '__main__':
    main()
