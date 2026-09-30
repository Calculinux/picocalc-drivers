# PicoCalc keyboard: interrupt-delivery evaluation & plan

Status: research/evaluation + agreed plan — no code changes in this document.
Revised: 2026-09-30 (v2: adds UART/USB switch topology, compat-mode and
firmware-update plan). Line numbers refer to `main` at `cf9f01a` unless noted.

Goal of this document: capture everything learned while evaluating a move from
the current 128 Hz I²C-poll keyboard pipeline to interrupt-driven event
delivery — across the Linux drivers (this repo), the PicoCalc mainboard
hardware, and the two STM32 keyboard firmwares — plus the agreed plan to
implement it seamlessly (no DIP switches, no case opening, after a one-time
first install). Everything here should be reusable by whoever implements.

Legend: **[verified]** = traced to source/schematic with citation,
**[assumed]** = inferred, worth confirming, **[open]** = unresolved.

## 1. Current Linux-side architecture (this repo)

The MFD keyboard driver (`drivers/picocalc_mfd_kbd/picocalc_mfd_kbd.c`) runs a
fixed pipeline:

- Soft timer at `HZ/128` (≈7.8125 ms → 128 Hz), `kbd_timer_function()`
  **L577-582**. Each expiry `schedule_work`s on `g_ctx->work_struct`.
- `input_workqueue_handler()` (**L515**) drains the device FIFO via
  `input_fw_read_fifo()` (**L225-277**): repeated 2-byte `regmap_bulk_read` of
  `REG_ID_FIF (0x09)` until a zero terminator; up to 31 items.
- Each FIFO item → `key_report_event()` (**L279**): scancode→keycode mapping,
  shift tracking, dual-shift mouse-mode toggle, mouse direction flags,
  second-key emulation (F6..F10, Break/Home/End/PageUp/PageDown/Ins).
- **Idle cost: ~128 I²C transactions/sec + 128 SoC wakeups/sec, permanently.**

Hidden coupling: the mouse-mode repeat engine rides on the 128 Hz heartbeat —
while `mouse_move_dir` is armed and `mouse_mode` is on, every tick emits
`REL_X/REL_Y` with a 1×/2×/4× hold ramp (**L529-561**). Any redesign that
decouples event intake from the heartbeat must re-home this repeater (a
repeating `delayed_work` at the current ~7.8 ms period, armed by
`mouse_move_dir`) or mouse mode loses its cadence. This refactor is shared by
every option below.

Known debt touched by this topic:

- File-global `g_ctx` and `DEFINE_TIMER(g_kbd_timer)` (**L575, L613, L727**).
- Dead IRQ scaffolding already written but disabled:
  - `devm_request_threaded_irq(&i2c_client->dev, i2c_client->irq, NULL,
    input_irq_handler, IRQF_SHARED | IRQF_ONESHOT, …)` — **L680-703** (references
    a handler that no longer exists).
  - `kbd_write_i2c_u8(ctx, REG_ID_INT, 0)` after drain — "clear client interrupt
    flag", **L568-572**.
  The drain code is thread-safe (no locking; already used from workqueue
  context), so a threaded IRQ handler can reuse `input_fw_read_fifo()` /
  `key_report_event()` verbatim.
- `luckfox-lyra/picocalc-luckfox-lyra.dtsi`: `picocalc-mfd@1f` has **no
  `interrupts`/`irq-gpios`**; Lyra 31/32 (GPIO4_B2/B3) reserved for M0 audio,
  RM_IO12/13 for the PWM-audio overlay — both spoken for.
- Legacy `drivers/picocalc_kbd/` flavor is architecturally identical (same
  FIFO protocol, same workqueue/timer) and inherits any shared refactor.

## 2. Mainboard hardware: the complete UART/USB picture

Sources: `clockwork_Mainboard_V2.0_Schematic.pdf` (sheet 1/1), the annotated
J701/U701/U702/U703 region provided by the maintainer, and the Calculinux
device trees.

The keyboard MCU is an **STM32F103R8T6**, I²C slave at 0x1F.

### 2.1 The MCU serial/USB paths

**[verified]** The Type-C port (J701) is a **sink** (CC1/CC2 pulldowns
R701/R702, 5.1 kΩ) and powers the board. Two DIP-switched multiplexers
(WAS7227 SPDT, selectors from SW701) define all connectivity:

| Switch | Selector | Position A (assumed factory) | Position B |
|---|---|---|---|
| U701 | `SEL2` | Type-C D± → **CH340C** USB | Type-C D± → `M_USB_DP/DM` (STM32 PA11/PA12 native USB) |
| U703 | `SEL1` | CH340C TxD/RxD → **Lyra `UART0` pair** (console) | CH340C TxD/RxD → **MCU flashing/debug UART** (USART1, PA9/PA10 net, a.k.a. "UART1") |

Consequences:

- The **console path is passive hardware**: Type-C → CH340C → (SEL1=A) → Lyra
  uart0. This is where U-Boot `stdout-path = &uart0`
  (`uboot-rk3506-luckfox.dtsi`) and kernel `earlycon` land. The keyboard MCU
  is *not* in this path; stock firmware does no "USB forwarding". This path is
  sacrosanct — nothing in this plan may disturb it. SEL1's factory position
  (A) is **[open: verify]**.
- **MCU flashing** (STM32 ROM bootloader, USART1-only) is reachable only via
  SEL1=B + CH340C + USB — i.e., the DIP switch + case access in the BIOS
  README. No other path reaches PA9/PA10.
- The **STM32's native USB** (PA11/PA12) is live but only behind SEL2=B —
  a USB-based transport is *possible but switch-gated*; judged moot for
  production (§8 option C).

**[verified]** There is a **third, un-switched path**: the Lyra's **UART1
pair** → mainboard net `M_UART3_RX` → **MCU pin PC10** (and `M_UART3_TX` →
PC9), straight to the connector with no DIP selector. Notes:

- PC9 is consumed as the **AXP2101 PMU interrupt input** by *both* firmware
  trees, so only the PC10 leg is practically usable.
- In the BIOS, **PC10 is the `PICO_IRQ` output** (§4). In stock firmware PC10
  is unowned (floating input).
- **[open — the single remaining hardware unknown]** which **Lyra GPIO balls**
  the M_UART3 pair terminates on. Must be measured (continuity from connector
  socket → Lyra pins); not provable from the schematic text available.

### 2.2 Inventory of MCU external nets

| Net (MCU side) | MCU pin | Role / destination |
|---|---|---|
| keyboard I²C | PB9 (SDA)/PB8 (SCL) | → mainboard → Lyra i2c2 (RM_IO10/11) — the register/FIFO bus |
| AXP2101 PMU bus | PB11/PB10 | PMU SDA/SCL |
| AXP2101 IRQ | PC9 | `M_UART3_TX` net — **PMU input** (both FW) |
| **`M_UART3_RX`** | **PC10** | **un-switched; = `PICO_IRQ` in BIOS; the IRQ carrier** |
| HP_DET | PC11 | headphone detect (R201, 1.2 kΩ) — **stolen as USART3_RX in BIOS `UART_PICO_INTERFACE` mode** |
| `M_USB_DP/DM` | PA11/PA12 | native USB → SEL2 → Type-C (DIP-gated) |
| debug/flash UART (USART1) | PA9/PA10 | → SEL1 → CH340C (DIP-gated) |
| power/chg/button nets | — | CHGLED, PWR_OK, power button |
| matrix ROW1-8/COL1-8, membrane M11-M78 | internal | not brought out |

**[verified]** No dedicated kbd-INT net exists other than the repurposed
M_UART3_RX line — every MCU output is accounted for above. Corroboration:
Jack's forum post — "the only connections between the pico board and the stm32
are the I2C and the UART bus… I was thinking of reconditioning it as IRQ_pin".

### 2.3 Pinmux landmine in the Calculinux DTS (independent fix needed)

**[verified]** `linux-rk3506-luckfox-lyra.dtsi` (kernel) enables
`&uart1 { pinctrl = rm_io30 (TX) + rm_io28 (RX); status = "okay"; }`, while
`picocalc-luckfox-lyra.dtsi` (this repo) assigns the **same two balls** to the
SD-card reader (`&spi1`: rm_io29 clk, **rm_io28 mosi**, rm_io31 miso,
**rm_io30 csn0**). Ball mapping (kernel
`arch/arm/boot/dts/rk3506-pinctrl-rmio.dtsi`): **RM_IO28 = GPIO1_C3,
RM_IO30 = GPIO1_D2**. Two drivers request the same pins → runtime pinctrl
conflict (first binder wins).

**[assumed]** Since the mainboard cannot route one net to both an SD slot and
a UART, the M_UART3 pair almost certainly does *not* land on RM_IO28/30;
those are likely stock LuckFox Lyra debug-header balls (unwired in the
PicoCalc case), making the uart1 node generic-Luckfox heritage. Likely fix:
`status = "disabled"` for uart1 in the picocalc dtsi (SD reader keeps the
balls) — **pending runtime confirmation** via `/sys/kernel/debug/pinctrl` and
a check that the SD slot works today. File as its own fix regardless of the
IRQ work.

## 3. Stock firmware: `clockworkpi/PicoCalc` — `Code/picocalc_keyboard`

(Arduino core, busy-loop; `master` and `devel` checked.)

Protocol **[verified]** (`reg.h`, `conf_app.h`, `reg.ino`,
`picocalc_keyboard.ino`):

- Registers: `TYP 0x00` (0x00 = official), VER 0x01, CFG 0x02, **INT 0x03**,
  KEY 0x04, BKL 0x05, DEB 0x06, FRQ 0x07, RST 0x08, FIF 0x09, BK2 0x0A, BAT
  0x0B, C64_MTX 0x0C, C64_JS 0x0D, OFF 0x0E. Write flag = MSB of reg address.
- Keys scanned in busy `loop()` (10 ms idle) into a 31-slot FIFO; host reads
  2-byte items until 0x00.
- INT latch: `key_cb()` sets `INT_KEY` when `CFG_KEY_INT`; `lock_cb()` sets
  INT_CAPSLOCK/INT_NUMLOCK; overflow sets INT_OVERFLOW. **Defaults on**:
  `CFG_OVERFLOW_INT | CFG_KEY_INT | CFG_USE_MODS | CFG_REPORT_MODS`.
  Overflow defaults to **drop-new-entry**.
- Ack: host write *replaces* the whole INT byte → writing 0x00 clears all.
- **I²C watchdog: no traffic for 2.5 s → `ResetI2CBus()`.** Reduced-poll
  schemes need keepalive ≤ ~2 s spacing.

IRQ pin status **[verified]**: `INT_DURATION_MS = 1` exists but is unused; the
only pulse code is **commented out** in `lock_cb()` ("// int_pin can be a
LED") and `int_pin` is declared nowhere (master and devel). The official
firmware carries a non-functional draft (per Jack: no program space left).

## 4. Alternative firmware: `jackcartersmith/picocalc_BIOS`
(mirrored at `Calculinux/picocalc_BIOS`)

Local: `/home/benklop/repos/PicoCalc/picocalc_BIOS`. Mirror verified **0
commits behind upstream** on 2026-09-30 (HEAD `1b649c8`
"IC2S: readded missing REG_ID_INT in callback handler" — the INT ack path
*regressed and had to be re-added*; smoke-test the handshake on first flash).

Full HAL/CubeMX rewrite; "more efficient both functionally and electrically",
~3.5 mA run, <0.1 mA standby.

### 4.1 Implemented IRQ protocol — the headline finding

- `PICO_IRQ` = **GPIOC pin 10 (PC10)**, push-pull output, **active-low**
  (`Core/Inc/hal_interface.h:109-110`).
- Asserted (driven LOW) from `key_cb()` per key event when `INT_KEY` ∈
  `REG_ID_INT_CFG`, from `lock_cb()` on lock toggles, and on overflow/RTC
  alarm (`Core/Src/main.c`). Guarded by `#ifndef UART_PICO_INTERFACE`.
- De-asserted **only** by the host's I²C write to `REG_ID_INT`: masked clear —
  `reg_set_value(REG_ID_INT, old & ~written_mask)` — then the pin is raised
  (`Core/Src/i2cs.c`). The pin rises on *any* write to that register, mask or
  not. Treat the line as "event pending → go drain": latch is advisory, the
  FIFO is the authoritative backlog.
- Handshake: *event → pin LOW → host IRQ → drain FIFO → masked ack → pin
  HIGH*.
- **⚠ Compat gap:** the pin is configured as an output unconditionally at
  boot. On a Lyra whose ball is still muxed UART-TX (idle-high driven), a
  keypress-induced LOW would fight the SoC driver. **Required firmware
  change:** gate both the output configuration and all asserts behind a
  `SYS_CFG`/`INT_CFG` bit — proposed `IRQ_LINE_ENABLE`, **default 0**, pin
  left as pulled input until set. ~20 lines; request upstream to Jack. Until
  it lands, flashing the BIOS with the ball not remuxed to GPIO is *unsafe*.

### 4.2 Register-map extensions (host must branch on TYP)

- **`REG_ID_TYP = 0xCA`** vs 0x00 stock — readiness gate for all custom
  behavior (vendor-id probe agreed in the forum).
- `SYS_CFG 0x02` (successor of CFG) + separate **`INT_CFG 0x12`**; stock
  `CFG_KEY_INT` (CFG bit 4) moves to INT_CFG bit 3.
- `REG_ID_DEB` write = 16-bit hold period (2 bytes back on read).
- Full **RTC** exposure (`RTC_CFG/DATE/TIME/ALARM_DATE/ALARM_TIME`) on the
  internal RTC (backup domain survives standby). Admitted warts: calendar
  day-roll, sleep date retention — WIP. Alarms trigger the IRQ line only.
- Backlight scale 0-9; out-of-range writes (our driver writes 0-255,
  `default-brightness = 128`) are linearly mapped for compatibility (coarse
  stepping perceptible if driven finely).

### 4.3 Power & lifecycle

**[claimed/verified-in-code]** ~3.5 mA run; <0.1 mA standby (PMU alive,
STM32 asleep, RTC running); settings in emulated flash EEPROM; power-button
semantics (Shift+short = pico reset; long = PMU shutdown, optional host-ACK);
I²C re-architected (fixes stock "i2c got stuck" reports). Flash = 64 KB.
Watchdog was **removed** upstream ("Sadly removed the watchdog") — update/
rollback safety must live in app self-test (§7.2).

### 4.4 Alternate transport: `UART_PICO_INTERFACE`

Build flag: **USART3 PC10 TX / PC11 RX**, 115200 8N1, GPIO IRQ suppressed.
Status: init/NVIC present; **no key-event transmit path found in `Core/Src`**
— scaffolding/experimental. MCU→host simplex; RX leg steals PC11
(stock: HP_DET). Option B′ (§8) — contingent alternative.

### 4.5 Host reference driver

`tests/pcsb/` (Pico SDK/RP2040) is deliberately poll-based, works with both
firmwares — **not** an IRQ-consumer reference. No public Linux-side IRQ
consumer exists; ours would be the first. shtirlic maintains a NuttX IRQ
driver (forum) — sync candidate.

## 5. Prior art & intent (forum)

[Custom PicoCalc BIOS/keyboard firmware](https://forum.clockworkpi.com/t/custom-picocalc-bios-keyboard-firmware/17292)
(May 2025+, Ben participating):

- shtirlic's motivating ask: IRQ instead of constant polling via "spare second
  UART" pins; proposed 7-bit command-register taxonomy (resets/shutdown/power
  sequencing).
- Jack: intended carrier = the spare UART line; roadmap "I2C IRQ mode
  (+docs)"; stock FW has only an unimplemented draft.
- shtirlic (Aug 2025): BIOS stable on hardware; stock 1.2 has I²C stalls;
  NuttX IRQ driver ongoing.
- Phantom keys: membrane-contact physics + partly fixed by the rewrite.

Takeaways: the protocol was designed in firmware long before any Linux work;
the remaining work is host-side + one small firmware gate + one meter check.

## 6. Compat-mode & arming design (agreed)

Principle: **a flashed BIOS is electrically invisible until the host says
otherwise.**

1. Firmware: new `IRQ_LINE_ENABLE` bit (default 0). When 0: PC10 = pulled
   input, never driven; latch still fills so poll mode is unaffected.
   When 1: current BIOS behavior. *(Pending upstream.)*
2. Host activation sequence (kernel, TYP-gated):
   1. Probe: `TYP == 0xCA` and VER within supported set;
   2. DT: ball on the M_UART3 net present, remuxed GPIO input; trigger
      LEVEL (active-low pin) — pull config validated empirically (push-pull
      source; pull mostly guards power states);
   3. Write `IRQ_LINE_ENABLE`;
   4. `request_threaded_irq` + start the 1 s watchdog drain.
3. **Disarm = emergency stop**: one I²C write returns the pin to input and
   the host falls back to poll mode without reboot — the field kill-switch.
4. Tiering (auto-selected by probe; each downgrade a non-event):

| Tier | Condition | Delivery |
|---|---|---|
| 0 | stock FW (TYP=0x00) | 128 Hz poll; Option A adaptive poll available |
| 1 | BIOS, enable bit off | poll; IRQ pin silent |
| 2 | BIOS + bit + DT GPIO | level-IRQ mode (or B′ stream build) |

## 7. Firmware update plan (agreed)

Objective: **seamless firmware upgrades shipped as part of Calculinux — no
DIP fiddling, no case opening** after a one-time first install.

### 7.1 First install (one-time, switch-gated) — unavoidable

**[answer to the open question]** Stock firmware **cannot** be upgraded over
the M_UART3 pair: PC10-as-RX is not implemented in stock, and the STM32 ROM
bootloader listens only on the PA9/PA10 net — reachable solely via SEL1=B +
CH340C + USB (BIOS README procedure: DIP switch, motherboard USB,
STM32CubeProgrammer). So the *first* BIOS install walks the DIP+CH340C path,
case open, once. Document in the Calculinux docs (mirror of the BIOS flashing
page).

### 7.2 Steady state: I²C is the primary update channel (no switches)

Once any BIOS is resident, updates run over the always-available I²C bus:

- **BIOS side:** DFU state via register command; while in DFU: ignore
  `REG_ID_OFF` (§7.5), accept chunked payloads over the existing write path,
  verify (CRC), program, update boot select, reset (host-issued `RST`).
  30 KB at 400 kHz I²C is seconds — size is not a constraint.
- **Flash layout:** A/B app banks in the 64 KB (boot area + two ~28 KB
  slots); **boot selection persisted in the EEPROM** the BIOS already uses;
  version stamp; **app self-test at boot reverts on failure** — required,
  since the watchdog was removed upstream, so self-test-and-revert *is* the
  safety mechanism.
- **Host side:** a `calculinux-update` component ("kbd-stm32-fw"): read
  TYP/VER → compare to packaged build → push chunks → `REG_ID_RST` →
  re-probe → roll back on bad banner. Users see "keyboard firmware updated"
  inside the normal update flow.
- **Recovery rail** (catastrophic brick): the §7.1 DIP+CH340C procedure,
  documented.

### 7.3 UART as secondary channel

SEL1=B (CH340C → MCU USART1) stays available for low-level debug and
ROM-bootloader recovery. The un-switched M_UART3 pair is *not* an update
channel (direction/ownership per §2.1) — it is the IRQ line, full stop.

### 7.4 Updates must never regress

Console path (SEL1=A / uart0) must work after every firmware update. Smoke
test: post-reset console banner over Type-C, FIFO handshake, IRQ handshake
(§4.1), typed-character check.

### 7.5 Power-sequence traps

- MFD core `shutdown()` writes `REG_ID_OFF` (power-off the whole peripheral)
  on host reboot — an updater racing a reboot kills the session. DFU state
  must ignore OFF (or the flow holds it), and `calculinux-update` must
  serialize kbd-fw updates with reboots.
- BIOS long-press = PMU shutdown without host ACK by default (opt-in ACK mode
  per forum) — confirm DFU/host-ACK interplay with Jack.

## 8. Design options & effort

Shared precondition for all: mouse-repeat re-home (§1) + retirement of
`g_ctx`/`DEFINE_TIMER` globals + stats (irqs, drained, watchdog rescues,
i2c errors — silent IRQ loss = dead keyboard; observability is mandatory).

### Option A — adaptive slow-poll (no firmware, no hardware)

Fast poll (128 Hz) while recently active; decay to **1 Hz keepalive** when
idle (satisfies stock's 2.5 s watchdog); first FIFO hit re-arms fast mode.

- Idle traffic 128/s → 1/s (≈99 %); worst first-keystroke latency after long
  idle ≈ 1 s; ~8 ms thereafter.
- **1-2 days**, risk ≈ 0. Companion to PR #33 (same code path). Ship first
  regardless of B's fate — the Tier-0 improvement.

### Option B — level IRQ on the M_UART3 ball (recommended end state)

Requires §6 machinery + firmware gate + the meter check (§2.1).

1. DT: `interrupts-extended`/`irq-gpios` + pinctrl on the identified ball.
2. Threaded IRQ (primary NULL, `IRQF_ONESHOT`): drain via existing workqueue
   functions; **masked** `REG_ID_INT` ack (write back bits observed —
   correct on both firmwares; never blanket 0).
3. **1 s watchdog drain** always (missed pulses, NAKs, wedges; keepalive).
4. Gates: TYP + DT property + `IRQ_LINE_ENABLE` (§6).
5. PM: `enable_irq_wake()`; mask IRQ in suspend (regmap/bus frozen).
6. Emergency disarm path (§6.3).

**≈2-3 days driver/DT + 1-2 days PM/stats + 2-3 days bench/soak** = ~1.5
weeks, dominated by the physical verification and joint firmware work.

### Option B′ — UART event-stream variant (contingent alternative)

Complete the BIOS `UART_PICO_INTERFACE` path (add event TX in `key_cb()`;
init/NVIC present — a few dozen lines). MCU streams framed key events into
the Lyra's uart1 RX on the same ball; SoC wakes on UART-RX FIFO IRQ. Zero
driver-vs-driver contention (correct polarity, one-directional), "UART"
preserved in the strongest sense; costs: simplex, byte-parser in Linux.
Shares the entire Linux substrate with B; differ in ~150 lines. **Prototype
both; select by what the meter/soak says about the ball.** Relevant only if
the ball proves unsuitable for a level GPIO.

### Option C — native USB transport (parked)

Live but DIP-gated (§2.1); a CDC device (~10-16 KB, headroom exists) buys
nothing production-grade without the switch. Park.

### Rejected — UART3 crossover as host console channel

Contends with the PMU IRQ input (PC9) or HP_DET (PC11) depending on mode.
Skip.

## 9. Consolidated pitfalls register

1. **Never drive PC10 before the host remuxes the ball** (push-pull fight
   with a UART-TX-idle-high if the ball is UART-muxed) → the
   `IRQ_LINE_ENABLE` gate; BIOS-on-old-DT must stay a no-op.
2. **Ack semantics differ by firmware**: stock = full-byte replace (write 0
   clears); BIOS = masked clear, pin released on *any* write. Host must
   write back only the bits it observed — never blanket 0 — to be correct on
   both.
3. **Stock 2.5 s I²C watchdog** → reduced-poll schemes need keepalive ≥ ~1
   Hz or expect periodic `ResetI2CBus()` hiccups.
4. **INT-enable bits moved** between firmwares (stock CFG.4 @0x02 → BIOS
   INT_CFG.3 @0x12); branch on TYP, never assume.
5. **FIFO overflow drops new entries** (both FWs default); chord storms can
   silently eat releases → stuck keys. Watchdog drain narrows, doesn't
   eliminate.
6. **Backlight scale change** in BIOS (0-9, with mapping) — coarse stepping
   if driven finely.
7. **BIOS INT-ack regressed once** (re-fixed in `1b649c8`); smoke-test the
   full handshake on every flash.
8. **BIOS RTC is WIP-quality** (calendar/day-roll admitted); no scheduling
   on it yet.
9. **Audio pin reservations** (Lyra 31/32 M0 audio; RM_IO12/13 PWM audio)
   must not be touched by DT work in this area.
10. **`REG_ID_OFF` power-off on host reboot** (MFD `shutdown()`) interacts
    with the update flow (§7.5).
11. **Console path (uart0 / CH340C / SEL1=A) is sacrosanct** — verify after
    every change that could touch the connector region.
12. **UART1/SD pinmux conflict in the DTS** (§2.3) — independent bug;
    resolve before relying on either consumer.

## 10. Execution plan & verification checklist

Phases (dependencies ordered):

- **P0 — physical verification (blocks B; ~1 bench hour):**
  (a) which Lyra GPIO balls carry the M_UART3 pair (continuity, connector
  socket → Lyra); (b) SEL1/SEL2 factory positions; (c) runtime owner of
  RM_IO28/30 (`/sys/kernel/debug/pinctrl`) + SD slot working today.
- **P1 — DTS housekeeping (independent, file now):** disable/resolve the
  uart1-vs-spi1 conflict (§2.3); confirm console path untouched.
- **P2 — firmware gate (joint, small):** `IRQ_LINE_ENABLE` bit upstream
  (Jack → mirror); validate pin-idle behavior with a meter.
- **P3 — Option A** (Tier-0 win): adaptive slow-poll + repeat re-home;
  PR against this repo (pairs with #33).
- **P4 — Option B/B′** prototyped in parallel: DT + threaded IRQ (+parser for
  B′), masked ack, watchdog, stats, PM; select winner via soak.
- **P5 — update chain:** BIOS DFU state + A/B + self-revert (joint with
  Jack); `calculinux-update` "kbd-stm32-fw" component; §7.4 smoke suite.
- **P6 — (north star, parked):** U-Boot southbridge usage (wait-for-key,
  RTC display); display ambitions — after P4 soaks.

Sync list: Jack (gates, DFU, power semantics), shtirlic (NuttX IRQ consumer,
protocol edge cases), Ben/Calculinux (dtsi ownership, update integration).

## Appendix: source map

| Artifact | Location |
|---|---|
| Linux kbd driver (poll, dead IRQ scaffold) | `drivers/picocalc_mfd_kbd/picocalc_mfd_kbd.c` (main@cf9f01a) |
| Legacy kbd flavor | `drivers/picocalc_kbd/picocalc_kbd.c` |
| Register header (Linux) | `drivers/picocalc_mfd/picocalc_reg.h` |
| MFD core (regmap, child populate, `shutdown()` → `REG_ID_OFF`) | `drivers/picocalc_mfd/picocalc_mfd.c` |
| PicoCalc DT nodes | `luckfox-lyra/picocalc-luckfox-lyra.dtsi` |
| Kernel Lyra dtsi (uart1 node) | kernel `arch/arm/boot/dts/linux-rk3506-luckfox-lyra.dtsi` |
| Ball→GPIO map | kernel `arch/arm/boot/dts/rk3506-pinctrl-rmio.dtsi` (RM_IO28=GPIO1_C3, RM_IO30=GPIO1_D2) |
| U-Boot console | `uboot-rk3506-luckfox.dtsi` (`stdout-path = &uart0`); meta layer `meta-picocalc-bsp-rockchip/recipes-bsp/u-boot/files/v2026.07-rk3506/dt/rk3506-luckfox-lyra.dtsi` |
| Mainboard schematic (1 sheet) | `github.com/clockworkpi/PicoCalc` → `clockwork_Mainboard_V2.0_Schematic.pdf` (J701/U701/U702/U703 region annotated copy on file with maintainer) |
| Stock FW | `github.com/clockworkpi/PicoCalc` → `Code/picocalc_keyboard` (master+devel) |
| Alternative FW | `github.com/jackcartersmith/picocalc_BIOS`; Calculinux mirror `github.com/Calculinux/picocalc_BIOS` (in sync 2026-09-30, HEAD 1b649c8); local `/home/benklop/repos/PicoCalc/picocalc_BIOS` |
| BIOS IRQ protocol | `Core/Src/main.c` (asserts), `Core/Src/i2cs.c` (masked ack + release), `Core/Inc/hal_interface.h:109` (PC10), `Core/Src/regs.c` (TYP 0xCA, defaults) |
| BIOS host reference (poll) | `tests/pcsb/` |
| Design thread | forum.clockworkpi.com t=17292 |
| Related PRs | Calculinux/picocalc-drivers#33 (event-path restructure), #34 (this doc) |
