# Sensor protocol

From the header of `firmware/neurosync_ble/neurosync_ble.ino`.

BLE: Nordic UART service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`; notifications on TX `6e400003-…`, commands written to RX `6e400002-…`. Over USB the same lines are printed at 115200 baud (send commands followed by a newline).

```text
neurosync_ble.ino — Bluetooth (BLE) rehab tracker, ESP32-S3 + MPU-6050 / 6500 / 9250 / 9255 (Cal Hacks 13.0)

The board advertises over Bluetooth Low Energy as  NeuroSync-Ankle  (change DEVICE_NAME below,
e.g. NeuroSync-Left / NeuroSync-Right when using two sensors).
Connect with  neurosync_ble.html  in Chrome, or  neurosync_receiver.py  (Python + bleak).

Two modes:
  ARM  — sensor on the forearm. Logs every raise (height reached); raises that hit the target count as reps.
  WALK — sensor on the shin / ankle. Start, walk, Stop. Sends each stride (leg swing + stride time)
         and flags a possible fall when the leg stays tilted far over for a few seconds.
         Calibrate while STANDING so upright = 0 degrees.

Every BLE message is one short text line (20 bytes or less), comma-separated:
  L,rot,tilt,angle      live, 20x/sec: leg rotation speed (deg/s), leg tilt (deg), arm angle (deg)
  S,swing,strideMs,t    a stride finished: leg swing (deg), time since last stride (ms, 0 = first), session time (0.1 s)
  R,hit,peak,reps,att   an arm raise finished: 1 = reached target, peak (deg), reps so far, attempts so far
  F,count,t             possible fall: fall number, session time (0.1 s, -1 if not recording)
  T,rec,fall,t,fallIn   status 4x/sec: recording 0/1, fall alert showing 0/1, session time (0.1 s),
                        seconds-to-fall-alert in 0.1 s (-1 = leg not tilted over)
  M,mode,target,ok      once a second: 0 = arm / 1 = walk, arm target (deg), sensor working 0/1
  C  calibrated     B  session started     E,dur  session stopped (duration in 0.1 s)     !,lost  sensor contact lost

Commands written to the RX characteristic (or typed into the Serial Monitor):
  c = calibrate   m0 = arm mode   m1 = walk mode   s = start   x = stop   o = "I'm OK" (clear fall alert)   t45 = arm target 45 deg

Same lines are also printed over USB at 115200, so you can watch them in the Serial Monitor.
Wiring: SDA -> GPIO 8, SCL -> GPIO 9, VCC -> 3V3, GND -> GND (NCS -> 3V3, AD0 -> GND on 9250/6500 boards).
---------- Bluetooth ----------
---------- Wiring ----------
---------- Arm mode settings ----------
---------- Walk mode settings ----------
---------- Fall detection (Walk mode) ----------
Bluetooth objects
Sensor
Arm state
Walk state
Session
Falls
---------------- sending ----------------
One short text line out over Bluetooth (max 20 bytes) and USB.
---------------- Bluetooth callbacks ----------------
---------------- sensor ----------------
accel in g, gyro in deg/s (offsets removed)
Hold still: measures gravity direction (= 0 degrees) and gyro offsets.
---------------- ARM mode ----------------
---------------- WALK mode ----------------
Tenths of a second left before a fall alert (or -1 if the leg isn't tilted over)
---------------- commands ----------------
---------------- setup / loop ----------------
```
