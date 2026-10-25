# Steady

**Post-stroke recovery, at home.** A phone app plus a low-cost wearable motion sensor that helps stroke survivors practise speech and memory, walk more evenly, and get help fast if they fall, with one shared record for their care team.

Built at **Cal Hacks 13.0** (October 23–25, 2026, Palace of Fine Arts, San Francisco).

**Try it on your phone:** https://mana.space/a/steady  ·  **Demo video:** [`media/demo-tour.mp4`](media/demo-tour.mp4)  ·  **Pitch deck:** [`deck/STEADY-pitch-deck.pdf`](deck/STEADY-pitch-deck.pdf)

| Speech | Memory | Movement | Fall check-in | Care report |
|---|---|---|---|---|
| ![](media/screenshots/speech.jpg) | ![](media/screenshots/memory.jpg) | ![](media/screenshots/movement.jpg) | ![](media/screenshots/fall-check-in.jpg) | ![](media/screenshots/care-report.jpg) |

## The problem

1 in 4 adults will have a stroke in their lifetime, and in the U.S. someone has a stroke every 40 seconds (more than 795,000 a year, CDC). After discharge most recovery happens at home, between short weekly visits:

- **Speech:** about 1 in 3 survivors has aphasia. Therapy is about an hour a week; at home it is worksheets with no feedback.
- **Memory:** up to 60% have memory or thinking problems in the first year. Generic brain games drill strangers' faces, not family.
- **Movement:** nearly half of survivors fall in their first year home. A fall button only works if you can press it.
- **Care:** physio, speech therapist and neurologist each keep separate notes, and nearly 1 in 4 strokes is a repeat stroke.

## What Steady does

1. **Speech:** a picture-naming coach. You see a picture, tap, and say the word. Your attempt is transcribed (ElevenLabs Scribe) and Steady tells you one thing: correct, or not quite (with what it heard). A calm voice coach (ElevenLabs TTS) can say the word for you first.
2. **Memory:** two short games built from your own life: name the people in your family photos, and recall everyday words in order. A name you struggle with becomes tomorrow's speech practice.
3. **Movement:** an ankle sensor (ESP32-S3 + MPU-6050) streams every stride over Bluetooth. Steady counts steps, cadence, leg swing, stride-time variation, and left/right symmetry when you wear one on each leg. Arm-raise mode tracks shoulder range.
4. **Fall check-in:** if the leg stays tipped over, Steady asks first: you get 30 seconds to tap “I’m OK” before your emergency contact is called with your Medical ID.
5. **Care hub:** your contacts and care team in one place, and one tap sends your doctor a **PDF summary plus the raw data as CSV** (every stride and arm raise) through the phone's share sheet.

Steady measures, prompts and routes. A licensed clinician makes every clinical decision. Not affiliated with Kaiser Permanente.

## Repository

```
app/index.html                 The Steady phone app (single file: HTML, CSS, JS). Opens in any browser.
app/mana-bridge.jsx            The Mana component that hosts the app and bridges it to the cloud:
                               live sensor relay to other phones, saved sessions, voice (TTS) and speech-to-text.
firmware/neurosync_ble/        ESP32-S3 firmware (Arduino): MPU-6050 over I2C, BLE Nordic UART service.
tools/neurosync_ble.html       Stand-alone Web Bluetooth console for the sensor.
tools/neurosync_receiver.py    Laptop receiver (Python + bleak) for one or two sensors.
docs/ble-protocol.md           The sensor's message and command format.
media/                         Demo videos and screenshots.  deck/  Pitch deck.
```

## Run it

**Phone (easiest):** open https://mana.space/a/steady. The first screen asks for the microphone and tests speech and the voice coach.

**Locally:** open `app/index.html` in Chrome (or serve the folder: `python3 -m http.server` then visit `http://localhost:8000/app/`). Running on its own, the app uses the browser's built-in speech recognition and voice instead of the cloud ones, and keeps sessions in local storage.

**Sensor:**
1. Wire an MPU-6050 to an ESP32-S3: SDA → GPIO 8, SCL → GPIO 9, VCC → 3V3, GND → GND.
2. Flash `firmware/neurosync_ble/neurosync_ble.ino` with the Arduino IDE (board: ESP32S3 Dev Module). It advertises as `NeuroSync-Ankle` (use `NeuroSync-Left` / `NeuroSync-Right` for two sensors).
3. In Steady, open **Movement → Connect by Bluetooth** (Chrome on Android or a laptop; iPhone browsers do not support Web Bluetooth, so iPhones show the live stream relayed from the connected device). USB also works: **Connect by USB** at 115200 baud.
4. Stand still → **Calibrate** → **Start walk**. Strap it to the outside of the weaker leg, just above the ankle.

Only one program can connect to the sensor at a time: close `tools/neurosync_ble.html` and `neurosync_receiver.py` first.

## How it's built

- **App:** vanilla HTML/CSS/JS, phone-first, accessible (bigger text, stronger contrast, read-aloud).
- **Hardware:** ESP32-S3 + MPU-6050 6-axis IMU; on-device stride detection, arm-raise peaks and tilt-based fall detection; BLE notifications ≤ 20 bytes.
- **Web Bluetooth / Web Serial** to talk to the sensor from the browser.
- **Mana** (mana.space) hosts the app, relays the live sensor stream to every phone in real time (Firebase Realtime Database), stores finished sessions, and proxies ElevenLabs so no API key reaches the browser.
- **ElevenLabs:** Scribe v2 for speech-to-text, `eleven_v4_turbo` for the calm voice coach (clips cached so repeats play instantly).
- **Reports:** jsPDF for the PDF; a tidy CSV (`date,time,category,measure,value,unit,session,note`); Web Share API to send both from the phone.

## Data and safety

Demo patient data is synthetic. Speech audio is only transcribed to check the word and is not saved. Sensor sessions are stored in the app owner's workspace. Fall alerts always ask the user first. This is a hackathon prototype, not a medical device.

## Team

Jack Langhoff · Maya Van · Miriya Mayenkar · Tanya Hemdev

## Acknowledgements

Built during Cal Hacks 13.0 with help from AI coding assistants (Claude, Mana). Statistics: World Stroke Organization; CDC Stroke Facts (2024); Flowers et al., *Arch Phys Med Rehabil* 2016; AHA/ASA Scientific Statement on post-stroke cognitive impairment (2023); Walsh et al., FREESE cohort (2018).

## License

MIT
