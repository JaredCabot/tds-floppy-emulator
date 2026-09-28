# Hardware Reference - SFR1M44-DU26 floppy emulator (PCB SFRC2D.B)

Reverse-engineered pin map for the Artery AT32F415 variant. Source: LiveBoxAndy's
KiCad netlist ([liveboxandy/Gotek-SFRC2DB](https://github.com/liveboxandy/Gotek-SFRC2DB)) cross-checked against his
[schematic (PDF)](https://github.com/liveboxandy/Gotek-SFRC2DB/blob/main/Gotek.pdf). `docs/02-pinmap.csv` is the machine-readable single source
of truth for code; the tables here are the human-readable view of the same data.

## Core parts
| Ref | Part | Role |
|-----|------|------|
| U3  | AT32F415RBT7 (LQFP64), 8 MHz HEXT crystal | Main MCU. Up to 144 MHz, 32 KB SRAM, 128 KB flash |
| U8  | SST25VF016B (16 Mbit SPI NOR, 2 MB) | Serial flash - the 1.44 MB buffer store |
| U1  | 74AHC04 (hex inverter) | Buffers floppy outputs; open-drain via Q1–Q6, 1K pull-ups to 5V |
| U4  | AMS1117-3.3 | 5V→3.3V regulator |
| P1  | 26-pin FFC | Slimline floppy bus to host (scope) |
| P3  | USB-A | USB host port (the stick) |

## Electrical notes (review 2026-09-28, against DS_AT32F415 V2.02)

Two limits of the board design, the same with any firmware:

- **SEL (PA0), STEP (PA1) and DIR (PB0) are on pins that are not 5 V
  tolerant, and a host that drives them high lifts the 3.3 V rail.** These
  three pins' absolute maximum is 4.0 V (DS Table 6; the pin table, pp. 26-27,
  marks them I/O without FT). Above the rail, each pin's protection diode
  conducts into the 3.3 V rail. With only the board's 1 k pull-ups to 5 V this
  is about 1 mA per pin and the rail stays at 3.3 V. A host that drives the
  lines high itself pushes much more. Measured 2026-09-28, unit in a TDS 794D:
  the three pins at about 4.4 V and the 3.3 V rail at 3.8 V; the same unit on
  a bench 5 V supply with no host: 3.3 V. The regulator (U4, AMS1117-3.3) can
  source current but not sink it, so once the injected current exceeds what
  the MCU and flash draw, the rail floats up. That puts the AT32F415 and the
  SST25VF016B above their 3.6 V operating limit (both 4.0 V absolute maximum)
  and the three pins above theirs. Units have run like this for years, with
  any firmware, but it is outside the datasheets. Hardware fix: a series
  resistor of about 1 k between each of the three lines and its pin, which
  limits each to about 1 mA, well below the MCU's own draw, so the rail stays
  in regulation. Moving the pull-ups to 3.3 V does not help when the host
  drives the lines itself. An SWD probe's VTref reads the raised rail too.
  SIDE1 (PB4), WGATE (PB9) and WDATA (PA8) are on 5 V-tolerant pins. Since
  1.2.0 the firmware leaves the MCU's internal pull-ups off on all six inputs,
  as the datasheet requires above VDD + 0.3 V (5.3, I/O static
  characteristics, note 3).
- **U1 (74AHC04) runs from 5 V but is driven by the 3.3 V MCU.** At 5 V an AHC
  input is only guaranteed high above 0.7 x VCC, about 3.5 V. It works because
  the real threshold is near half the supply, but it is not guaranteed. A
  74AHCT04 (TTL inputs, high above 2.0 V) is a drop-in replacement.

## Floppy interface - inputs to MCU (from host)
| MCU pin | Signal | Notes |
|---------|--------|-------|
| PA0  | SEL   | drive select, active low |
| PA1  | STEP  | head-step pulse |
| PB0  | DIR   | step direction |
| PB4  | SIDE1 | head/side select |
| PB9  | WGATE | write gate |
| PA8  | WDATA | write data stream from host |
| PA7  | RDATA_3V3 | read-data feedback/timing tap |

## Floppy interface - outputs from MCU (to host)
Path: MCU pin -> 74AHC04 inverter -> PMBT2222A open-drain -> 1K pull-up to 5V -> P1.
| MCU pin | Signal | Inverter | Q | Host pin |
|---------|--------|----------|---|----------|
| PB8 | INDEX  | U1A | Q1 | P1.2  |
| PB6 | TRK0   | U1C | Q2 | P1.20 |
| PB5 | WPROT  | U1D | Q3 | P1.22 |
| PB3 | READY  | U1E | Q4 | P1.8  |
| PA7 | RDATA  | U1B | Q5 | P1.24 |
| PB7 | DSKCHG | U1F | Q6 | P1.6  |

## Control / status
| MCU pin | Signal | Function |
|---------|--------|----------|
| PC7 | BUT_L | Left button - WRITE-mode toggle. Active low (S3 to GND) |
| PC8 | BUT_R | Right button - NEXT-page. Active low (S4 to GND) |
| PB10 | I2C2_DTA | Bi-colour LED (red), **active-low** + display SDA on header J7 |
| PB11 | I2C2_CLK | Display SCL on header J7 |

> LED wiring (PCB netlist): L4 green + L5 red share a common anode (net L4-A,
> to +3.3 V); red cathode = PB10, green cathode = SEL. So red lights when PB10
> is LOW (active-low), green lights whenever the scope selects the drive.
>
> LED convention (from instructions file): red = USB<->buffer transfer,
> green = host<->buffer transfer. The LED nets share the SEL / I2C2_DTA lines;
> confirm drive polarity on the bench before trusting exact colour control.

## SPI2 -> SST25VF016B serial flash (the buffer store)
| MCU pin | Signal | Flash pin |
|---------|--------|-----------|
| PB12 | SPI2_SS   | CE#  (1) |
| PB13 | SPI2_SCK  | SCK  (6) |
| PB14 | SPI2_MISO | SO   (2) |
| PB15 | SPI2_MOSI | SI   (5) |

## USB host (P3)
| MCU pin | Signal | via |
|---------|--------|-----|
| PA12 | USB D+ | R1 22R |
| PA11 | USB D- | R2 22R |

USB OTGFS peripheral run in **host** mode to read the stick.

## Debug / boot
| Pin | Signal | Header |
|-----|--------|--------|
| PA13 | SWDIO | J10.4 |
| PA14 | SWCLK | J10.3 |
| PA9  | USART1_TX | J3.3 |
| PA10 | USART1_RX | J4.1 |
| BOOT0 | boot select | J3.1, 4K7 pull-down (RN13) |
| NRST | reset | J4.2, 4K7 pull-up (R10) + 100nF |

### Header pinouts (from PCB netlist - mind the rails)
- **J10** (SWD): 1=GND, 2=**5V**, 3=SWCLK(PA14), 4=SWDIO(PA13).
  WARNING: J10 pin 2 is **5V**, not 3.3 V. For an ST-Link, take the 3.3 V
  voltage reference from J3.2, never J10.2. See docs/06.
- **J3**: 1=BOOT0, 2=**+3.3V**, 3=USART1_TX(PA9).
- **J4**: 1=USART1_RX(PA10), 2=NRST.

## Straps
- RA (PC6), RB (PA2), RC (PB1): 0R links to GND, present in stock config.
  FlashFloppy removes them; **our clone keeps the board stock**.
- Rear jumper block: install one jumper on the two pins closest to the ZIF
  socket (S1, drive select), leave the others open (per instructions file).

### Rear jumper block (schematic)
None of these reaches the MCU: they are straps on the interface lines, and the
firmware sees only the internal select net SEL (PA0, also the green LED).

| Jumper | Pins | Effect | With this firmware |
|---|---|---|---|
| **S1** | MO1 1-2 | SEL driven by the interface's DRIVE SELECT line | Required for the TDS; tested |
| **MO** | MO1 2-3 | SEL driven by MOTOR ON instead (hosts that enable a drive by motor-on alone) | Works without firmware changes (SEL is SEL), untested, not needed on the TDS; the green LED then follows motor-on |
| **JE** | JE1 1-2 | HD OUT tied to GND: tells the host "2DD (720 KB)" | **Supported from 1.1.0.** Off: 1.44 MB HD disks (HD OUT floats high). Fitted: 720 KB DD; the disk becomes DD when the host formats it (docs/10). The MCU cannot read this jumper: pin 9 goes only to a 1 k pull-up and JE |
| **JD** | JD1 1-2 | Pin 11 tied to GND (net DINST on the schematic) | Not used. Pin 11 is not connected in the TEAC FD-05HF spec (docs/09); generic 26-pin tables give it as density select (/REDWC), driven by the host. The MCU does not see it and the TDS detects the disk through READY and DISK CHANGE. Leave off |

MO1 is one 3-pin header: pin 2 (SEL) is common, so S1 and MO are the two
positions of the same jumper. Pin 11 (net DINST), MTRON pin 10 and HD OUT
pin 9 each have a 1 k pull-up to 5 V (schematic, traced 2026-09-27). Signal
names: pin 9 is HD OUT in the TEAC spec and MEDIA ("media sense") in generic
26-pin tables; pin 11 is NC in the TEAC spec and /REDWC ("density select",
a host output) in generic tables. The DINST ("disk installed") net name is the
schematic's; no drive spec found supports it.

## Memory budget - the central constraint
- 32 KB SRAM total. A full 1.44 MB floppy image does **not** fit in RAM.
- The "buffer" is therefore the 2 MB SPI NOR (U8), not RAM.
- Floppy image is served track-by-track from SPI flash; RAM holds only the
  working track buffer(s) + USB stack + FatFs. Every design decision follows
  from this.
