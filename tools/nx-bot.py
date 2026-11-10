#!/usr/bin/env python3
"""Drive a Switch running the sys-botbase sysmodule (TCP port 6000) from this computer.

  tools/nx-bot.py <ip> click A              one press (also: B X Y L R ZL ZR PLUS MINUS HOME CAPTURE
                                            DUP DDOWN DLEFT DRIGHT LSTICK RSTICK)
  tools/nx-bot.py <ip> press R              hold a button until `release`
  tools/nx-bot.py <ip> release R
  tools/nx-bot.py <ip> stick LEFT 0 32767   stick position, -32768..32767 each axis (RIGHT for the C-stick)
  tools/nx-bot.py <ip> seq "click A; wait 0.5; press R; wait 1; click A; release R"
  tools/nx-bot.py <ip> shot out.jpg         screenshot of the console's display
  tools/nx-bot.py <ip> raw "getVersion"     any sys-botbase command

Button names follow sys-botbase. `hbmenu-netloader` is a canned sequence: from the HOME menu, hold R
and launch the first game tile, wait for the Homebrew Menu, press Y so the netloader listens.
"""
import socket, sys, time

PORT = 6000


class Bot:
    def __init__(self, ip):
        self.sock = socket.create_connection((ip, PORT), timeout=10)
        self.sock.settimeout(10)

    def send(self, cmd):
        self.sock.sendall((cmd + "\r\n").encode())

    def read_line(self, timeout=10):
        self.sock.settimeout(timeout)
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = self.sock.recv(65536)
            if not chunk:
                break
            buf += chunk
        return buf.decode(errors="replace").strip()

    # A held press: sys-botbase's own "click" is sometimes too short for the system to register.
    def click(self, b): self.send(f"press {b}"); time.sleep(0.12); self.send(f"release {b}")
    def press(self, b): self.send(f"press {b}")
    def release(self, b): self.send(f"release {b}")
    def stick(self, which, x, y): self.send(f"setStick {which} {int(x):#x} {int(y):#x}")

    def screenshot(self, path):
        self.send("pixelPeek")
        data = self.read_line(timeout=30)
        with open(path, "wb") as f:
            f.write(bytes.fromhex(data))
        return path

    def seq(self, script):
        for step in [s.strip() for s in script.split(";") if s.strip()]:
            parts = step.split()
            if parts[0] == "wait":
                time.sleep(float(parts[1]))
            elif parts[0] in ("click", "press", "release"):
                getattr(self, parts[0])(parts[1]); time.sleep(0.05)
            elif parts[0] == "stick":
                self.stick(parts[1], parts[2], parts[3]); time.sleep(0.05)
            else:
                self.send(step); time.sleep(0.05)


def classify(path):
    """What the console shows, from a few pixel probes of a 1280x720 screenshot."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = lambda x, y: im.getpixel((int(x * w / 1280), int(y * h / 720)))
    def near(c, ref, tol=28): return all(abs(a - b) <= tol for a, b in zip(c, ref))
    bottom = [px(x, 690) for x in (200, 640, 1100)]
    grey = lambda c, v, tol=10: all(abs(ch - v) <= tol for ch in c)
    if all(c[1] > 120 and c[2] > 110 for c in bottom): return "hbmenu"              # teal bar
    if all(grey(c, 70) for c in bottom) and grey(px(640, 357), 70, 12): return "picker"   # "Who is using the software?"
    left = [px(x, 690) for x in (200, 400, 640)]      # the bar's right half carries button hints
    if all(grey(c, 45) for c in left) and grey(px(640, 357), 45, 12):
        return "home-playing" if sum(px(849, 675)) > 500 else "home"   # white (X) glyph: "Close Software" offered
    band = [px(x, 245) for x in (300, 640, 1000)]
    if all(c[0] > 150 and c[1] < 80 and c[2] < 80 for c in band): return "ready"    # READY TO FIGHT banner
    ok_button = px(461, 564)
    if ok_button[1] > 170 and ok_button[2] > 170 and ok_button[0] < 170 and sum(px(640, 200)) < 30: return "error"   # "The software was closed because an error occurred."
    if all(sum(c) < 30 for c in bottom) and sum(px(640, 357)) < 30: return "black"
    return "unknown"


def quit_app(bot, shots_dir):
    """Close whatever application is running or suspended (HOME, X on its tile, A to confirm)."""
    import os
    os.makedirs(shots_dir, exist_ok=True)
    for step in range(6):
        path = os.path.join(shots_dir, f"quit-{step}.jpg")
        bot.screenshot(path)
        state = classify(path)
        print(f"[quit] step {step}: {state}", flush=True)
        if state == "hbmenu": return True
        if state == "error": bot.click("A"); time.sleep(2.5); continue
        if state == "home": return True
        if state == "home-playing":
            bot.click("X"); time.sleep(1.2)          # "Close the software?"
            bot.click("A"); time.sleep(3); continue
        bot.click("HOME"); time.sleep(1.5)
    return False


def launch_hbmenu(bot, shots_dir):
    """From wherever the console is, reach the Homebrew Menu with the application's memory
    (hold R while the first game tile launches) and arm the netloader with Y."""
    import os
    os.makedirs(shots_dir, exist_ok=True)
    # Re-attach sys-botbase's virtual controller: Change Grip/Order (or a reconnect) drops it, and
    # every press is then silently ignored. The next input after a detach attaches it afresh.
    bot.send("detachController"); time.sleep(1.5)
    held_r = False
    for step in range(30):
        path = os.path.join(shots_dir, f"launch-{step}.jpg")
        bot.screenshot(path)
        state = classify(path)
        print(f"[launch] step {step}: {state}", flush=True)
        if state == "hbmenu":
            if held_r: bot.release("R"); held_r = False
            time.sleep(0.5); bot.click("Y"); return True
        if state == "home":
            if not held_r: bot.press("R"); held_r = True; time.sleep(0.4)
            bot.click("A"); time.sleep(1.5); continue
        if state == "home-playing":
            if held_r: bot.release("R"); held_r = False
            bot.click("X"); time.sleep(1.2); bot.click("A"); time.sleep(3); continue
        if state == "picker":
            if not held_r: bot.press("R"); held_r = True; time.sleep(0.4)
            bot.click("A"); time.sleep(2.5); continue
        if state == "error":
            print("[launch] the previous run crashed on exit (Atmosphère error screen); dismissing", flush=True)
            if held_r: bot.release("R"); held_r = False
            bot.click("A"); time.sleep(2.5); continue
        if state == "black":
            time.sleep(2); continue
        bot.click("B"); time.sleep(1.5)       # a dialog or menu we do not know: back out
    if held_r: bot.release("R")
    return False


def main(argv):
    if len(argv) < 3:
        print(__doc__); return 64
    ip, cmd, args = argv[1], argv[2], argv[3:]
    bot = Bot(ip)
    if cmd in ("click", "press", "release"):
        getattr(bot, cmd)(args[0])
    elif cmd == "stick":
        bot.stick(args[0], args[1], args[2])
    elif cmd == "seq":
        bot.seq(" ".join(args))
    elif cmd == "shot":
        print(bot.screenshot(args[0] if args else "switch.jpg"))
    elif cmd == "raw":
        bot.send(" ".join(args)); print(bot.read_line())
    elif cmd == "hbmenu-netloader":
        shots = args[0] if args else "build/switch-pull/launch"
        ok = quit_app(bot, shots) and launch_hbmenu(bot, shots)
        return 0 if ok else 1
    elif cmd == "quit-app":
        return 0 if quit_app(bot, args[0] if args else "build/switch-pull/launch") else 1
    elif cmd == "state":
        bot.screenshot(args[0] if args else "switch.jpg"); print(classify(args[0] if args else "switch.jpg"))
    else:
        print(__doc__); return 64
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
