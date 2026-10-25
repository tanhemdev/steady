"""
neurosync_receiver.py — read NeuroSync sensors over Bluetooth (BLE) on a laptop.

Setup (once, needs internet — use a phone hotspot if the venue WiFi blocks pip):
    pip install bleak

Run:
    python neurosync_receiver.py                              # first sensor whose name starts with NeuroSync
    python neurosync_receiver.py NeuroSync-Left NeuroSync-Right   # two sensors at once

While it runs, type a command and press Enter (sent to every connected sensor):
    c = calibrate    m0 = arm mode    m1 = walk mode    s = start    x = stop
    o = clear fall alert ("I'm OK")    t45 = arm target 45 degrees    q = quit

Every line received is saved to  neurosync_log_<time>.csv  (time, sensor, message fields).
Put your own gait code in on_line() below — it gets every message already split into fields.

Windows: Bluetooth must be on (Settings > Bluetooth & devices). Don't pair the sensor in Windows
settings; this script finds and connects to it by itself.
Mac: the first run asks to let Terminal / VS Code use Bluetooth — click Allow.
Only one program can be connected to a sensor at a time, so close the Chrome page first.
"""
import asyncio
import csv
import sys
import time
from datetime import datetime

from bleak import BleakClient, BleakScanner

RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # commands to the sensor
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # data from the sensor
PREFIX = "NeuroSync"

FIELDS = {
    "L": ["rotation_dps", "tilt_deg", "arm_angle_deg"],
    "S": ["leg_swing_deg", "stride_ms", "session_time_ds"],
    "R": ["hit", "peak_deg", "reps", "attempts"],
    "F": ["fall_number", "session_time_ds"],
    "T": ["recording", "fall_alert", "session_time_ds", "fall_in_ds"],
    "M": ["mode_walk", "target_deg", "sensor_ok"],
    "E": ["duration_ds"],
}

clients = {}
log_file = open(f"neurosync_log_{datetime.now():%Y%m%d_%H%M%S}.csv", "w", newline="")
log = csv.writer(log_file)
log.writerow(["time_s", "sensor", "type", "f1", "f2", "f3", "f4"])
t0 = time.time()
last_print = {}


def on_line(sensor, kind, values):
    """Called for every message. Replace / extend with your own gait code.

    kind is one letter (see FIELDS above); values is a dict of numbers, e.g.
      kind "S" -> {"leg_swing_deg": 74, "stride_ms": 1100, "session_time_ds": 52}
    """
    if kind == "S":
        st = values["stride_ms"] / 1000
        print(f"[{sensor}] stride: leg swing {values['leg_swing_deg']:.0f} deg"
              + (f", stride time {st:.2f} s, cadence {120 / st:.0f} steps/min" if st > 0 else ""))
    elif kind == "R":
        print(f"[{sensor}] arm raise: {values['peak_deg']:.0f} deg — "
              f"{'REACHED target' if values['hit'] else 'missed'} ({values['reps']:.0f}/{values['attempts']:.0f})")
    elif kind == "F":
        print(f"\n!!! [{sensor}] POSSIBLE FALL #{values['fall_number']:.0f} — check on the patient (send 'o' to clear)\n")
    elif kind == "L":
        # Live data, 20x per second. Printed once a second so the screen stays readable.
        if time.time() - last_print.get(sensor, 0) > 1:
            last_print[sensor] = time.time()
            print(f"[{sensor}] live: rotation {values['rotation_dps']:.0f} deg/s, "
                  f"leg tilt {values['tilt_deg']:.0f} deg, arm angle {values['arm_angle_deg']:.0f} deg")
    elif kind == "T" and values["fall_in_ds"] >= 0:
        print(f"[{sensor}] leg tilted over — fall alert in {values['fall_in_ds'] / 10:.1f} s")


def handle(sensor, raw):
    line = raw.decode(errors="ignore").strip()
    parts = line.split(",")
    kind, rest = parts[0], parts[1:]
    log.writerow([f"{time.time() - t0:.3f}", sensor, kind] + rest)
    if kind == "C":
        print(f"[{sensor}] calibrated")
    elif kind == "B":
        print(f"[{sensor}] session started")
    elif kind == "E":
        print(f"[{sensor}] session stopped ({int(rest[0]) / 10:.1f} s)")
    elif kind == "!":
        print(f"[{sensor}] lost contact with the motion sensor — check the wires")
    if kind in FIELDS:
        try:
            values = dict(zip(FIELDS[kind], map(float, rest)))
        except ValueError:
            return
        on_line(sensor, kind, values)


async def find(name):
    print(f"Looking for {name}…")
    if name == PREFIX:
        return await BleakScanner.find_device_by_filter(
            lambda d, adv: (d.name or adv.local_name or "").startswith(PREFIX), timeout=15)
    return await BleakScanner.find_device_by_name(name, timeout=15)


async def run_sensor(name):
    while True:
        dev = await find(name)
        if dev is None:
            print(f"Couldn't find {name}. Is it powered on, and is the Chrome page closed? Retrying…")
            continue
        label = dev.name or name
        try:
            async with BleakClient(dev) as client:
                clients[label] = client
                await client.start_notify(TX, lambda _, data: handle(label, data))
                print(f"Connected to {label} ✓  (type c to calibrate, m1 for walk mode, s to start)")
                while client.is_connected:
                    await asyncio.sleep(0.5)
        except Exception as e:
            print(f"[{label}] connection problem: {e}")
        clients.pop(label, None)
        print(f"[{label}] disconnected — reconnecting…")
        await asyncio.sleep(1)


async def keyboard():
    loop = asyncio.get_running_loop()
    while True:
        cmd = (await loop.run_in_executor(None, sys.stdin.readline)).strip()
        if cmd == "q":
            log_file.close()
            print("Saved log:", log_file.name)
            raise SystemExit
        if not cmd:
            continue
        for label, client in list(clients.items()):
            try:
                await client.write_gatt_char(RX, cmd.encode(), response=True)
            except Exception as e:
                print(f"[{label}] couldn't send '{cmd}': {e}")


async def main():
    names = sys.argv[1:] or [PREFIX]
    await asyncio.gather(keyboard(), *(run_sensor(n) for n in names))


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except (KeyboardInterrupt, SystemExit):
        log_file.close()
