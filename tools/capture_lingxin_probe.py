"""Bounded ADB shell capture without PowerShell console wrapping.

Example: python tools/capture_lingxin_probe.py --seconds 45 --output tmp/probe.log
         --command "2:wifi status" --command "5:lingxin start"
Device serial is explicit, and capture owns just one shell for its lifetime.
"""
import argparse
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--seconds', type=float, default=45)
    parser.add_argument('--command', action='append', default=[])
    args = parser.parse_args()
    schedule = sorted((float(item.split(':', 1)[0]), item.split(':', 1)[1]) for item in args.command)
    if not 0 < args.seconds <= 600 or any(at < 0 or at >= args.seconds or '\n' in cmd or '\r' in cmd for at, cmd in schedule):
        parser.error('duration must be 0..600 seconds and commands must be single-line within duration')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation avoids replacing evidence from an earlier run.
    with args.output.open('xb') as output:
        proc = subprocess.Popen(['adb', '-s', args.serial, 'shell'], stdin=subprocess.PIPE,
                                stdout=output, stderr=subprocess.STDOUT)
        start = time.monotonic()
        try:
            while proc.poll() is None and time.monotonic() - start < args.seconds:
                while schedule and time.monotonic() - start >= schedule[0][0]:
                    _, command = schedule.pop(0)
                    proc.stdin.write((command + '\r\n').encode('utf-8'))
                    proc.stdin.flush()
                    print(f'{time.monotonic() - start:.3f}s command: {command}', flush=True)
                time.sleep(0.05)
            if proc.poll() is not None:
                raise RuntimeError(f'ADB shell ended early: code={proc.returncode}')
        finally:
            if proc.poll() is None:
                proc.terminate()
            proc.wait(timeout=10)
    print(f'capture: {args.output.resolve()}', flush=True)


if __name__ == '__main__':
    main()
