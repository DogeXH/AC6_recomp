#!/usr/bin/env python3
# Drives a running test instance (input script "repl <dir>/repl") from boot to AC6's main menu,
# then through a fixed menu sequence for a role. It observes the game only through the harness's
# in-process screenshots and classifies them against reference captures in <run>/refs.
#
#   ac6_drive.py <instance-dir> boot              # until the main menu (CAMPAIGN highlighted)
#   ac6_drive.py <instance-dir> host-player       # main menu -> Player Match -> Create Session -> lobby
#   ac6_drive.py <instance-dir> join-quick        # main menu -> Player Match -> Quick Match
#   ac6_drive.py <instance-dir> ready             # client lobby: move to READY and press it
#   ac6_drive.py <instance-dir> start             # host lobby: move to START MATCH and press it
#   ac6_drive.py <instance-dir> classify          # print the current screen's best reference
#
# Run with python3 -I. Needs Pillow.
import os
import subprocess
import sys
import time

from PIL import Image, ImageChops, ImageStat

TOOLS = os.path.dirname(os.path.abspath(__file__))
RUN = os.environ.get("AC6NP_RUN_ROOT", "/Users/cole/ACE6-Recomp/ac6-online-20261019/run")
REFS = os.path.join(RUN, "refs")
_ref_cache = {}


def repl(inst, lines, shot=None, timeout=300):
    cmd = [os.path.join(TOOLS, "repl_cmd.sh"), inst, "--timeout", str(timeout)]
    if shot:
        cmd += ["--shot", shot]
    cmd += ["--"] + lines
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip().splitlines()
    return out[-1] if shot and out else None


def thumb(path):
    im = Image.open(path).convert("L").resize((64, 36))
    return im


def refs():
    # Built locally and published whole: pair tests classify from two threads at once, and a
    # half-filled shared cache gave the other thread a single reference.
    global _ref_cache
    if not _ref_cache:
        cache = {}
        for f in sorted(os.listdir(REFS)):
            if f.endswith(".png"):
                cache[f[:-4]] = thumb(os.path.join(REFS, f))
        _ref_cache = cache
    return _ref_cache


TEXT_BOX = (380, 280, 900, 335)  # dialog message strip in the 1280x720 capture
_text_cache = {}


def text_crop(path):
    return Image.open(path).convert("L").crop(TEXT_BOX).resize((130, 14))


def disambiguate(path, names):
    """Picks among dialog screens that differ only in their message text."""
    best = None
    for n in names:
        if n not in _text_cache:
            _text_cache[n] = text_crop(os.path.join(REFS, n + ".png"))
        d = ImageStat.Stat(ImageChops.difference(text_crop(path), _text_cache[n])).mean[0]
        if best is None or d < best[0]:
            best = (d, n)
    return best[1]


DIALOGS = ("load_prompt", "autosave", "load_complete", "cancel_load", "gamedata_missing",
           "checking_dlc")


def classify(path):
    name, score, second, mean, stddev = _classify(path)
    if name in DIALOGS and score < 12:
        name = disambiguate(path, DIALOGS)
    return name, score, second, mean, stddev


def _classify(path):
    t = thumb(path)
    scores = []
    for name, r in refs().items():
        diff = ImageStat.Stat(ImageChops.difference(t, r)).mean[0]
        scores.append((diff, name))
    scores.sort()
    mean = ImageStat.Stat(t).mean[0]
    stddev = ImageStat.Stat(t).stddev[0]
    return scores[0][1], scores[0][0], scores[1][1], mean, stddev


def snap(inst, tag):
    return repl(inst, ["wait 50"], shot=tag)


def boot(inst, max_steps=90):
    n = 0
    for step in range(max_steps):
        png = snap(inst, f"boot{step:02d}")
        name, score, second, mean, stddev = classify(png)
        state = name if score < 12 else ("blank" if stddev < 4 else "unknown")
        print(f"boot step {step}: {state} (best {name} {score:.1f}, next {second}, sd {stddev:.1f})", flush=True)
        if state == "main_menu":
            return True
        if state in ("blank", "checking_dlc"):
            repl(inst, ["wait 3000"])
        elif state == "title":
            repl(inst, ["press START 200", "wait 4000"])
        elif state == "gamedata":
            repl(inst, ["press A 200", "wait 4000"])
        elif state == "load_prompt":
            repl(inst, ["press LEFT 200", "wait 500", "press A 200", "wait 6000"])
        elif state == "load_complete":
            repl(inst, ["press A 200", "wait 4000"])
        elif state == "autosave":
            repl(inst, ["press A 200", "wait 5000"])  # NO is the default
        elif state == "cancel_load":
            repl(inst, ["press A 200", "wait 3000"])  # NO (keep loading) is the default
        elif state == "gamedata_missing":
            repl(inst, ["press A 200", "wait 3000"])
        elif stddev > 30:  # legal screens and the intro movie (detailed imagery) can be skipped
            repl(inst, ["press START 200", "wait 3000"])
        else:  # dark, flat loading screens: START there asks to cancel the load
            repl(inst, ["wait 3000"])
        n += 1
    return False


SLOW = ["set gap_ms 350"]


def host_player(inst):
    repl(inst, SLOW + ["press DOWN 200", "wait 900", "press A 200", "wait 3000",  # ONLINE
                       "press A 200", "wait 2500",                                 # PLAYER MATCH
                       "press DOWN 200", "wait 500", "press DOWN 200", "wait 700",
                       "press A 200", "wait 4000",                                 # CREATE SESSION
                       "loop_start 10", "press DOWN 200", "loop_end", "wait 900"])
    png = snap(inst, "host_check")
    name, score, *_ = classify(png)
    print("before create:", name, round(score, 1))
    if name != "create_session_button" or score > 12:
        print("not on the CREATE SESSION button; aborting (see", png, ")")
        return False
    repl(inst, ["press A 200", "wait 6000"])
    return True


def join_quick(inst):
    repl(inst, SLOW + ["press DOWN 200", "wait 900", "press A 200", "wait 3000",  # ONLINE
                       "press A 200", "wait 2500",                                 # PLAYER MATCH
                       "press A 200", "wait 8000"])                                # QUICK MATCH
    return True


def ready(inst):
    repl(inst, SLOW + ["loop_start 6", "press DOWN 200", "loop_end", "wait 800"])
    png = snap(inst, "ready_check")
    name, score, *_ = classify(png)
    print("ready check:", name, round(score, 1))
    if name != "client_lobby_ready":
        return False
    repl(inst, ["press A 200", "wait 1500"])
    return True


def start(inst):
    repl(inst, SLOW + ["loop_start 7", "press DOWN 200", "loop_end", "wait 900"])
    png = snap(inst, "start_check")
    name, score, *_ = classify(png)
    print("start check:", name, round(score, 1))
    if name != "host_lobby_start":
        return False
    repl(inst, ["press A 200"])
    return True


# --- Verified cursor movement --------------------------------------------------------------
# Menu rows (y centre in the 1280x720 capture) and the x span to sample. The highlighted row is
# the brightest one; each press is checked against a fresh capture so dropped inputs are retried.
LOBBY_HOST = {"x": (145, 350), "rows": {"aircraft": 232, "special": 262, "handicap": 292,
              "team": 322, "invite": 367, "rules": 412, "gamercard": 457, "start": 507}}
LOBBY_CLIENT = {"x": (145, 350), "rows": {"aircraft": 232, "special": 262, "handicap": 292,
                "team": 322, "invite": 367, "gamercard": 457, "ready": 507}}
CREATE = {"x": (145, 400), "rows": {"players": 162, "reserved": 192, "stage": 267, "npc": 297,
          "time": 327, "rank": 357, "actype": 387, "handi": 417, "wpnlv": 447, "voice": 477,
          "create": 527}}


def highlighted(png, layout):
    im = Image.open(png).convert("L")
    x0, x1 = layout["x"]
    best, best_v = None, -1
    for name, y in layout["rows"].items():
        v = ImageStat.Stat(im.crop((x0, y - 8, x1, y + 8))).mean[0]
        if v > best_v:
            best, best_v = name, v
    return best, best_v


def goto(inst, layout, target, tries=16):
    order = list(layout["rows"])
    for i in range(tries):
        png = snap(inst, f"goto_{target}_{i:02d}")
        cur, v = highlighted(png, layout)
        print(f"goto {target}: at {cur} ({v:.0f})", flush=True)
        if cur == target and v > 120:
            return True
        key = "DOWN" if order.index(target) > order.index(cur) else "UP"
        repl(inst, [f"press {key} 250", "wait 700"])
    return False


def player_ready(host_inst, row):
    """Reads the ready marker ("?" box) of player-list row `row` (1-based) on the host."""
    png = snap(host_inst, f"readymark{row}")
    im = Image.open(png).convert("L")
    y = 81 + 30 * (row - 1)
    return ImageStat.Stat(im.crop((566, y - 8, 586, y + 8))).mean[0] > 90


def ready(inst, host_inst=None, row=2, attempts=4):
    for attempt in range(attempts):
        if host_inst and player_ready(host_inst, row):
            return True
        if not goto(inst, LOBBY_CLIENT, "ready"):
            return False
        repl(inst, ["wait 600", "press A 250", "wait 1500"])
        if not host_inst:
            return True
        if player_ready(host_inst, row):
            return True
        print(f"ready: host does not show row {row} ready yet (attempt {attempt + 1})", flush=True)
    return False


def start(inst):
    return press_confirmed(inst, LOBBY_HOST, "start", '"ev":"xgi.start"', wait_s=10)


def press_confirmed(inst, layout, target, needle, attempts=4, wait_s=8):
    """Moves to `target`, lets the cursor settle, presses A and waits for `needle` in the trace;
    retries when the press was swallowed."""
    for attempt in range(attempts):
        if not goto(inst, layout, target):
            return False
        start_len = trace_len(inst)
        repl(inst, ["wait 600", "press A 250"])
        if wait_trace(inst, needle, wait_s, start_len):
            return True
        print(f"press {target}: no {needle} yet (attempt {attempt + 1})", flush=True)
    return False


def host_player(inst):
    repl(inst, SLOW + ["press DOWN 250", "wait 900", "press A 250", "wait 3000",  # ONLINE
                       "press A 250", "wait 2500",                                 # PLAYER MATCH
                       "press DOWN 250", "wait 600", "press DOWN 250", "wait 800",
                       "press A 250", "wait 4000"])                                # CREATE SESSION
    if not press_confirmed(inst, CREATE, "create", '"ev":"xgi.create"'):
        return False
    repl(inst, ["wait 5000"])
    return True


# --- Verified main-menu tree navigation -------------------------------------------------------
MENU_ROWS = (153, 198, 243, 288, 333, 378)


def menu_info(png):
    """(depth, highlighted row index) for AC6's checkbox menus, or (None, None)."""
    im = Image.open(png).convert("L")
    boxes = [ImageStat.Stat(im.crop((142, y - 12, 168, y + 12))).mean[0] for y in MENU_ROWS]
    hi = max(range(len(boxes)), key=lambda i: boxes[i])
    if boxes[hi] < 150 or sorted(boxes)[-2] > 120:
        return None, None
    # Real menus leave a dark gap between the checkbox and the label; settings screens draw a
    # full-width highlight bar there instead.
    y = MENU_ROWS[hi]
    if ImageStat.Stat(im.crop((174, y - 6, 180, y + 6))).mean[0] > 100:
        return None, None
    l2 = ImageStat.Stat(im.crop((230, 63, 420, 75))).mean[0]
    l3 = ImageStat.Stat(im.crop((230, 83, 420, 95))).mean[0]
    depth = 3 if l3 > 35 else (2 if l2 > 28 else 1)
    return depth, hi


# ONLINE menu row: 0 = PLAYER MATCH, 1 = RANKED MATCH (AC6NP_RANKED=1).
MATCH_ROW = 1 if os.environ.get("AC6NP_RANKED") else 0


def nav_menu(inst, path, final_press=True, tries=30):
    """path = row index per menu level, e.g. [1, 0, 2] = ONLINE > PLAYER MATCH > CREATE SESSION."""
    unknown = 0
    for i in range(tries):
        png = snap(inst, f"nav_{i:02d}")
        depth, hi = menu_info(png)
        print(f"nav {path}: depth {depth} row {hi}", flush=True)
        if depth is None:
            unknown += 1
            if unknown > 5:
                return False
            repl(inst, ["wait 1200"])  # transition animation, or a dialog still closing
            continue
        unknown = 0
        if depth > len(path):
            repl(inst, ["press B 250", "wait 1500"])
            continue
        want = path[depth - 1]
        if hi != want:
            repl(inst, [f"press {'DOWN' if want > hi else 'UP'} 250", "wait 700"])
            continue
        if depth == len(path):
            if final_press:
                repl(inst, ["wait 400", "press A 250", "wait 2500"])
            return True
        repl(inst, ["wait 400", "press A 250", "wait 2500"])
    return False


def host_player(inst):
    # The A on CREATE SESSION is sometimes swallowed; the settings screen is not a checkbox menu,
    # so stay in nav_menu (which presses A again) until the menu is gone.
    for attempt in range(4):
        if not nav_menu(inst, [1, MATCH_ROW, 2]):
            return False
        repl(inst, ["wait 2000"])
        if menu_info(snap(inst, f"host_entered_{attempt}"))[0] is None:
            break
        print(f"host: still on the menu after CREATE SESSION (attempt {attempt + 1})", flush=True)
    else:
        return False
    # AC6NP_TIME_LEFT=n: press LEFT n times on TIME LIMIT (default 10 min) for a shorter match.
    time_left = int(os.environ.get("AC6NP_TIME_LEFT", "0") or 0)
    if time_left > 0 and goto(inst, CREATE, "time"):
        for _ in range(time_left):
            repl(inst, ["press LEFT 250", "wait 700"])
        snap(inst, "host_time_limit")
    if not press_confirmed(inst, CREATE, "create", '"ev":"xgi.create"'):
        return False
    repl(inst, ["wait 5000"])
    return True


def join_quick(inst):
    start_len = trace_len(inst)
    for attempt in range(4):
        if not nav_menu(inst, [1, MATCH_ROW, 0]):
            return False
        if wait_trace(inst, '"ev":"xgi.search"', 12, start_len):
            return True
        print(f"join-quick: no search yet (attempt {attempt + 1})", flush=True)
    return False


def remote(inst):
    """(user@host, remote dir) for a remote instance mirror (<inst>/remote), else None."""
    try:
        with open(os.path.join(inst, "remote")) as f:
            host, rdir = f.read().split()[:2]
            return host, rdir
    except (OSError, ValueError):
        return None


def ssh(inst, command, **kw):
    host, _ = remote(inst)
    opts = ["-o", "BatchMode=yes", "-o", "ControlMaster=auto", "-o",
            "ControlPath=" + os.path.expanduser("~/.ssh/ac6np-%C"), "-o", "ControlPersist=20m"]
    kh = os.path.join(inst, "known_hosts")
    if os.path.exists(kh):
        opts += ["-o", "UserKnownHostsFile=" + kh]
    return subprocess.run(["ssh", *opts, host, command], capture_output=True, text=True, **kw)


def inst_path(inst, *parts):
    """A path inside the instance as the game sees it (the remote dir for remote instances)."""
    r = remote(inst)
    return os.path.join(r[1] if r else inst, *parts)


def trace_lines(inst):
    if remote(inst):
        return ssh(inst, f"cat '{inst_path(inst, 'trace.jsonl')}' 2>/dev/null").stdout.splitlines()
    try:
        with open(os.path.join(inst, "trace.jsonl")) as f:
            return f.read().splitlines()
    except OSError:
        return []


def trace_len(inst):
    if remote(inst):
        out = ssh(inst, f"wc -l < '{inst_path(inst, 'trace.jsonl')}' 2>/dev/null").stdout.strip()
        return int(out) if out.isdigit() else 0
    return len(trace_lines(inst))


def trace_has(inst, needle, start_len=0):
    if remote(inst):
        q = needle.replace("'", "'\\''")
        r = ssh(inst, f"tail -n +{start_len + 1} '{inst_path(inst, 'trace.jsonl')}' 2>/dev/null"
                      f" | grep -m1 -F -q -- '{q}'")
        return r.returncode == 0
    return any(needle in l for l in trace_lines(inst)[start_len:])


def wait_trace(inst, needle, timeout_s, start_len=0):
    end = time.time() + timeout_s
    while time.time() < end:
        if trace_has(inst, needle, start_len):
            return True
        time.sleep(0.5)
    return False


def pair_test(run_s=120, boot_first=True):
    """Host on A, quick-match on B, start a Player Match and record it."""
    import threading
    insts = os.path.join(RUN, "instances")
    # AC6NP_PAIR="<host> <client>" picks the instances (default "A B"); either may be remote.
    host_name, client_name = os.environ.get("AC6NP_PAIR", "A B").split()[:2]
    a, b = os.path.join(insts, host_name), os.path.join(insts, client_name)
    results = {a: True, b: True}
    ts = [threading.Thread(target=lambda i=i: results.__setitem__(i, boot(i))) for i in (a, b)]
    for t in ts if boot_first else []:
        t.start()
    for t in ts if boot_first else []:
        t.join()
    if not all(results.values()):
        print("pair-test: boot failed", results)
        return False
    if not host_player(a):
        print("pair-test: host failed")
        return False
    if not wait_trace(a, '"ev":"xgi.create"', 15):
        print("pair-test: host session not created")
        return False
    if not join_quick(b) or not wait_trace(b, '"ev":"xgi.join","handle"', 20):
        print("pair-test: B never joined")
        return False
    if not wait_trace(a, '"remote":1', 20):
        print("pair-test: host never saw the remote join")
        return False
    time.sleep(2)
    if not ready(b, host_inst=a) or not start(a):
        print("pair-test: ready/start failed")
        return False
    for i in (a, b):
        d = os.path.join(i, "shots", "match")
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
        if remote(i):
            ssh(i, f"rm -rf '{inst_path(i, 'shots', 'match')}'; mkdir -p '{inst_path(i, 'shots', 'match')}'")
        repl(i, [f"screenshot_every 3000 {inst_path(i, 'shots', 'match')}"])
    print(f"pair-test: match started, recording {run_s} s", flush=True)
    if os.environ.get("AC6NP_SAMPLE"):
        time.sleep(10)
        procs = []
        for i in (a, b):
            if remote(i):
                continue
            pid = open(os.path.join(i, "ac6recomp.pid")).read().strip()
            out = os.path.join(i, "sample_sync.txt")
            procs.append(subprocess.Popen(["/usr/bin/sample", pid, "5", "-file", out],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        for p in procs:
            p.wait()
        print("pair-test: sampled both processes", flush=True)
        run_s = max(0, run_s - 15)
    time.sleep(run_s)
    for i in (a, b):
        repl(i, ["screenshot_stop"])
        if remote(i):
            fetch_remote(i)
    return True


def fetch_remote(inst):
    """Copies a remote instance's match screenshots (as PNG) and trace into its local mirror."""
    host, rdir = remote(inst)
    local = os.path.join(inst, "shots", "match")
    os.makedirs(local, exist_ok=True)
    names = ssh(inst, f"ls '{rdir}/shots/match' 2>/dev/null").stdout.split()
    for n in names:
        if not n.endswith(".ppm"):
            continue
        dst = os.path.join(local, n)
        with open(dst, "wb") as f:
            subprocess.run(f"ssh -o BatchMode=yes -o ControlPath={os.path.expanduser('~/.ssh/ac6np-%C')} "
                           f"{host} \"gzip -1 -c '{rdir}/shots/match/{n}'\" | gunzip", shell=True,
                           stdout=f, check=False)
        subprocess.run(["sips", "-s", "format", "png", dst, "--out", dst[:-4] + ".png"],
                       capture_output=True)
        os.remove(dst)
    with open(os.path.join(inst, "trace.jsonl"), "w") as f:
        f.write("\n".join(trace_lines(inst)) + "\n")


def main():
    inst, action = sys.argv[1], sys.argv[2]
    if action == "classify":
        png = snap(inst, "classify")
        print(png, classify(png))
        return 0
    if action in ("pair-test", "pair-continue"):
        ok = pair_test(int(sys.argv[3]) if len(sys.argv) > 3 else 120,
                       boot_first=action == "pair-test")
        print(action, "ok" if ok else "FAILED")
        return 0 if ok else 1
    ok = {"boot": boot, "host-player": host_player, "join-quick": join_quick,
          "ready": ready, "start": start}[action](inst)
    print(action, "ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
