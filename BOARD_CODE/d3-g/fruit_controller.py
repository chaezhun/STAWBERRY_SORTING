#!/usr/bin/env python3
"""
Strawberry sorting - control code for the D3-G A72 core
AI-G over TCP -> classify -> position trigger -> IPC -> R5 -> CAN -> VCP-G (servo, LED, LCD)

Sorting is done by angle on a single SG90 servo driving the tilt plate:
  - fresh  : stays at the default 90 degrees (level), so the fruit passes through
  - rotten : tilts 40 degrees left, holds briefly, then returns to level
  - unripe : tilts 40 degrees right, holds briefly, then returns to level

Assumptions:
  - the servo is position controlled; byte 0 of the CAN frame carries the target angle
  - the conveyor runs from its own controller, so this code does not touch it
  - the plate acts when the reported coordinates fall inside the trigger box

Values that get tuned on the rig:
  1. the tilt angles
  2. the trigger box, measured so it lands exactly on the plate position

Usage: sudo python3 fruit_controller.py
IPC_Example.py from the board SDK is invoked as a subprocess in send mode.
"""

import socket
import time
import threading
import subprocess
import os

# ============================================================
# network and CAN identifiers
# ============================================================
AI_G_PORT          = 5000     # TCP port the AI board connects to
SERVO_CAN_ID       = 0x111    # tilt plate servo
CAN_ID_LED_CONTROL = 0x114    # LED control
CAN_ID_LCD_STATS   = 0x116    # LCD statistics
# The second servo and the conveyor channel are no longer used; both are external.

# ============================================================
# Orchestration: this board starts and stops recognition on the AI board over SSH,
#   so the whole run is a single command: fix the local IP, start recognition,
#   act on the results, and shut everything down after 30 seconds of silence.
#   One-time setup: static IP on the AI board, sshd running, and a passwordless key.
# ============================================================
ORCHESTRATE   = True                  # drive the AI board over SSH automatically
SETUP_D3G_NET = True                  # pin the local ethernet address at start-up
AIG_USER      = "root"                # AI board login
AIG_IP        = "192.168.10.100"      # AI board ethernet address
D3G_ETH_IP    = "192.168.10.101"      # D3-G eth0 IP
AIG_START_CMD = "sh /home/root/ai_result_sender.sh"   # start recognition and streaming
AIG_STOP_CMD  = "pkill -f tcnnapp 2>/dev/null; killall tcnnapp 2>/dev/null; true"  # stop it (busybox safe)

def _ssh_aig(remote_cmd, background=False):
    cmd = ["ssh", "-o", "StrictHostKeyChecking=no", "-o", "BatchMode=yes",
           "-o", "ConnectTimeout=5", f"{AIG_USER}@{AIG_IP}", remote_cmd]
    try:
        if background:
            subprocess.Popen(cmd)
        else:
            subprocess.run(cmd, timeout=10)
    except Exception as e:
        print(f"  [ssh error] {e} - check the key and the network")

def start_aig():
    print(f"[ai] starting recognition on {AIG_USER}@{AIG_IP}")
    _ssh_aig(AIG_START_CMD, background=True)

def stop_aig():
    print("[ai] stopping recognition")
    _ssh_aig(AIG_STOP_CMD, background=False)

def setup_d3g_net():
    """Pin the ethernet address so NetworkManager does not clear it."""
    try:
        subprocess.run(["sudo", "nmcli", "device", "set", "eth0", "managed", "no"], timeout=5)
        subprocess.run(["sudo", "ifconfig", "eth0", D3G_ETH_IP, "netmask", "255.255.255.0", "up"], timeout=5)
        print(f"[net] eth0 fixed at {D3G_ETH_IP}")
    except Exception as e:
        print(f"[net] could not configure eth0, do it by hand: {e}")

# ============================================================
# classes and LED colours
# ============================================================
CLASS_FRESH  = 0              # stays level and passes through
CLASS_ROTTEN = 1             # tilts left
CLASS_UNRIPE = 2             # tilts right
CLASS_NAMES  = {0: "fresh", 1: "rotten", 2: "unripe"}
LED_RED    = 1               # rotten
LED_YELLOW = 2               # unripe
LED_GREEN  = 4               # fresh

# ============================================================
# Tuning constants. Everything adjusted on the rig lives here.
# ============================================================
# --- 1. tilt angles ---
SERVO_DEFAULT_DEG = 90       # level, the resting position
SERVO_LEFT_DEG    = 50       # 40 degrees left, for rotten
SERVO_RIGHT_DEG   = 130      # 40 degrees right, for unripe
SERVO_HOLD_TIME   = 1.3      # seconds held tilted, long enough for the fruit to roll off

# --- 2. trigger box: both the X and the Y range must be satisfied ---
#    The belt runs diagonally across the frame, so fruit at the plate always
#    appears at a particular spot. Boxing that spot rejects fruit further up the
#    belt and phantom detections in the corners. Measure with DEBUG_COORDS.
TRIGGER_Y_LO = 390         # top edge, measured at the plate
TRIGGER_Y_HI = 490         # bottom edge
TRIGGER_X_LO = 230         # left edge; real fruit sits at 230-375, corner ghosts at 118
TRIGGER_X_HI = 375         # right edge; fruit enters from the right, so widening this triggers earlier
# --- 2a. a smaller box for unripe fruit ---
#   The model detects unripe fruit on almost every frame, so the debounce fills
#   immediately at the entry edge and the plate fires too early. Pulling the entry
#   edges inward makes the fruit travel further before it triggers.
TRIGGER_X_HI_UNRIPE = 335  # 40 px inside the normal edge
TRIGGER_Y_HI_UNRIPE = 455  # 35 px inside the normal edge
COUNT_GAP       = 1.5       # a class must be absent from the box this long before the next detection counts as a new piece of fruit
TRIGGER_CONFIRM = 3         # consecutive frames required inside the box, which rejects one- and two-frame ghosts

# --- 2b. fresh veto ---
#   The model often mislabels fresh fruit as rotten or unripe, but rarely the other
#   way round. So a 'fresh' label is the trustworthy one: a single fresh detection
#   inside the box blocks the servo for a while, protecting good fruit from being
#   rejected. The veto needs one frame while a reject needs several, so fresh wins.
FRESH_VETO      = True       # set False to make every class trigger independently
FRESH_VETO_TIME = 2.0        # roughly how long one piece of fruit takes to cross the box

# --- miscellaneous ---
MIN_CONFIDENCE     = 50      # ignore detections below this; 0 disables the check
LED_ON_TIME        = 1.0     # seconds an LED stays lit after a detection
#  Duplicate suppression is edge triggered rather than a cooldown: the plate acts
#  once when fruit first enters the box, and re-arms only after it has left.
END_SESSION_TIMEOUT = 30.0   # silence for this long ends the run: final totals to
                             #   the LCD, all LEDs off

# --- debug aid for tuning ---
#   Prints the coordinates of every valid detection regardless of the gate, so the
#   trigger box can be measured by running fruit down the belt and watching how
#   the numbers move. Turn it off afterwards, or it floods the console.
#
DEBUG_COORDS = True

# ============================================================
# IPC send, calling IPC_Example.py as a subprocess
#   A lock serialises access, since concurrent writes to the device clash.
# ============================================================
IPC_EXAMPLE_PATH = "/home/topst/Fabless_Education_blink/Step_0/IPC_Example.py"
_ipc_lock = threading.Lock()

def send_ipc(can_id, data_hex):
    """Send a CAN message to the R5 core. Calls are serialised."""
    cmd = ["sudo", "python3", IPC_EXAMPLE_PATH,
           "snd", "--canID", f"0x{can_id:03X}", "--sndDataHex", data_hex]
    with _ipc_lock:
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
            if result.returncode != 0:
                print(f"  [ipc error] id=0x{can_id:03X} {result.stderr.strip()}")
        except subprocess.TimeoutExpired:
            print(f"  [ipc error] id=0x{can_id:03X} timed out")

def set_led(led_id, on):
    """Set one LED without blocking. The IPC call goes to its own thread so it
    cannot delay a servo command; the motor always takes priority."""
    run_async(send_ipc, CAN_ID_LED_CONTROL, f"{led_id:02X}{(1 if on else 0):02X}")

def all_led_off():
    send_ipc(CAN_ID_LED_CONTROL, "0000")   # id 0 turns everything off

# One colour per class, each with its own timer, so several can be lit at once.
CLASS_LED = {CLASS_ROTTEN: LED_RED, CLASS_UNRIPE: LED_YELLOW, CLASS_FRESH: LED_GREEN}
_led_timers = {}             # class_id → threading.Timer
_led_on = set()              # currently lit, so the same command is not resent every frame
_led_lock = threading.Lock()

def _led_off(class_id):
    led = CLASS_LED.get(class_id)
    with _led_lock:
        _led_timers.pop(class_id, None)
        was_on = led in _led_on
        _led_on.discard(led)
    if led is not None and was_on:
        set_led(led, False)

def led_pulse(class_id):
    """Light the colour for this class immediately and clear it after LED_ON_TIME.
    Continued detections refresh the timer without resending anything, and each
    class keeps its own timer."""
    led = CLASS_LED.get(class_id)
    if led is None:
        return
    with _led_lock:
        old = _led_timers.get(class_id)
        if old is not None:
            old.cancel()
        nt = threading.Timer(LED_ON_TIME, _led_off, args=(class_id,))
        nt.daemon = True
        _led_timers[class_id] = nt
        nt.start()
        need_on = led not in _led_on
        if need_on:
            _led_on.add(led)
    if need_on:
        set_led(led, True)   # only on the transition, not every frame

def all_leds_clear():
    """Turn every LED off and clear the timers."""
    with _led_lock:
        for t in _led_timers.values():
            t.cancel()
        _led_timers.clear()
        _led_on.clear()
    all_led_off()

# ============================================================
# running totals
# ============================================================
class FruitStats:
    def __init__(self):
        self.total = 0
        self.fresh = 0
        self.rotten = 0
        self.unripe = 0
        self._lock = threading.Lock()

    def record(self, class_id):
        with self._lock:
            self.total += 1
            if class_id == CLASS_FRESH:
                self.fresh += 1
            elif class_id == CLASS_ROTTEN:
                self.rotten += 1
            elif class_id == CLASS_UNRIPE:
                self.unripe += 1

    def lcd_hex(self):
        """Four bytes for the LCD: total, fresh, rotten, unripe."""
        with self._lock:
            return (f"{min(self.total,255):02X}"
                    f"{min(self.fresh,255):02X}"
                    f"{min(self.rotten,255):02X}"
                    f"{min(self.unripe,255):02X}")

    def line(self):
        with self._lock:
            t = self.total or 1
            return (f"total:{self.total} | fresh:{self.fresh}({self.fresh*100//t}%) | "
                    f"rotten:{self.rotten}({self.rotten*100//t}%) | "
                    f"unripe:{self.unripe}({self.unripe*100//t}%)")

stats = FruitStats()
_armed = {CLASS_FRESH: True, CLASS_ROTTEN: True, CLASS_UNRIPE: True}  # edge trigger, per class
_inwin = {CLASS_FRESH: 0, CLASS_ROTTEN: 0, CLASS_UNRIPE: 0}           # consecutive frames inside the box
_box_last = {CLASS_FRESH: 0.0, CLASS_ROTTEN: 0.0, CLASS_UNRIPE: 0.0}  # last seen, used to re-arm
_fresh_veto_until = 0.0  # rejects are blocked until this time
_last_detect  = 0.0      # timestamp of the last detection
_session_active = False  # so the end-of-run actions happen exactly once
_end_lock = threading.Lock()

def update_lcd():
    """Push the totals to the LCD. Only called when a run ends."""
    send_ipc(CAN_ID_LCD_STATS, stats.lcd_hex())

def end_watcher():
    """After END_SESSION_TIMEOUT seconds with no detection, treat the run as
    finished: show the totals on the LCD and turn the LEDs off."""
    global _session_active
    while True:
        time.sleep(1.0)
        with _end_lock:
            active = _session_active
            idle = time.time() - _last_detect
        if active and idle >= END_SESSION_TIMEOUT:
            print(f"\n===== run finished, {END_SESSION_TIMEOUT:.0f}s with no detection =====")
            print(f"  final: {stats.line()}")
            update_lcd()        # totals to the LCD
            all_leds_clear()
            with _end_lock:
                _session_active = False
            if ORCHESTRATE:                     # also stop the AI board and exit
                stop_aig()
                servo_angle(SERVO_DEFAULT_DEG)
                print("[exit] no detections for 30 s, shutting down")
                os._exit(0)

# ============================================================
# tilt plate servo
# ============================================================
def servo_angle(deg):
    """Move the plate to the given angle. Byte 0 of the frame is the angle."""
    deg = max(0, min(180, int(deg)))
    send_ipc(SERVO_CAN_ID, f"{deg:02X}")

_servo_lock = threading.Lock()   # one plate, so one movement at a time

def servo_sort(angle, name):
    """Tilt, hold, return to level.
    Triggers that arrive while the plate is moving are dropped rather than queued.
    A queue would make the plate act late, on the wrong piece of fruit."""
    if not _servo_lock.acquire(blocking=False):
        print(f"  -> [{name}] plate already moving, dropped")
        return
    try:
        print(f"  -> [{name}] {SERVO_DEFAULT_DEG} to {angle}, hold {SERVO_HOLD_TIME}s, back to {SERVO_DEFAULT_DEG}")
        servo_angle(angle)
        time.sleep(SERVO_HOLD_TIME)
        servo_angle(SERVO_DEFAULT_DEG)
    finally:
        _servo_lock.release()

def run_async(fn, *args):
    """Run in a thread so the hold time does not block the TCP receive loop."""
    threading.Thread(target=fn, args=args, daemon=True).start()

# ============================================================
# handle one detection
#   wire format: "CLASS:1,CONF:85,X:300,Y:400,W:0,H:0"
#   The plate acts when the coordinates fall inside the trigger box.
# ============================================================
def process_detection(msg):
    global _last_detect, _session_active, _fresh_veto_until
    try:
        parts = dict(item.split(':') for item in msg.split(','))
        class_id   = int(parts['CLASS'])
        confidence = int(parts.get('CONF', 0))
        cx         = float(parts.get('X', -1))             # centre x, pixels
        cy         = float(parts.get('Y', -1))             # centre y, along the belt
    except (ValueError, KeyError) as e:
        print(f"[error] could not parse: {msg} ({e})")
        return

    name = CLASS_NAMES.get(class_id)
    if name is None:
        return                                   # unknown class
    if MIN_CONFIDENCE > 0 and confidence < MIN_CONFIDENCE:
        return                                   # below the confidence floor

    # tuning aid: print every valid detection
    if DEBUG_COORDS:
        print(f"[coords] {name:>6s} conf={confidence:3d}%  "
              f"X={parts.get('X','?')}  Y={parts.get('Y','?')}")

    # mark the run as active and refresh the end-of-run timer
    now = time.time()
    with _end_lock:
        _last_detect = now
        _session_active = True

    # Box gate plus edge trigger. Fruit that enters the box counts once, however
    # many frames it is detected on. The class re-arms only after it has been
    # absent from the box for COUNT_GAP seconds, which means the fruit has left.
    # Both axes must be inside, because the belt runs diagonally.
    # Unripe uses the smaller box; it is over-detected and otherwise fires too early.
    if class_id == CLASS_UNRIPE:
        x_hi, y_hi = TRIGGER_X_HI_UNRIPE, TRIGGER_Y_HI_UNRIPE   # entry edges pulled in
    else:
        x_hi, y_hi = TRIGGER_X_HI, TRIGGER_Y_HI
    in_box = (TRIGGER_X_LO <= cx <= x_hi) and (TRIGGER_Y_LO <= cy <= y_hi)

    # Ghost rejection: count up inside the box, down outside. Only a sustained run counts.
    if in_box:
        _inwin[class_id] = min(_inwin[class_id] + 1, TRIGGER_CONFIRM + 3)
    else:
        _inwin[class_id] = max(_inwin[class_id] - 1, 0)

    # Re-arm once the class has been absent long enough that the fruit must have left.
    # Re-detections and jitter fall inside the gap and do not count again.
    if in_box:
        _box_last[class_id] = now
    elif now - _box_last.get(class_id, 0.0) >= COUNT_GAP:
        _armed[class_id] = True

    # A fresh detection inside the box blocks rejects for a while
    if FRESH_VETO and class_id == CLASS_FRESH and in_box:
        if now >= _fresh_veto_until:
            print(f"  [veto] fresh detected, servo blocked for {FRESH_VETO_TIME:.1f}s")
        _fresh_veto_until = now + FRESH_VETO_TIME

    # while the veto is active, treat this piece as fresh and let it pass
    if (FRESH_VETO and class_id in (CLASS_ROTTEN, CLASS_UNRIPE)
            and now < _fresh_veto_until):
        if in_box and _armed.get(class_id, True) and _inwin[class_id] >= TRIGGER_CONFIRM:
            _armed[class_id] = False
            print(f"  [veto] ignoring {name}, treating as fresh")
        return   # no servo, no count, no LED

    if in_box and _armed.get(class_id, True) and _inwin[class_id] >= TRIGGER_CONFIRM:
        _armed[class_id] = False                  # disarm until the fruit leaves
        # dispatch the servo before the LED, so nothing delays the motor
        if class_id == CLASS_ROTTEN:
            run_async(servo_sort, SERVO_LEFT_DEG, "rotten, left")
        elif class_id == CLASS_UNRIPE:
            run_async(servo_sort, SERVO_RIGHT_DEG, "unripe, right")
        stats.record(class_id)
        print(f"\n[detect] {name} ({confidence}% confidence, x={cx:.0f} y={cy:.0f})")
        if class_id == CLASS_FRESH:
            print("  -> fresh: plate stays level, passes through")
        print(f"  [totals] {stats.line()}")    # the LCD only updates at the end

    # LED last, and non-blocking, so it never delays the motor
    led_pulse(class_id)

# ============================================================
# main: TCP server
# ============================================================
def main():
    print("=" * 60)
    print("  strawberry sorting - controller (single servo, position triggered)")
    print(f"  angles: fresh {SERVO_DEFAULT_DEG} / rotten {SERVO_LEFT_DEG} / "
          f"unripe {SERVO_RIGHT_DEG}, hold {SERVO_HOLD_TIME}s")
    print(f"  trigger box: X {TRIGGER_X_LO}-{TRIGGER_X_HI} and Y {TRIGGER_Y_LO}-{TRIGGER_Y_HI}, {TRIGGER_CONFIRM} frames, hold {SERVO_HOLD_TIME}s")
    print(f"  unripe box: X {TRIGGER_X_LO}-{TRIGGER_X_HI_UNRIPE} and Y {TRIGGER_Y_LO}-{TRIGGER_Y_HI_UNRIPE}")
    if FRESH_VETO:
        print(f"  fresh veto on: one fresh detection blocks the servo for {FRESH_VETO_TIME:.1f}s")
    print("  the conveyor is externally controlled")
    print(f"  LEDs light on detection, LCD shows totals at the end after {END_SESSION_TIMEOUT:.0f}s of silence")
    if ORCHESTRATE:
        print(f"  orchestration on: the AI board at {AIG_IP} is driven over SSH")
    print("=" * 60)

    # pin the local ethernet address
    if SETUP_D3G_NET:
        setup_d3g_net()

    # start clean: LEDs off, plate level; the LCD keeps its ready message
    all_led_off()
    servo_angle(SERVO_DEFAULT_DEG)

    # watchdog thread that ends the run after a period of silence
    threading.Thread(target=end_watcher, daemon=True).start()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", AI_G_PORT))
    server.listen(1)
    print(f"\n[tcp] waiting for the AI board on port {AI_G_PORT}")

    # start recognition only once the socket is listening
    if ORCHESTRATE:
        time.sleep(0.5)
        start_aig()

    try:
        while True:
            client, addr = server.accept()
            print(f"[tcp] connected: {addr}")
            buffer = ""
            try:
                while True:
                    data = client.recv(1024).decode('utf-8')
                    if not data:
                        print("[tcp] disconnected")
                        break
                    buffer += data
                    while '\n' in buffer:
                        line, buffer = buffer.split('\n', 1)
                        if line.strip():
                            process_detection(line.strip())
            except Exception as e:
                print(f"[tcp] receive error: {e}")
            finally:
                client.close()
    except KeyboardInterrupt:
        print("\n[exit] shutting down")
    finally:
        if ORCHESTRATE:
            stop_aig()
        # return the plate to level and clear the LEDs
        servo_angle(SERVO_DEFAULT_DEG)
        all_led_off()
        update_lcd()          # totals on every exit path, including Ctrl-C
        print("\n===== final totals =====")
        print(stats.line())
        server.close()

if __name__ == "__main__":
    main()
