#!/usr/bin/env python3
"""Runs a Windows netplay test instance's exe under Proton the way Steam does, then stops it.

Started by win-launch.sh (which sources win-env.sh and exports WT, I, INST, PROTON, SLR, STEAM,
AC6NP_WIN_EXE). Mirrors the Linux test install's ac6-launch.py: private lock, GPU dpm policy
restored on exit, and on SIGTERM it closes only this instance's own windows and processes.

Steam's launch of a Proton title is
  <SteamLinuxRuntime>/_v2-entry-point --verb=waitforexitandrun -- <Proton>/proton waitforexitandrun <exe>
with STEAM_COMPAT_CLIENT_INSTALL_PATH / STEAM_COMPAT_DATA_PATH set; this does the same with a
prefix of the instance's own (<inst>/pfx). AC6NP_WIN_CONTAINER=0 runs `proton run` directly.
"""
import fcntl
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def descendants(pid):
    """Every live descendant of pid (via /proc/*/stat ppid)."""
    children = {}
    for stat in Path('/proc').glob('[0-9]*/stat'):
        try:
            fields = stat.read_text().rsplit(')', 1)[1].split()
            children.setdefault(int(fields[1]), []).append(int(stat.parent.name))
        except (OSError, IndexError, ValueError):
            pass
    out, todo = [], [pid]
    while todo:
        for c in children.get(todo.pop(), []):
            out.append(c)
            todo.append(c)
    return out


def cmdline(pid):
    try:
        return Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace')
    except OSError:
        return ''


def main():
    inst = Path(os.environ['I'])
    proton = Path(os.environ['PROTON'])
    slr = os.environ.get('SLR', '')
    steam = os.environ.get('STEAM', '/home/deck/.local/share/Steam')
    exe_name = os.environ.get('AC6NP_WIN_EXE', 'ac6recomp.exe')
    exe = inst / 'bin' / exe_name
    env = dict(os.environ)
    for key in ('LD_PRELOAD', 'LD_LIBRARY_PATH', 'ENABLE_VULKAN_RENDERDOC_CAPTURE',
                'ENABLE_VK_LAYER_VALVE_steam_overlay_1', 'ENABLE_VK_LAYER_VALVE_steam_fossilize_1',
                'SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT', 'SDL_GAMECONTROLLER_IGNORE_DEVICES',
                'SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD', 'WINEPREFIX', 'WINEDLLOVERRIDES',
                'AC6_ALLOW_DEBUG_ATTACH', 'AC6_TIMING_TRACE', 'AC6_TRACE_SAVE_IO',
                'VK_DRIVER_FILES', 'VK_ICD_FILENAMES'):
        env.pop(key, None)
    env.setdefault('XDG_RUNTIME_DIR', '/run/user/1000')
    cache = inst / 'cache'
    for d in ('shader', 'mesa'):
        (cache / d).mkdir(parents=True, exist_ok=True)
    env.update(
        STEAM_COMPAT_CLIENT_INSTALL_PATH=steam,
        STEAM_COMPAT_DATA_PATH=str(inst / 'pfx'),
        STEAM_COMPAT_INSTALL_PATH=str(inst / 'bin'),
        STEAM_COMPAT_LIBRARY_PATHS=f'{steam}/steamapps:/run/media/deck/SN256/steamapps',
        STEAM_COMPAT_TOOL_PATHS=':'.join(p for p in (str(proton), slr) if p),
        # pressure-vessel exposes these host paths inside the container (instance files, ISO).
        STEAM_COMPAT_MOUNTS='/run/media/deck/SN256:/home/deck/Games/ACE6-Recomp',
        STEAM_COMPAT_SHADER_PATH=str(cache / 'shader'),
        MESA_SHADER_CACHE_DIR=str(cache / 'mesa'),   # /home is full; keep caches on the SD card
        SteamAppId='0', SteamGameId='0', STEAM_COMPAT_APP_ID='0',
        PULSE_SERVER='unix:' + env['XDG_RUNTIME_DIR'] + '/pulse/native',
        DISABLE_VULKAN_RENDERDOC_CAPTURE_1_36='1',
        VK_LOADER_LAYERS_DISABLE='VK_LAYER_RENDERDOC_Capture',
        WINEDEBUG=os.environ.get('AC6NP_WINEDEBUG', '-all'),
        AC6_NR2_PERF_LOG='0', AC6_NR2_FRAME_STATS='0')
    if os.environ.get('AC6NP_PROTON_LOG'):
        env.update(PROTON_LOG='1', PROTON_LOG_DIR=str(inst / 'history'))
    lock = open(Path(env['XDG_RUNTIME_DIR']) / f"ac6-netplay-win-{os.environ.get('INST', 'W')}.lock", 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print('this Windows test instance is already running.', file=sys.stderr)
        return 1
    if os.environ.get('AC6NP_WIN_CONTAINER', '1') != '0' and slr:
        cmd = [f'{slr}/_v2-entry-point', '--verb=waitforexitandrun', '--',
               str(proton / 'proton'), 'waitforexitandrun', str(exe), *sys.argv[2:]]
    else:
        cmd = [str(proton / 'proton'), 'run', str(exe), *sys.argv[2:]]
    print('win-run:', ' '.join(cmd), flush=True)

    dpm = Path('/sys/class/drm/card0/device/power_dpm_force_performance_level')
    helper = '/usr/bin/steamos-polkit-helpers/steamos-priv-write'
    tune = env.get('AC6_DECK_TUNE', '1') == '1' and not env.get('AC6NP_DUAL')
    previous, changed, child, stopping = None, False, None, False

    def stop(*_):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    def write_gpu(value):
        try:
            return subprocess.run([helper, str(dpm), value], stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, timeout=5).returncode == 0
        except (OSError, subprocess.TimeoutExpired):
            return False

    def game_pids():
        # The Windows process itself: argv[0] is its Windows path (D:\...\ac6recomp.exe).
        def argv0(p):
            return cmdline(p).split(' ')[0].replace('\\', '/').rsplit('/', 1)[-1].lower()
        return [p for p in descendants(child.pid) if argv0(p) == exe_name.lower()]

    def wineserver_kill():
        # Ends every process of this instance's own prefix only (each instance has its own).
        ws = proton / 'files' / 'bin' / 'wineserver'
        if ws.exists():
            subprocess.run([str(ws), '-k'], env=dict(env, WINEPREFIX=str(inst / 'pfx' / 'pfx')),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)

    try:
        if tune and env.get('AC6_NO_GPU_PIN', '0') != '1' and dpm.exists():
            previous = dpm.read_text().strip()
            if previous != 'high':
                changed = write_gpu('high')
        child = subprocess.Popen(cmd, cwd=inst / 'bin', env=env)
        seen = None
        while child.poll() is None and not stopping:
            pids = game_pids()
            if pids and pids[0] != seen:
                seen = pids[0]
                (inst / 'game.pid').write_text(f'{seen}\n')
            time.sleep(0.5)
        if child.poll() is None:
            # Close this launch's own windows first (never send gameplay inputs).
            for pid in descendants(child.pid):
                try:
                    wins = subprocess.run(['xdotool', 'search', '--pid', str(pid)], env=env,
                                          capture_output=True, text=True, timeout=3).stdout.split()
                    for w in wins:
                        subprocess.run(['xdotool', 'windowquit', w], env=env, timeout=3,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                except (OSError, subprocess.TimeoutExpired):
                    pass
            try:
                child.wait(timeout=8)
            except subprocess.TimeoutExpired:
                wineserver_kill()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
        rc = child.poll()
        print(f'win-run: exited rc={rc}' + (' (stopped)' if stopping else ''), flush=True)
        return 0 if rc == 0 or stopping else 1
    finally:
        if child is not None and child.poll() is None:
            left = descendants(child.pid)
            child.terminate()
            for pid in left:
                try:
                    os.kill(pid, signal.SIGTERM)
                except OSError:
                    pass
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                for pid in left:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except OSError:
                        pass
        try:
            (inst / 'game.pid').unlink()
        except OSError:
            pass
        if changed:
            try:
                if dpm.read_text().strip() == 'high':
                    write_gpu(previous)
            except OSError:
                pass


if __name__ == '__main__':
    sys.exit(main())
