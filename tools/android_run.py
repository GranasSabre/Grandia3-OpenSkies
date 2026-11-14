"""Test run on the connected phone (USB or wireless debugging): install the APK, start the game with
test options, wait, fetch the log and screenshots, stop the app.

Usage: python tools/android_run.py [--seconds=N] [--no-install] [--push-iso=PATH] [game options ...]
  game options go into args.txt in the app folder, e.g. --press=start:20 --shots=5
Results: build/android/test/grandia3.log and build/android/test/shots/*.bmp (+ .png)

The disc image is pushed once (--push-iso) as disc1.iso into the app folder; it is only read.
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADB = os.path.join(ROOT, 'toolchain', 'sdk', 'platform-tools', 'adb.exe')
PKG = 'io.github.granassabre.grandia3'
APP_DIR = f'/sdcard/Android/data/{PKG}/files'
OUT = os.path.join(ROOT, 'build', 'android', 'test')


def adb(*args, check=True, quiet=False):
    r = subprocess.run([ADB] + list(args), capture_output=True, text=True, errors='replace')
    if check and r.returncode != 0:
        sys.exit(f'adb {" ".join(args)} failed:\n{r.stdout}{r.stderr}')
    if not quiet and r.stdout.strip():
        print(r.stdout.strip())
    return r.stdout


def main():
    opts = [a for a in sys.argv[1:] if a.startswith(('--seconds=', '--push-iso=', '--screencap=', '--taps=')) or a == '--no-install']
    game_args = [a for a in sys.argv[1:] if a not in opts]
    seconds = next((float(a[10:]) for a in opts if a.startswith('--seconds=')), 60.0)
    devs = [l for l in adb('devices', quiet=True).splitlines()[1:] if l.strip().endswith('device')]
    if not devs:
        sys.exit('no phone found: USB debugging on and the PC allowed on the phone?')
    if '--no-install' not in opts:
        adb('install', '-r', os.path.join(ROOT, 'build', 'android', 'Grandia3.apk'))
    adb('shell', 'am', 'force-stop', PKG, check=False, quiet=True)
    # the app folder exists after the first start
    if adb('shell', f'test -d {APP_DIR} && echo yes', check=False, quiet=True).strip() != 'yes':
        adb('shell', 'monkey', '-p', PKG, '1', quiet=True)
        time.sleep(3)
        adb('shell', 'am', 'force-stop', PKG, quiet=True)
    iso = next((a[11:] for a in opts if a.startswith('--push-iso=')), None)
    if iso:
        print('pushing the disc image (once, a few minutes)...')
        adb('push', iso, f'{APP_DIR}/disc1.iso')
    args_file = os.path.join(OUT, 'args.txt')
    os.makedirs(OUT, exist_ok=True)
    with open(args_file, 'w', newline='\n') as f:
        f.write('\n'.join(game_args) + '\n')
    adb('push', args_file, f'{APP_DIR}/args.txt', quiet=True)
    adb('shell', f'rm -rf {APP_DIR}/shots {APP_DIR}/grandia3.log', check=False, quiet=True)
    adb('logcat', '-c', check=False, quiet=True)
    adb('shell', 'am', 'start', '-n', f'{PKG}/.GrandiaActivity', quiet=True)
    print(f'running {seconds:.0f} s ...')
    # --screencap=SEC,SEC: pictures of the phone's display (what the player sees)
    # --taps=SEC:X:Y,...: touches of 0.15 s at display coordinates (landscape) - tests the touch pad
    events = [(float(x), 'cap', None) for a in opts if a.startswith('--screencap=') for x in a[12:].split(',')]
    events += [(float(t.split(':')[0]), 'tap', t.split(':')[1:3]) for a in opts if a.startswith('--taps=') for t in a[7:].split(',')]
    events.sort(key=lambda e: e[0])
    t0 = time.time()
    ncap = 0
    for t, kind, arg in events:
        time.sleep(max(0.0, t - (time.time() - t0)))
        if kind == 'tap':
            subprocess.run([ADB, 'shell', 'input', 'swipe', arg[0], arg[1], arg[0], arg[1], '150'], capture_output=True)
            continue
        png = subprocess.run([ADB, 'exec-out', 'screencap', '-p'], capture_output=True).stdout
        with open(os.path.join(OUT, f'screen_{ncap}.png'), 'wb') as f:
            f.write(png)
        ncap += 1
    time.sleep(max(0.0, seconds - (time.time() - t0)))
    adb('pull', f'{APP_DIR}/grandia3.log', os.path.join(OUT, 'grandia3.log'), check=False, quiet=True)
    shots = os.path.join(OUT, 'shots')
    if os.path.isdir(shots):
        for f in os.listdir(shots):
            os.remove(os.path.join(shots, f))
    adb('pull', f'{APP_DIR}/shots', OUT, check=False, quiet=True)
    with open(os.path.join(OUT, 'logcat.txt'), 'w', encoding='utf-8') as f:
        f.write(adb('logcat', '-d', '-s', 'Grandia3:*', 'DEBUG:*', 'libc:*', 'AndroidRuntime:*', check=False, quiet=True))
    adb('shell', 'am', 'force-stop', PKG, check=False, quiet=True)
    try:  # BMP -> PNG for viewing
        from PIL import Image
        for f in sorted(os.listdir(shots)) if os.path.isdir(shots) else []:
            if f.endswith('.bmp'):
                Image.open(os.path.join(shots, f)).save(os.path.join(shots, f[:-4] + '.png'))
    except ImportError:
        pass
    log = os.path.join(OUT, 'grandia3.log')
    if os.path.exists(log):
        lines = open(log, encoding='utf-8', errors='replace').read().splitlines()
        print('log:', len(lines), 'lines;', ' | '.join(l for l in lines if 'fps:' in l)[-400:])
    else:
        print('no log: see build/android/test/logcat.txt')


if __name__ == '__main__':
    main()
