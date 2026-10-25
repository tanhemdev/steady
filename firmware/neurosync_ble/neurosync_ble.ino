// neurosync_ble.ino — Bluetooth (BLE) rehab tracker, ESP32-S3 + MPU-6050 / 6500 / 9250 / 9255 (Cal Hacks 13.0)
//
// The board advertises over Bluetooth Low Energy as  NeuroSync-Ankle  (change DEVICE_NAME below,
// e.g. NeuroSync-Left / NeuroSync-Right when using two sensors).
// Connect with  neurosync_ble.html  in Chrome, or  neurosync_receiver.py  (Python + bleak).
//
// Two modes:
//   ARM  — sensor on the forearm. Logs every raise (height reached); raises that hit the target count as reps.
//   WALK — sensor on the shin / ankle. Start, walk, Stop. Sends each stride (leg swing + stride time)
//          and flags a possible fall when the leg stays tilted far over for a few seconds.
//          Calibrate while STANDING so upright = 0 degrees.
//
// Every BLE message is one short text line (20 bytes or less), comma-separated:
//   L,rot,tilt,angle      live, 20x/sec: leg rotation speed (deg/s), leg tilt (deg), arm angle (deg)
//   S,swing,strideMs,t    a stride finished: leg swing (deg), time since last stride (ms, 0 = first), session time (0.1 s)
//   R,hit,peak,reps,att   an arm raise finished: 1 = reached target, peak (deg), reps so far, attempts so far
//   F,count,t             possible fall: fall number, session time (0.1 s, -1 if not recording)
//   T,rec,fall,t,fallIn   status 4x/sec: recording 0/1, fall alert showing 0/1, session time (0.1 s),
//                         seconds-to-fall-alert in 0.1 s (-1 = leg not tilted over)
//   M,mode,target,ok      once a second: 0 = arm / 1 = walk, arm target (deg), sensor working 0/1
//   C  calibrated     B  session started     E,dur  session stopped (duration in 0.1 s)     !,lost  sensor contact lost
//
// Commands written to the RX characteristic (or typed into the Serial Monitor):
//   c = calibrate   m0 = arm mode   m1 = walk mode   s = start   x = stop   o = "I'm OK" (clear fall alert)   t45 = arm target 45 deg
//
// Same lines are also printed over USB at 115200, so you can watch them in the Serial Monitor.
// Wiring: SDA -> GPIO 8, SCL -> GPIO 9, VCC -> 3V3, GND -> GND (NCS -> 3V3, AD0 -> GND on 9250/6500 boards).

#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <math.h>
#include <stdarg.h>

// ---------- Bluetooth ----------
const char* DEVICE_NAME = "NeuroSync-Ankle";
#define SVC_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define RX_UUID  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   // commands in
#define TX_UUID  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // data out (notify)

// ---------- Wiring ----------
const int SDA_PIN = 8;
const int SCL_PIN = 9;

// ---------- Arm mode settings ----------
float       targetDeg   = 60.0;  // a raise must reach this to count as a rep
const float RISE_DEG    = 15.0;  // a raise starts when the arm lifts this far above its lowest point
const float RETURN_FRAC = 0.5;   // a raise ends once the arm comes back down halfway
const float SMOOTHING   = 0.08;  // angle smoothing at 100 readings/sec

// ---------- Walk mode settings ----------
const float SWING_ON_DPS   = 120.0;  // leg rotating faster than this = leg is swinging
const float SWING_OFF_DPS  = 60.0;   // drops below this = foot planted again
const unsigned long MIN_STRIDE_MS = 400;
const unsigned long MAX_STRIDE_MS = 2500;

// ---------- Fall detection (Walk mode) ----------
const float FALL_TILT_DEG  = 60.0;   // leg tilted past this from standing starts the fall timer
const float FALL_KEEP_DEG  = 50.0;   // timer keeps running while tilt stays above this
const unsigned long FALL_HOLD_MS        = 3000;  // tilted this long = possible fall
const unsigned long FALL_HOLD_IMPACT_MS = 1500;  // ...or this long if a hard jolt happened just before
const float IMPACT_G       = 3.0;    // jolt bigger than this (in g) counts as an impact
const float REARM_DEG      = 30.0;   // after an alert, leg must come back below this before another alert

const unsigned long SAMPLE_MS = 10;  // read the sensor 100 times a second
const unsigned long LIVE_MS   = 50;  // send a live line 20 times a second

enum RunMode { MODE_ARM, MODE_WALK };
RunMode mode = MODE_ARM;

// Bluetooth objects
BLECharacteristic* txChar = nullptr;
volatile bool bleConnected = false;
bool wasConnected = false;
char cmdBuf[20];
volatile bool cmdReady = false;

// Sensor
uint8_t imuAddr = 0;
bool sensorOk = false;
float restX = 0, restY = 0, restZ = 1;   // gravity direction when calibrated
float gbx = 0, gby = 0, gbz = 0;         // gyro offsets (deg/s)

// Arm state
float angleSmooth = 0;
bool  armUp = false;
int   reps = 0, attempts = 0;
float repPeak = 0, valley = 0, baseAngle = 0;

// Walk state
float gmagSmooth = 0;
bool  inSwing = false;
unsigned long lastSwingStart = 0;
float axisAvg[3] = {0, 0, 0};
float swingAng = 0, swingMin = 0, swingMax = 0;
unsigned long pendingStrideMs = 0;

// Session
bool recording = false;
unsigned long sessionStart = 0;

// Falls
int fallCount = 0;
unsigned long impactMs = 0, tiltSinceMs = 0;
float tiltSmooth = 0;
bool fallActive = false, fallArmed = true;

unsigned long lastSample = 0, lastLive = 0, lastStatus = 0, lastModeMsg = 0, lastRetry = 0;

// ---------------- sending ----------------
// One short text line out over Bluetooth (max 20 bytes) and USB.
void sendLine(const char* fmt, ...) {
  char buf[24];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > 20) { n = 20; buf[20] = 0; }
  Serial.println(buf);
  if (bleConnected && txChar) {
    txChar->setValue((uint8_t*)buf, n);
    txChar->notify();
  }
}

long sessionDs(unsigned long t) {   // session time in tenths of a second
  return recording ? (long)((t - sessionStart) / 100) : -1;
}

// ---------------- Bluetooth callbacks ----------------
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) { bleConnected = true; }
  void onDisconnect(BLEServer* s) {
    bleConnected = false;
    BLEDevice::startAdvertising();   // let the page / script reconnect
  }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    auto v = c->getValue();
    size_t n = v.length();
    if (n > sizeof(cmdBuf) - 1) n = sizeof(cmdBuf) - 1;
    for (size_t i = 0; i < n; i++) cmdBuf[i] = v[i];
    cmdBuf[n] = 0;
    cmdReady = true;                 // handled in loop(), not here
  }
};

void setupBle() {
  BLEDevice::init(DEVICE_NAME);
  BLEServer* srv = BLEDevice::createServer();
  srv->setCallbacks(new ServerCB());
  BLEService* svc = srv->createService(SVC_UUID);
  txChar = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());
  BLECharacteristic* rx = svc->createCharacteristic(
      RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// ---------------- sensor ----------------
void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(imuAddr, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool deviceAt(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool findSensor() {
  if (deviceAt(0x68)) { imuAddr = 0x68; return true; }
  if (deviceAt(0x69)) { imuAddr = 0x69; return true; }
  return false;
}

void initSensor() {
  writeReg(0x6B, 0x80);  // reset
  delay(100);
  writeReg(0x6B, 0x01);  // wake up
  delay(50);
  writeReg(0x1C, 0x10);  // accelerometer +/-8 g
  writeReg(0x1B, 0x08);  // gyroscope +/-500 deg/s
  writeReg(0x1A, 0x03);  // low-pass filter ~42 Hz
}

// accel in g, gyro in deg/s (offsets removed)
bool readImu(float& ax, float& ay, float& az, float& gx, float& gy, float& gz) {
  uint8_t b[14];
  if (!readRegs(0x3B, b, 14)) return false;
  int16_t v[7];
  for (int i = 0; i < 7; i++) v[i] = (int16_t)((b[2 * i] << 8) | b[2 * i + 1]);
  ax = v[0] / 4096.0; ay = v[1] / 4096.0; az = v[2] / 4096.0;
  gx = v[4] / 65.5 - gbx; gy = v[5] / 65.5 - gby; gz = v[6] / 65.5 - gbz;
  return true;
}

void resetSession() {
  angleSmooth = 0; armUp = false; reps = 0; attempts = 0;
  repPeak = 0; valley = 0; baseAngle = 0;
  gmagSmooth = 0; inSwing = false; lastSwingStart = 0;
  swingAng = 0; swingMin = 0; swingMax = 0; pendingStrideMs = 0;
  for (int i = 0; i < 3; i++) axisAvg[i] = 0;
  fallCount = 0; fallActive = false; fallArmed = true; tiltSinceMs = 0; impactMs = 0; tiltSmooth = 0;
}

// Hold still: measures gravity direction (= 0 degrees) and gyro offsets.
void calibrate() {
  float sx = 0, sy = 0, sz = 0, sgx = 0, sgy = 0, sgz = 0;
  int n = 0;
  float savedX = gbx, savedY = gby, savedZ = gbz;
  gbx = gby = gbz = 0;
  for (int i = 0; i < 60; i++) {
    float ax, ay, az, gx, gy, gz;
    if (readImu(ax, ay, az, gx, gy, gz)) {
      sx += ax; sy += ay; sz += az; sgx += gx; sgy += gy; sgz += gz; n++;
    }
    delay(10);
  }
  if (n > 0) {
    restX = sx / n; restY = sy / n; restZ = sz / n;
    gbx = sgx / n; gby = sgy / n; gbz = sgz / n;
  } else {
    gbx = savedX; gby = savedY; gbz = savedZ;
  }
  resetSession();
  recording = false;
  sendLine("C");
}

float liftAngle(float x, float y, float z) {
  float dot = x * restX + y * restY + z * restZ;
  float mag = sqrt(x * x + y * y + z * z) * sqrt(restX * restX + restY * restY + restZ * restZ);
  if (mag < 0.001) return 0;
  float c = constrain(dot / mag, -1.0, 1.0);
  return acos(c) * 180.0 / PI;
}

// ---------------- ARM mode ----------------
void updateArm(float ax, float ay, float az) {
  float raw = liftAngle(ax, ay, az);
  angleSmooth = SMOOTHING * raw + (1 - SMOOTHING) * angleSmooth;

  if (!armUp) {
    if (angleSmooth < valley) valley = angleSmooth;
    if (angleSmooth > valley + RISE_DEG) {
      armUp = true;
      repPeak = angleSmooth;
      baseAngle = valley;
    }
  } else {
    if (angleSmooth > repPeak) repPeak = angleSmooth;
    float endAt = baseAngle + (repPeak - baseAngle) * RETURN_FRAC;
    if (angleSmooth < endAt) {
      armUp = false;
      valley = angleSmooth;
      bool hit = repPeak >= targetDeg;
      attempts++;
      if (hit) reps++;
      sendLine("R,%d,%d,%d,%d", hit ? 1 : 0, (int)lround(repPeak), reps, attempts);
    }
  }
}

// ---------------- WALK mode ----------------
void updateFall(float ax, float ay, float az, unsigned long now) {
  float amag = sqrt(ax * ax + ay * ay + az * az);
  if (amag > IMPACT_G) impactMs = now;

  // Smoothed leg tilt from standing (ignores the quick swings of normal walking)
  tiltSmooth = 0.03 * liftAngle(ax, ay, az) + 0.97 * tiltSmooth;

  if (!fallArmed) {                       // wait for the leg to come back up before alerting again
    if (tiltSmooth < REARM_DEG) fallArmed = true;
    tiltSinceMs = 0;
    return;
  }
  if (tiltSinceMs == 0) {
    if (tiltSmooth > FALL_TILT_DEG) tiltSinceMs = now;
    return;
  }
  if (tiltSmooth < FALL_KEEP_DEG) { tiltSinceMs = 0; return; }   // leg came back up: not a fall

  bool jolted = impactMs != 0 && impactMs + 3000 > tiltSinceMs;  // hard jolt within 3 s before tipping over
  unsigned long need = jolted ? FALL_HOLD_IMPACT_MS : FALL_HOLD_MS;
  if (now - tiltSinceMs >= need) {
    fallActive = true;
    fallArmed = false;
    fallCount++;
    sendLine("F,%d,%ld", fallCount, sessionDs(tiltSinceMs));
    tiltSinceMs = 0;
  }
}

// Tenths of a second left before a fall alert (or -1 if the leg isn't tilted over)
long fallCountdownDs(unsigned long now) {
  if (!fallArmed || tiltSinceMs == 0) return -1;
  bool jolted = impactMs != 0 && impactMs + 3000 > tiltSinceMs;
  unsigned long need = jolted ? FALL_HOLD_IMPACT_MS : FALL_HOLD_MS;
  unsigned long held = now - tiltSinceMs;
  return held >= need ? 0 : (long)((need - held) / 100);
}

void updateWalk(float ax, float ay, float az, float gx, float gy, float gz, float dt, unsigned long now) {
  float g[3] = {gx, gy, gz};
  float gmag = sqrt(gx * gx + gy * gy + gz * gz);
  gmagSmooth = 0.3 * gmag + 0.7 * gmagSmooth;

  updateFall(ax, ay, az, now);

  // The axis the leg swings around is the one that rotates most while walking.
  for (int i = 0; i < 3; i++) axisAvg[i] = 0.995 * axisAvg[i] + 0.005 * fabs(g[i]);
  int dom = 0;
  if (axisAvg[1] > axisAvg[dom]) dom = 1;
  if (axisAvg[2] > axisAvg[dom]) dom = 2;
  swingAng += g[dom] * dt;

  if (!recording) return;   // strides only count between Start and Stop

  if (!inSwing) {
    if (gmagSmooth > SWING_ON_DPS && (lastSwingStart == 0 || now - lastSwingStart > MIN_STRIDE_MS)) {
      inSwing = true;
      pendingStrideMs = (lastSwingStart > 0 && now - lastSwingStart < MAX_STRIDE_MS) ? now - lastSwingStart : 0;
      lastSwingStart = now;
      swingAng = 0; swingMin = 0; swingMax = 0;
    }
  } else {
    if (swingAng < swingMin) swingMin = swingAng;
    if (swingAng > swingMax) swingMax = swingAng;
    if (gmagSmooth < SWING_OFF_DPS) {
      inSwing = false;
      float swing = swingMax - swingMin;
      sendLine("S,%d,%lu,%ld", (int)lround(swing), pendingStrideMs, sessionDs(now));
    }
  }
}

// ---------------- commands ----------------
void handleCommand(const char* c) {
  switch (c[0]) {
    case 'c': case 'C':
      calibrate();
      break;
    case 'm': case 'M':
      mode = (c[1] == '1') ? MODE_WALK : MODE_ARM;
      resetSession();
      recording = false;
      lastModeMsg = 0;                      // send the new mode right away
      break;
    case 's': case 'S':
      resetSession();
      recording = true;
      sessionStart = millis();
      sendLine("B");
      break;
    case 'x': case 'X':
      if (recording) {
        recording = false;
        sendLine("E,%lu", (millis() - sessionStart) / 100);
      }
      break;
    case 'o': case 'O':
      fallActive = false;
      break;
    case 't': case 'T': {
      float t = atof(c + 1);
      if (t >= 10 && t <= 170) { targetDeg = t; lastModeMsg = 0; }
      break;
    }
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);   // don't stall when running on a power bank with no laptop listening
#endif
  delay(1000);

  setupBle();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  unsigned long t0 = millis();
  while (!findSensor() && millis() - t0 < 5000) {
    Serial.println("!,nosensor (check wires: SDA 8, SCL 9)");
    delay(500);
  }
  if (imuAddr) {
    initSensor();
    sensorOk = true;
    delay(300);
    calibrate();
  }
}

void loop() {
  unsigned long now = millis();

  // Commands from Bluetooth
  if (cmdReady) {
    char local[sizeof(cmdBuf)];
    memcpy(local, (const void*)cmdBuf, sizeof(cmdBuf));
    cmdReady = false;
    handleCommand(local);
  }

  // Commands typed in the Serial Monitor (one per line)
  static char line[20];
  static int lineLen = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (lineLen > 0) { line[lineLen] = 0; handleCommand(line); lineLen = 0; }
    } else if (lineLen < (int)sizeof(line) - 1) {
      line[lineLen++] = ch;
    }
  }

  // Just connected: send the current mode straight away
  if (bleConnected && !wasConnected) lastModeMsg = 0;
  wasConnected = bleConnected;

  if (now - lastSample < SAMPLE_MS) return;
  float dt = (now - lastSample) / 1000.0;
  if (dt > 0.1) dt = 0.1;
  lastSample = now;

  float ax, ay, az, gx, gy, gz;
  if (!imuAddr || !readImu(ax, ay, az, gx, gy, gz)) {
    // Sensor missing or contact dropped: keep Bluetooth running and try to reconnect once a second.
    sensorOk = false;
    if (now - lastRetry > 1000) {
      lastRetry = now;
      sendLine("!,lost");
      Wire.end();
      Wire.begin(SDA_PIN, SCL_PIN);
      Wire.setClock(100000);
      if (findSensor()) { initSensor(); sensorOk = true; }
    }
  } else {
    sensorOk = true;
    if (mode == MODE_ARM) updateArm(ax, ay, az);
    else updateWalk(ax, ay, az, gx, gy, gz, dt, now);
  }

  if (now - lastLive >= LIVE_MS) {
    lastLive = now;
    sendLine("L,%d,%d,%d", (int)lround(gmagSmooth), (int)lround(tiltSmooth), (int)lround(angleSmooth));
  }
  if (now - lastStatus >= 250) {
    lastStatus = now;
    sendLine("T,%d,%d,%ld,%ld", recording ? 1 : 0, fallActive ? 1 : 0,
             recording ? (long)((now - sessionStart) / 100) : 0L, fallCountdownDs(now));
  }
  if (lastModeMsg == 0 || now - lastModeMsg >= 1000) {
    lastModeMsg = now;
    sendLine("M,%d,%d,%d", mode == MODE_WALK ? 1 : 0, (int)lround(targetDeg), sensorOk ? 1 : 0);
  }
}
