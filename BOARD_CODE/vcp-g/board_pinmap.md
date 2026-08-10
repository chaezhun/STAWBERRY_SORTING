# VCP-G pin map

Mapping between the firmware GPIO names (`GPA`/`GPB`/`GPC`/`GPK`) and the physical
connectors on the board, transcribed from the board pinout and checked against two
known-good references: `J8D102` pin 6 is `GPIO_A[13]`, matching the PWM example, and
`J8D102` pins 1-4 are `GPB25/28/11/27`, matching the GPIO example.

## Wiring used by this project

| Part | Firmware GPIO | Connector |
|---|---|---|
| Red LED (rotten) | `GPA(6)` | J8D104 pin 16 |
| Yellow LED (unripe) | `GPA(28)` | J8D104 pin 18 |
| Green LED (fresh) | `GPA(29)` | J8D104 pin 19 |
| Tilt plate servo | `GPB(7)` | J3D100 pin 1, with 5 V on pin 2 and ground on pin 6 |
| LCD SCL | `GPC(7)` | J10D100 SCL |
| LCD SDA | `GPC(6)` | J10D100 SDA |
| Conveyor enable | `GPB(2)` | J8D104 pin 21 |

`GPA(7)` sits on J8D104 pin 17, which does not drive on this board. The blue LED is
defined against it in the header but never used, and yellow was moved to pin 18 for
that reason.

The conveyor runs from an external PWM module with a manual speed knob. The board
only switches its power through a relay on the enable pin, so none of the old
direct-PWM motor pins are wired.

## Connectors

### J8D104 (purple, silk 14-21)

| Silk | GPIO |
|---|---|
| 14 | `GPIO_C[00]` |
| 15 | `GPIO_C[01]` |
| 16 | `GPIO_A[06]` |
| 17 | `GPIO_A[07]` |
| 18 | `GPIO_A[28]` |
| 19 | `GPIO_A[29]` |
| 20 | `GPIO_B[03]` |
| 21 | `GPIO_B[02]` |

### J8D102 (blue, silk 0-7)

| Silk | GPIO |
|---|---|
| 0 | `GPIO_B[26]` |
| 1 | `GPIO_B[25]` |
| 2 | `GPIO_B[28]` |
| 3 | `GPIO_B[11]` |
| 4 | `GPIO_B[27]` |
| 5 | `GPIO_B[10]` |
| 6 | `GPIO_A[13]` |
| 7 | `GPIO_B[01]` |

### J10D100 (light blue, I2C, silk 9-13)

| Silk | GPIO |
|---|---|
| SCL | `GPIO_C[07]` |
| SDA | `GPIO_C[06]` |
| AREF | `AD0[06]` |
| GND | ground |
| 13 | `GPIO_C[12]` |
| 12 | `GPIO_C[15]` |
| 11 | `GPIO_C[14]` |
| 10 | `GPIO_C[13]` |
| 9 | `GPIO_B[00]` |

### J8D101 (yellow, silk A0-A7)

| Silk | GPIO |
|---|---|
| A0 | `AD0[03]` |
| A1 | `AD0[04]` |
| A2 | `GPIO_C[02]` |
| A3 | `GPIO_C[03]` |
| A4 | `GPIO_C[05]` |
| A5 | `GPIO_C[04]` |
| A6 | `AD0[05]` |
| A7 | `AD0[01]` |

### J8D103 (green, silk A8-A11 and S4-S7)

| Silk | GPIO |
|---|---|
| A8 | `GPIO_C[08]` |
| A9 | `GPIO_C[09]` |
| A10 | `GPIO_C[10]` |
| A11 | `AD0[02]` |
| S4 | `GPIO_K[14]` |
| S5 | `GPIO_K[15]` |
| S6 | `GPIO_K[01]` |
| S7 | `GPIO_K[08]` |

### J3D100 (pink, 2x3)

| Pin | GPIO |
|---|---|
| 1 | `GPIO_B[07]` |
| 2 | `5P0_B` |
| 3 | `GPIO_B[04]` |
| 4 | `GPIO_B[06]` |
| 5 | `GPIO_B[05]` |
| 6 | ground |

### J5D100 (lavender, 2x5)

| Pin | GPIO | Pin | GPIO |
|---|---|---|---|
| 1 | `3P3_D` | 2 | `3P3_C` |
| 3 | `GPIO_K[09]` | 4 | `GPIO_K[01]` |
| 5 | `GPIO_K[08]` | 6 | `GPIO_K[02]` |
| 7 | `GPIO_K[07]` | 8 | `GPIO_K[03]` |
| 9 | ground | 10 | ground |

### J8D100 (red)

Power only: `3P3_A` as IOREF, `PORN` as reset, `3P3_B` at 3.3 V, `5P0_A` at 5 V, two
grounds, and VIN.

### J18D100 (large blue)

Even pins 22-52 and odd pins 23-53, with ground and `5P0_C` down the outer columns.
It carries most of the remaining `GPIO_A` lines plus `GPIO_K[11]`, `GPIO_K[13]` and
`GPIO_B[19..24]`. Nothing in this project is wired to it.
