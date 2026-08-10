# Strawberry Sorting System

Strawberries move down a conveyor. A camera and an NPU classify each one as fresh,
unripe or rotten, and a tilt plate at the end of the belt drops it to the correct
side. Three embedded boards with different chips, different operating systems and
different languages are chained into one line.

Advanced Project, Chung-Ang University, in cooperation with the Telechips fabless
training programme. Two-person team, March to June 2026.

[한국어](README.ko.md)

---

## Highlights

| | |
|---|---|
| Boards | Three — 8 TOPS NPU (Yocto), dual-core controller (Linux + FreeRTOS), MCU actuator |
| Chain | Ethernet TCP → IPC shared memory → CAN bus |
| Reliability | **Exactly one actuation per piece of fruit** at 2 fps with frequent misdetection |
| Operation | The whole line starts and stops from a single SSH command — no UART cables |
| My part | Control and actuation boards, communication chain, system integration |

## The chain

```
 camera --> [ AI-G ]  --TCP over ethernet-->  [ D3-G A72 ]
            YOLOv8n                            classify, decide when to act
            on the NPU                                 |
                                                       | IPC, shared memory
                                                       v
                                              [ D3-G R5, FreeRTOS ]
                                                       |
                                                       | CAN bus
                                                       v
                                              [ VCP-G ]  servo, LEDs, LCD
```

| Board | Job | What it runs |
|---|---|---|
| AI-G | recognition | camera plus an 8 TOPS NPU, Yocto Linux, YOLOv8n |
| D3-G | control | A72 under Linux decides; the R5 under FreeRTOS bridges shared memory to CAN |
| VCP-G | actuation | TCC70xx MCU driving the servo, three LEDs and an LCD |

Every link uses a different mechanism, and getting them to mesh was most of the work.
The AI board has no Python at all — it is a minimal Yocto image — so the result
sender is written in `sh` with `awk` and `nc`. Between the two cores of the D3-G the
path is `/dev/tcc_ipc_micom`. From there to the actuator board it is CAN, with one
byte of angle for the servo, two bytes for the LEDs and four for the LCD counters.

## One action per strawberry

The camera delivers about two frames per second and the model misfires often enough
to matter. Acting directly on a detection means the plate fires several times for one
piece of fruit, or fires at a coordinate that jumped for a single frame. Five stages
sit between a detection and the servo:

| Stage | Rule |
|---|---|
| confidence | ignore anything below 50% |
| trigger box | both X and Y have to fall inside the plate's position |
| debounce | three consecutive frames inside the box |
| re-arm gap | the class has to be absent for 1.5 s before the next piece counts |
| non-blocking servo | a trigger arriving mid-movement is dropped, not queued |

The last one came out of a failure. With one plate, servo calls were serialised behind
a lock — and over-triggering built a backlog of 1.3-second movements, so the plate
kept turning over an empty belt long after the fruit had gone. Making the lock
non-blocking removed the backlog rather than draining it.

## Three kinds of misdetection

The model validates at mAP50 around 0.99 and still misbehaves on a real belt. Each
failure needed a different correction.

| Problem | Cause | Fix |
|---|---|---|
| triggers early and too easily | the decision used only the Y coordinate | a 2D box on X and Y, so only the spot in front of the plate counts |
| unripe over-detected | it has the most training data and is found on nearly every frame, so it triggers the moment it enters the box | a smaller box for unripe alone, with the entry edges pulled inward |
| fresh fruit rejected | fresh is occasionally read as rotten or unripe | a single fresh detection blocks the other classes for two seconds |

The last fix rests on an observation about the *direction* of the errors. Fresh being
called defective is common; defective being called fresh is rare. So a fresh label is
the trustworthy one, and the logic is deliberately asymmetric: fresh acts on one
frame while rotten and unripe need three, which means fresh always wins the race.

## Running it without cables

Originally each board needed its own UART cable and its own typed commands. The
controller now starts recognition on the AI board over SSH and shuts everything down
after thirty seconds with no detections, so the whole line is one command.

Getting there also meant making the AI board's IP survive a reboot as a systemd
service, setting up a passwordless key from one board to the other, stopping
NetworkManager from wiping the static address with `nmcli managed no`, and retesting
the assumption that the R5 needed a manual `log on` — a cold power cycle showed it did
not, which removed the last UART cable. The system boots cold and runs over WiFi.

## Tuning without reflashing

Rebuilding the actuator firmware means a WSL build, entering boot mode, FWDN, and a
reboot. Every parameter that gets adjusted on the rig therefore lives in the Python
controller instead: the three plate angles, the hold time, the trigger box
coordinates, the debounce count and the re-arm gap. Changing any of them is a file
copy and a restart. Moving the camera meant re-measuring the box more than once, so
this paid for itself.

## Repository structure

```
BOARD_CODE/d3-g/
  fruit_controller.py        TCP server, classification gate, IPC dispatch
  IPC_Example.py             IPC send CLI, A72 to R5
  IPC_Library.py             IPC packet construction and CRC16
BOARD_CODE/ai-g/
  ai_result_sender.sh        parses tcnnapp output and streams it over TCP
  NPU_PIPELINE.md            training, conversion and deployment, by Kang Yohan
  sample_logs/               captured detection output for each class
BOARD_CODE/vcp-g/
  app.can.demo/              CAN receive and dispatch to servo, LEDs and LCD
  app.base/                  entry point
  drivers/                   PWM, I2C and LCD, GPIO, CAN
  board_pinmap.md            GPIO names against physical connector pins
3d-model/
  model_file/                exported STEP and STL
  preview/                   rendered views of the three parts
docs/
  final_presentation_ko.pdf  final presentation, in Korean
  final_report_ko.pdf        final report, in Korean
```

## Hardware notes

The plate started on an MG90S, which turns out to be a continuous-rotation servo:
speed is controllable but position is not, so timing the angle drifted every run. A
position-controlled SG90 replaced it and the CAN frame now carries the target angle
directly.

The LCD came up with a backlight but no characters, and an I2C scan returned 0x00 on
every address. Adding a scanner across both ports found nothing either, and the
actual cause was the wiring: it sat on pins that cannot be routed to I2C on this
board. Moving it to the proper header and matching the firmware port fixed it.

During wireless operation the motor once stopped responding while the LEDs and the
CAN log stayed healthy, which isolated the fault to the servo wiring — insulating
tape caught in a jumper joint.

## 3D printed parts

All three parts were modelled parametrically in CadQuery, with every dimension a
named constant, and printed on a Bambu X1-Carbon. The exported STEP and STL are here
along with a render of each part.

- **Enclosure**, 96.5 x 123.5 x 107.0 mm, holding the three boards on stacked shelves
  with openings for power, connectors and the camera
- **Tilt plate**, 110 x 50 x 1.3 mm with end walls, press-fitting onto the servo spline
- **Support wedge**, holding the servo at 30 degrees so fresh fruit rolls straight
  through, with a servo pocket and a cable route

The support was first modelled as a hollow shell, which produced a non-watertight
solid that the slicer rejected. It is now a watertight solid hollowed by infill.

## Build and run

**Prerequisites** — the Telechips TOPST SDK for the VCP-G firmware (built under
WSL), Python 3 on the D3-G board, and a trained model deployed on the AI-G board.

1. **Actuator board** — build `BOARD_CODE/vcp-g/` with the SDK and flash it over
   FWDN. This only needs redoing when the pin map or CAN format changes; all
   tuning parameters live on the controller instead.
2. **AI board** — place the three model files in
   `/usr/share/strawberry_shifted_v2_quantized/` and copy `ai_result_sender.sh`
   to `/home/root/`. See [`NPU_PIPELINE.md`](BOARD_CODE/ai-g/NPU_PIPELINE.md) for
   how the model is converted and deployed.
3. **Controller** — run `sudo python3 fruit_controller.py` on the D3-G. With
   `ORCHESTRATE = True` it fixes its own IP, starts recognition on the AI board
   over SSH, and shuts everything down after 30 seconds of silence.

One-time setup: a static IP on the AI board and a passwordless SSH key from the
controller to it.

## Credits

Chae Jihun: control and actuation boards, the communication chain, system
integration, on-site tuning and the final documents.
Kang Yohan: dataset, YOLOv8n training and the NPU deployment pipeline.
The 3D models were done jointly.

The Telechips course materials and the dataset images are not included here.
