# 🏓 PING PONG SLIDER

**A two-player Pong game played across two OLED displays, powered by two Seeed Studio XIAO ESP32-S3 boards communicating wirelessly through ESP-NOW.**

> **Firmware version: v1.4** · Arduino / ESP32-S3 project · English documentation
>
> The PCB used for this project is based on **Acathla-fr's IFT-S3**: [original GitHub repository and KiCad sources](https://github.com/Acathla-fr/IFT-S3/tree/main). We do not claim authorship of the original hardware design.

<!-- Coming soon: game banner or photograph -->
<!-- ![PING PONG SLIDER — two boards during gameplay](docs/images/ping-pong-slider.jpg) -->

## Table of Contents

- [Overview](#overview)
- [Demo](#demo)
- [Hardware](#hardware)
- [Controls](#controls)
- [How the Game Works](#how-the-game-works)
- [Wireless Communication](#wireless-communication)
- [Installation](#installation)
- [Repository Structure](#repository-structure)
- [Version History](#version-history)
- [Troubleshooting and Limitations](#troubleshooting-and-limitations)
- [Credits and Attribution](#credits-and-attribution)

## Overview

**PING PONG SLIDER** turns two PCBs, each equipped with a XIAO ESP32-S3 and an OLED display, into a two-player Pong game. Each player controls a paddle at the bottom of their screen. A ball travels upward to the top boundary, *virtually crosses over* to the opponent's display, moves downward, and must be returned with the paddle.

Key features include:

- An **OLED menu** to choose Player 1 or Player 2 without editing the firmware.
- **Coordinated role assignment** between both boards.
- A **ready-up flow** and synchronized countdown.
- A ball whose **bounce angle** depends on where it hits the paddle.
- A **5% speed increase per paddle bounce** and at the start of each new round.
- **ESP-NOW game-state transfers**, with no Wi-Fi router required.
- A **five-round match**, shared bounce counter, and end-of-match statistics.
- **Return-to-menu and forfeit handling** through the Select button.

The goal is a standalone experience: **neither a computer nor an open Serial Monitor is required during gameplay**.

## Demo

<p align="center">
  <img src="docs/images/20261021_171604.jpg"
       alt="Two Ping Pong Slider boards"
       width="48%">
  <img src="docs/images/20261021_172258.jpg"
       alt="Ping Pong Slider final match statistics"
       width="36%">
</p>

<p align="center">
  <em>
    PING PONG SLIDER running on two IFT-S3 boards
    equipped with XIAO ESP32-S3 microcontrollers.
    Main menu and final match statistics.
  </em>
</p>

## Hardware

For a two-player game:

| Quantity | Component | Purpose |
| --- | --- | --- |
| 2 | **Seeed Studio XIAO ESP32-S3** | Microcontroller and ESP-NOW wireless communication |
| 2 | **IFT-S3 PCBs**, populated with the required components | Button matrix and board connections |
| 2 | **128 × 64 I²C OLED displays (SSD1306)** | Game and menu display |
| 2 | **Compatible Wi-Fi antennas** for the XIAO setup | Wireless link between both boards |
| 2 | Suitable USB power supplies | Standalone power for each board |

The firmware configures the OLED at I²C address **`0x3C`** (SDA = GPIO5, SCL = GPIO6) and two NeoPixels on GPIO4. It uses the IFT-S3 PCB's button matrix.

## Controls

| Physical button | In-game name | Action |
| --- | --- | --- |
| **SW1** | Up | Select Player 1 in the menu |
| **SW7** | Down | Select Player 2 in the menu |
| **SW3** | A | Confirm the role, mark yourself ready, and confirm relevant actions |
| **SW4** | Left | Move the paddle left |
| **SW2** | Right | Move the paddle right |
| **SW5** | Select | Request a return to the menu or forfeit the match |
| **SW6** | B | Cancel a forfeit confirmation |

**Note:** These buttons are read through a **diode-equipped matrix scan**, not as seven independent buttons wired directly to ground. The mappings above reflect the IFT-S3 hardware used during development.

## How the Game Works

### 1. Player selection

Both boards run **the exact same sketch**. On the `PING PONG SLIDER` menu, Up/Down selects the desired role and A confirms it. ESP-NOW coordination assigns the complementary role to the other board so that both players cannot end up with the same number.

### 2. Getting ready

Each OLED shows its assigned role (`Vous etes Joueur 1/2` in the firmware) and `A: Suivant`. After confirming, each player waits for their opponent. Once both players are ready, the displays show:

`3` → `2` → `1` → `Partie lancee !`

Each countdown number stays on screen for one second; the start message appears for **two seconds**.

### 3. Ball rally

- The **first player to move their paddle** initiates the serve.
- Each paddle is **20 × 2 pixels** and moves with Left/Right.
- The ball is a circle with a **2-pixel radius**, with an initial speed of **35 pixels/second**.
- The ball bounces off the side boundaries and moves upward after a paddle hit. The outgoing angle depends on the contact point.
- When the ball reaches the top boundary, its state is **sent to the other XIAO**, where it reappears at the top and moves downward.
- Ball speed increases by **5% on each paddle bounce** and by **5% at the start of each new round**.

The top bar displays the local player (`J1` or `J2`), the **score from that player's perspective** (`| 3-2 |` means “me – opponent”), and the **shared total bounce count**.

### 4. End of a round and scoring

When a player misses the ball, their OLED shows **`Balle perdue !`** (“Ball lost!”); the opponent sees **`Manche gagnee !`** (“Round won!”). The result remains visible for **three seconds**, and the next round is prepared automatically.

A match consists of **five rounds in total**. After the fifth round, the game displays the final result and statistics:

```text
J1|3-2|J2
Rebonds : 42
Temps ecoule : 04:26
A: Nouvelle partie
```

*Illustrative example; actual values depend on the match. The firmware's on-screen UI currently uses French labels.* Pressing **A** on the statistics screen returns both players to the main menu.

### 5. Returning to the menu and forfeiting

The **Select button (SW5)** can be used at any time:

- **Before a match starts:** it takes both players back to the main menu.
- **During a match:** it opens `Quittez la partie ?` (“Quit the game?”) with **A: YES** and **B: NO** actions; the on-screen labels remain `A: OUI` and `B: NON`.
- If the forfeit is confirmed, the opponent sees **`Partie abandonnee par Joueur X`** (“Game abandoned by Player X”) and can return to the menu by pressing A.

## Wireless Communication

The game uses **ESP-NOW**, an ESP32 wireless communication protocol. No router, Wi-Fi password, or home network is needed.

In **v1.4**, both boards operate on **Wi-Fi channel 1** and exchange **broadcast** messages. The firmware handles role assignment, ready status, countdown start, serve initiation, ball handoffs, acknowledgments, bounces, missed balls, forfeits, and resets.

Ball handoffs and round results use **sequence numbers, acknowledgments, and retries** to reduce desynchronization caused by lost packets. These measures do not guarantee a completely lossless radio link.

> **Current limitation:** This implementation targets **one pair of boards in the same area**. Multiple independent pairs nearby may interfere with discovery and broadcast messages. Dedicated pairing would be needed for reliable multi-pair operation.

### No Serial Monitor required

Serial debugging is disabled by default:

```cpp
#define ENABLE_SERIAL_DEBUG 0
```

This allows the game to run **without an open Serial Monitor**. Temporarily change the value to `1` when diagnosing an issue.

## Installation

### Requirements

1. Install [Arduino IDE](https://www.arduino.cc/en/software).
2. Install **ESP32 by Espressif Systems** (the **3.x** series, compatible with the ESP-NOW callback signatures used here).
3. Select **Seeed XIAO ESP32S3**, or the corresponding board entry in your installed package.
4. Install **Adafruit GFX Library**, **Adafruit SSD1306**, and **Adafruit NeoPixel** through the Arduino IDE Library Manager. `WiFi`, `Wire`, `esp_now`, and `esp_wifi` come from the Arduino/ESP32 environment.

### Flashing both boards

1. Download or clone this repository.
2. Open **`Ping_Pong_Slider_v1.4/Ping_Pong_Slider_v1.4.ino`** in Arduino IDE.
3. Connect the first XIAO ESP32-S3, select the board and serial port, then **upload** the sketch.
4. Repeat with **the exact same sketch** on the second XIAO.
5. Power both boards, choose player roles on the OLEDs, and start a match.

You do not need to enter MAC addresses or hard-code a player number.

### Updating from VS Code

If the project has been cloned and you are ready to publish changes, use VS Code's **Source Control** panel to stage files, create a **commit**, and select **Sync Changes** (or run `git push`).

## Repository Structure

The following layout reflects the **six Arduino folders** in the original local repository:

```text
Ping Pong Slider/
├── README.md
├── Ping_Pong_Slider_v1.0/
│   └── Ping_Pong_Slider_v1.0.ino
├── Ping_Pong_Slider_v1.1/
│   └── Ping_Pong_Slider_v1.1.ino
├── Ping_Pong_Slider_v1.2/
│   └── Ping_Pong_Slider_v1.2.ino
├── Ping_Pong_Slider_v1.3/
│   └── Ping_Pong_Slider_v1.3.ino
├── Ping_Pong_Slider_v1.4/
│   └── Ping_Pong_Slider_v1.4.ino
└── reset_all/
    └── reset_all.ino
```

| Folder/file | Contents |
| --- | --- |
| `Ping_Pong_Slider_v1.0` through `v1.3` | Earlier firmware iterations retained for development history |
| **`Ping_Pong_Slider_v1.4`** | **Latest provided firmware version**, intended for gameplay |
| `reset_all` | Utility sketch for testing/resetting board peripherals |
| `README.md` | Project overview, installation instructions, functionality, and attribution |

**Photos and a video** will be added later. A `docs/images/` folder can be created without changing the firmware directory layout.

## Version History

| Version | Status |
| --- | --- |
| `v1.0` | First version archived in this project |
| `v1.1` | Development iteration |
| `v1.2` | Development iteration |
| `v1.3` | Development iteration |
| **`v1.4`** | **Latest supplied version: multiplayer match, five rounds, statistics, and forfeit handling** |

These versions are preserved as **separate firmware files**, not Git tags. Detailed changes between versions can be added from the development history.

## Troubleshooting and Limitations

- **OLED stays black:** Check power, I²C address (`0x3C`), and SDA/SCL wiring.
- **Boards cannot communicate:** Ensure both run the same firmware version on the same ESP-NOW channel and that their antennas are properly connected.
- **Buttons do not respond:** Verify the SW1–SW7 mapping and diode-matrix wiring.
- **Gameplay slows down:** Make sure `ENABLE_SERIAL_DEBUG` is set to `0` for standalone operation.
- **Several pairs are playing nearby:** The current broadcast implementation does not isolate pairs and is not designed for that configuration.

This project is still evolving. Radio stability, collision handling, and transitions between rounds should be tested on both physical PCBs after each protocol change.

## Credits and Attribution

### IFT-S3 hardware

The PCB used in this project is based on **[Acathla-fr — IFT-S3](https://github.com/Acathla-fr/IFT-S3/tree/main)**. The KiCad source files used as hardware references include the board layout, button matrix, and schematic files:

- `IFT-S3.kicad_pro`
- `IFT-S3.kicad_sch`
- `keyboard_matrix.kicad_sch`
- `IFT-S3.kicad_pcb`
- `IFT-S3-1couche.kicad_pcb`

**Many thanks to the IFT-S3 authors for designing and sharing this hardware.** PING PONG SLIDER is a game/firmware project built **on that hardware platform**; the original PCB design is not ours.

### Software

The **PING PONG SLIDER** firmware and its gameplay mechanics were developed for this project using the IFT-S3 hardware platform. Adafruit libraries and Espressif software components are used under their respective terms.
