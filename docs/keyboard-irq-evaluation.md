# PicoCalc keyboard: interrupt-delivery evaluation

Status: research/evaluation — no code changes in this document.
Date: 2026-09-30. Line numbers below refer to `main` at `cf9f01a` unless noted.

Goal of this document: capture everything learned while evaluating a move from the
current 128 Hz I²C-poll keyboard pipeline to interrupt-driven event delivery, across
all three layers — the Linux drivers (this repo), the PicoCalc mainboard hardware,
and the two available STM32 keyboard firmwares. Everything here is intended to be
reusable for whoever implements the change.

Legend: **[verified]** = traced to source/schematic with citation,
**[assumed]** = inferred, worth confirming, **[open]** = unresolved, blocking or
informative.

## 1. Current Linux-side architecture (this repo)

The MFD keyboard driver (`drivers/picocalc_mfd_kbd/picocalc_mfd_kbd.c`) runs a fixed
pipeline:

- Soft timer at `HZ/128` (≈7.8125 ms → 128 Hz), `kbd_timer_function()` **L577-582**.
  Each expiry `schedule_work`s on `g_ctx->work_struct`.
- `input_workqueue_handler()` (**L515**) drains the device FIFO via
  `input_fw_read_fifo()` (**L225-277**): repeated 2-byte `regmap_bulk_read` of
  `REG_ID_FIF (0x09)` until a zero terminator; up to `KBD_FIFO_SIZE` (31) items.
- Each FIFO item → `key_report_event()` (**L279**), which maps HID scancodes to
  Linux keycodes and implements shift tracking, the dual-shift mouse-mode toggle,
  mouse direction-flag tracking, and second-key (F6..F10, Break/Home/End/PageUp/
  PageDown/Ins) emulation.
- **Idle cost: ~128 I²C transactions/sec + 128 SoC wakeups/sec, permanently.**

Hidden coupling — the mouse-mode repeat engine rides on the 128 Hz heartbeat:
while `mouse_move_dir` is armed and `mouse_mode` is on, *every* tick emits
`REL_X/REL_Y` with a 1×/2×/4× hold-duration ramp (**L529-561**). Any redesign that
decouples event intake from the heartbeat must re-home this repeater or mouse mode
loses its cadence.

Known debt touched by this topic:

- File-global `g_ctx` and `DEFINE_TIMER(g_kbd_timer)` (**L575, L613, L727**) —
  single-instance assumptions, awkward for IRQ descriptor ownership.
- Dead IRQ scaffolding already written but disabled:
  - `devm_request_threaded_irq(&i2c_client->dev, i2c_client->irq, NULL,
    input_irq_handler, IRQF_SHARED | IRQF_ONESHOT, …)` — **L680-703** (references a
    handler that no longer exists).
  - `kbd_write_i2c_u8(ctx, REG_ID_INT, 0)` after drain — "clear client interrupt
    flag", **L568-572**.
  The drain code itself is thread-safe (no locking; already used from workqueue
  context), so a threaded IRQ handler can reuse `input_fw_read_fifo()` and
  `key_report_event()` verbatim.
- `luckfox-lyra/picocalc-luckfox-lyra.dtsi`: the `picocalc-mfd@1f` node has **no
  `interrupts`/`irq-gpios` property**; pinctrl reserves Lyra 31/32 (GPIO4_B2/B3)
  for M0 audio and RM_IO12/13 for the PWM-audio overlay — both are spoken for.

The legacy `drivers/picocalc_kbd/` flavor is architecturally identical (same FIFO
protocol, same workqueue/timer shape) and would inherit any shared refactor.

## 2. The mainboard hardware (what can carry an IRQ)

Source: `clockwork_Mainboard_V2.0_Schematic.pdf` (sheet 1 of 1, "clockwork
Mainboard V2.0", 2024-12-20) in
`https://github.com/clockworkpi/PicoCalc`.

The keyboard MCU is an **STM32F103R8T6** (CKS32F103Rx), I²C slave at 0x1F. Full
audit of its external nets **[verified]**:

| Net (MCU side) | MCU pin | Role |
|---|---|---|
| `M_I2C1_SDA` / `M_I2C1_SCL` | PD0/PD1 area | Keyboard I²C bus → mainboard ("I2C1") |
| `M_I2C2_SDA` / `M_I2C2_SCL` | PB11/PB10 | AXP2101 PMU bus |
| `M_UART3_TX` | PC9 | "spare UART" line 1 — consumed as **PMU IRQ input** by both firmwares |
| `M_UART3_RX` | **PC10** | "spare UART" line 2 — the only candidate IRQ carrier (see §4) |
| `HP_DET` | PC11 | Headphone detect to mainboard (R201, 1.2 kΩ) |
| `M_USB_DP/DM` | PA11/PA12 | Native USB, inactive in both firmware trees; also feeds CH340C debug converter |
| power/chg/button nets | — | CHGLED, PWR_OK, power button → mainboard |
| key matrix ROW1-8/COL1-8, membrane pads M11-M78, KEY1-12 | internal | not brought out |

The Pico side of the mainboard connector is labelled with PicoMite GPIO numbers
(`GP2/GP3` = UART0 RX/TX, `GP4/GP5` = M_UART1 RX/TX, `GP21/GP28` = M_USB DP/DM,
`GP14-17` = display SPI/DC/RST on the 20-pin connector). **[open]** The Luckfox
Lyra mates with the same mainboard connector (all Calculinux wiring goes through
it), but the Lyra-side GPIO assignment for the PC10 (`M_UART3_RX`) net is not
documented anywhere we found — this is **the single remaining hardware unknown**.
Candidate ways to resolve it: (a) continuity meter from the connector socket to
Lyra pins, (b) Luckfox Lyra board pinout/schematic, (c) probe behaviour.

**[verified]** No dedicated kbd-INT net exists between MCU and mainboard: every
MCU output is accounted for above, and none is labelled as an interrupt.
Corroboration: on the original PicoCalc, the IRQ's intended consumer was the
PicoMite — Jack's forum post (below) states "the only connections between the pico
board and the stm32 are the I2C and the UART bus… I was thinking of reconditioning
it as IRQ_pin".

## 3. Stock firmware: `clockworkpi/PicoCalc` — `Code/picocalc_keyboard`

([GitHub](https://github.com/clockworkpi/PicoCalc/tree/master/Code/picocalc_keyboard),
checked `master` and `devel`). Arduino-core, busy-loop design.

Protocol basics **[verified]**:

- Registers per `reg.h`: `REG_ID_TYp 0x00` (0x00 = official), VER 0x01, CFG 0x02,
  **INT 0x03**, KEY 0x04, BKL 0x05, DEB 0x06, FRQ 0x07, RST 0x08, FIF 0x09, BK2
  0x0A, BAT 0x0B, C64_MTX 0x0C, C64_JS 0x0D, OFF 0x0E. Writes flagged by MSB.
- Key events are debounced/scanned in `loop()` (10 ms idle delay) and enqueued into
  a 31-slot FIFO; host reads 2-byte items until 0x00.
- INT status register: `key_cb()` latches `INT_KEY` per event when
  `CFG_KEY_INT` is set; `lock_cb()` latches INT_CAPSLOCK/INT_NUMLOCK; overflow
  latches INT_OVERFLOW. **Defaults: `CFG_OVERFLOW_INT | CFG_KEY_INT |
  CFG_USE_MODS | CFG_REPORT_MODS`** (`reg_init()`), i.e. latching is ON out of the
  box. Overflow policy defaults to **drop-new-entry** (`CFG_OVERFLOW_ON` unset).
- Ack convention: the whole INT byte is replaced by a host write, so **writing
  0x00 clears all bits**.
- I²C watchdog: if no I²C traffic in **2.5 s the firmware resets its I²C
  peripheral** (`ResetI2CBus()`, `loop()`). Hosts must keep the bus alive with
  ≤ ~2 s spacing in any reduced-poll scheme.

Interrupt-pin status: **implemented in spirit, absent in silicon use**
**[verified]**:

- `INT_DURATION_MS = 1` defined (`conf_app.h`) but unused.
- The only pin-pulse code is **commented out** in `lock_cb()`
  (`// int_pin can be a LED`, 1 ms pulse) and `int_pin` is **declared nowhere** —
  same on `devel`. Clockwork's own README comment admits the official firmware
  "have a draft for that feature, but not implemented in final (probably because
  there is no program space left…)" (forum quote, §5).

Net: on stock hardware + stock firmware, a true IRQ line does not exist. The
register latches exist but need a poller — they are an optimization hint, not a
delivery channel.

## 4. Alternative firmware: `jackcartersmith/picocalc_BIOS` (mirrored at
`Calculinux/picocalc_BIOS`)

Local: `/home/benklop/repos/PicoCalc/picocalc_BIOS`; the Calculinux mirror was
verified **0 commits behind upstream** on 2026-09-30 (HEAD `1b649c8`
"IC2S: readded missing REG_ID_INT in callback handler" — an ack-path regression
had to be re-fixed, so exercise the INT write path during bring-up).

A full HAL/CubeMX rewrite ("personal rewrite of the original PicoCalc STM32
firmware… more efficient functionally and electrically").

Implemented IRQ protocol **[verified]** — this is the headline finding:

- `PICO_IRQ` = **GPIOC pin 10 (PC10)**, push-pull output, **active-low**
  (`Core/Inc/hal_interface.h:109-110`).
- Asserted (driven LOW) from `key_cb()` on any key event when
  `INT_KEY` ∈ `REG_ID_INT_CFG`, from `lock_cb()` on lock toggles, and on FIFO
  overflow / RTC alarm (`Core/Src/main.c` ~L342-401). Guarded by
  `#ifndef UART_PICO_INTERFACE`.
- De-asserted exclusively by the host's I²C write to `REG_ID_INT`: the handler
  applies a **masked clear** — `reg_set_value(REG_ID_INT, old & ~written_byte)` —
  then raises the pin (`Core/Src/i2cs.c` REG_ID_INT branch). Note: the pin is
  raised on *any* write to that register, mask or not, so the line is best treated
  as "event pending → go drain", with the latch as advisory and the FIFO as the
  authoritative backlog. (The masked ack is strictly nicer than stock firmware's
  full-byte-replace — no torn-status window if the host writes a partial mask.)
- So the complete handshake is: *event → pin LOW → host IRQ → drain FIFO → write
  `REG_ID_INT` ack → pin HIGH*.

Register-map extensions (host must branch on firmware type) **[verified]**:

- `REG_ID_TYP = 0xCA` ("That's me :3", `Core/Src/regs.c` ~L91); stock reports
  0x00. **This is the readiness gate for IRQ mode** — the design Jack and Ben
  agreed on in the forum thread (vendor-id-style provenance probe).
- `SYS_CFG 0x02` (successor of CFG) and a separate **`INT_CFG 0x12`** — the stock
  `CFG_KEY_INT` (bit 4 of 0x02) moves to bit 3 of 0x12. Kernel code that wants to
  *ensure* INT enablement must write different registers per TYP.
- `REG_ID_DEB` write is a 16-bit hold-period (`keyboard_set_hold_period`); reads
  return 2 bytes.
- Full RTC exposure: `REG_ID_RTC_CFG/DATE/TIME/ALARM_DATE/ALARM_TIME` backed by
  the STM32's internal RTC (backup-domain registers persist across deep-sleep
  standby). Alarm "only trigger[s] the IRQ signal (no wake-up)" w.r.t. the host.
  Known warts per author: calendar/day-roll issues, sleep/date retention — WIP.
- Backlight scale changed to 0-9 steps; out-of-range writes (our driver writes
  0-255, `default-brightness = <128>`) are accepted and linear-mapped for driver
  compatibility.

Power story (why this firmware also matters beyond IRQs) **[claimed/verified-in-code]**:
~3.5 mA run, < 0.1 mA standby (PMU left alive, STM32 asleep; RTC keeps running),
persistent settings in emulated EEPROM (flash), power-button semantics (short
press w/ Shift = pico reset; long press = PMU shutdown), I²C re-arm/error handling
rewritten ("I2C: arch review and speed testing"). Author reports stock-firmware
I²C stalls that this rewrite fixes (forum, Aug 2025).

Alternate transport: `UART_PICO_INTERFACE` build flag configures **USART3 on
PC10(TX)/PC11(RX), 115200 8N1** (note the PC11 swap — stock firmware uses PC11 as
HP_DET) and suppresses the GPIO IRQ path. Effect/usage: not clearly consumed by
any driver yet; treat as experimental. **[open]** whether this mode transmits a
key-event stream (grep finds no event TX path in `Core/Src` outside DEBUG huart1)
or is scaffolding.

Host-side reference driver: `tests/pcsb/` (Pico SDK / RP2040) — deliberately
poll-based ("works with this and the original firmware"), so it is **not** an IRQ
consumer reference. **[open]** no public IRQ-consumer reference exists for the
Linux side; ours would be the first.

## 5. Prior art and intent (forum evidence)

Thread: [Custom PicoCalc BIOS/keyboard firmware](https://forum.clockworkpi.com/t/custom-picocalc-bios-keyboard-firmware/17292)
(May 2025 – ongoing; Ben (maintainer) participates).

- shtirlic's original ask (the motivation for this whole effort): "IRQ for keyboard
  events instead of i2c constant polling, will save a lot of CPU(power) — we can
  use pins from **spare second UART already connected to stm32 and pico**. Second
  pin for rtc?" and a proposal to use pin #1 for keyboard IRQ, pin #2 for
  misc. southbridge events, plus the 7-bit command-register taxonomy
  (reset/shutdown/power-sequence controls).
- Jack's constraint admission: "the pico pin to be used seem to be the issue for
  me right now, the only connections between the pico board and the stm32 are the
  I2C and the UART bus. As I didn't plan to use this UART, I was thinking of
  **reconditioning it as IRQ_pin**… And can only be de-asserted by reading the
  IRQ registers/FIFO." — and "the official firmware have a draft for that
  feature, but not implemented in final (probably because there is no program
  space left…)". Roadmap at the time: missing official registers, RTC, "**I2C IRQ
  mode** (+docs)", power modes.
- shtirlic (Aug 2025): running the BIOS on a real unit, "working great and pretty
  stable", stock 1.2 has "i2c got stuck" issues; interrupt experiments not yet
  started by him; maintaining a custom NuttX driver with IRQ support for
  PicoMite-class hosts.
- Phantom-key discussion: partly membrane-switch contact physics ("click but
  nothing registered" off-centre), partly addressed by the rewrite
  ("reduced — probably wiped out — duplicate key press").

Takeaways: (a) the IRQ line's intended carrier is the "spare UART" pair, exactly
matching the schematic audit (§2); (b) the protocol (latch + masked-ack + level
line) was designed and shipped in firmware years before any Linux work — the
remaining work is squarely on the Lyra/host side; (c) there is community interest
and at least one other maintainer (NuttX) working the same problem, worth syncing
with.

## 6. Design options (ranked) and effort

### Option A — Adaptive slow-poll (no firmware, no hardware)

Fast poll (128 Hz) while recently active; decay to a **1 Hz keepalive** when idle
(also satisfies stock firmware's 2.5 s watchdog); first FIFO hit re-arms fast
mode. Optionally consult `REG_ID_INT` as a dirty hint in BIOS builds.

- Idle traffic: 128 txns/s → 1 txn/s (≈99 % less); idle wakeups likewise.
- Worst first-keypress latency after long idle ≈ 1 s; subsequent latency ~8 ms.
- Requires the mouse-repeat re-home (shared with B) — the one genuinely novel
  piece: a repeating `delayed_work` at the current ~7.8 ms period, 1×/2×/4×
  ramp, armed by `mouse_move_dir`.
- Effort: **1–2 days** driver work + testing. Risk ≈ 0. Candidate companion to
  PR #33 (which restructured exactly this code path).
- Downside: still polling; latency floor is structural.

### Option B — True IRQ (BIOS firmware + Lyra GPIO) — recommended end state

Requires: `Calculinux/picocalc_BIOS` flashed (TYP 0xCA) and the PC10/`M_UART3_RX`
net terminating on a usable Lyra GPIO (the one open question, §2). No PCB mod is
needed if that net reaches the Lyra — the "spare UART" line is factory-routed.

Kernel/DT work (the dead scaffolding at kbd L680-703 shows the intended shape):

1. DT: `interrupts-extended`/`irq-gpios` + pinctrl group (pull-down recommended:
   line is push-pull active-low from the MCU, but keep the input clean if the
   firmware is asleep/resetting — actually with push-pull this mostly guards
   against floating states on power sequences; validate polarity empirically).
2. Threaded IRQ (primary handler NULL, `IRQF_ONESHOT`): drain FIFO via the
   existing workqueue functions, then `REG_ID_INT` masked ack (write the bits
   observed, not 0, so lock/alarm bits from other consumers are respected).
3. **1 s watchdog drain** regardless — catches missed pulses, I²C NAKs, firmware
   wedges; keepalive for stock firmware stays compatible if we ever run it.
4. Gate: IRQ mode only when `REG_ID_TYP == 0xCA` AND the DT property is present;
   otherwise transparent polling fallback. (Matches the vendor-id probe the
   forum settled on.)
5. Hygiene: retire `g_ctx`/`DEFINE_TIMER` globals; stats (irqs, drained, watchdog
   rescues, i2c errors) — silent IRQ loss = dead keyboard, observability is
   mandatory.
6. PM: `enable_irq_wake()`; mask IRQ in suspend (regmap unusable while the bus
   freezes).
7. Mouse-repeat re-home (shared with A).

Effort: DT+driver 2–3 days, repeat engine/PM/stats 1–2 days, bench + soak 2–3
days → **~1.5 weeks** assuming the Lyra-side pin is usable. Dominant risk: the
§2 open question (verify on metal before writing code — continuity/probe first).

### Option C — Native USB HID firmware (shelved)

MCU native USB (PA11/PA12) *is* routed to the mainboard connector
(`M_USB_DP/DM`, GP21/28), so a USB-device rewrite would be the architecturally
clean endgame, but it is a ground-up firmware project (descriptors, enumeration,
power sequencing) with no community momentum for it on the Lyra side. Park.

### Variant considered and rejected — reuse UART3 crossover

Electrically present (the "spare UART" pair) and would allow SoC UART-RX-IRQ
delivery without an IRQ line — but on stock firmware the pair's TX leg (PC9) is
the AXP2101 PMU interrupt input, and the BIOS's `UART_PICO_INTERFACE` mode swaps
PC11 (stock: HP_DET). Either way another function dies. Skip; prefer the GPIO
role the BIOS already implements.

## 7. Consolidated pitfalls register

1. **Stock-firmware 2.5 s I²C watchdog** → any reduced-poll scheme needs a
   keepalive ≥ ~1 Hz, or expect periodic `ResetI2CBus()` hiccups.
2. **Ack race semantics differ by firmware**: stock = full-byte replace
   (write-0 clears); BIOS = masked clear. Host should write-back the bits it saw
   (never a blanket 0) to be correct on both — on stock, that equals an
   effectively-equivalent clear of the same bits.
3. **The pin is released on any `REG_ID_INT` write in the BIOS**, even mask-less;
   treat the line as edge/event-pending, not as a persistent level.
4. **INT-enable bits moved** between firmwares (CFG.4 @ 0x02 → INT_CFG.3 @
   0x12); do not assume stock layout behind a TYP check.
5. **FIFO overflow drops new entries by default** in stock; BIOS inherits the
   `CFG_OVERFLOW_ON` knob (in SYS_CFG). Chord storms > 31 events can silently eat
   releases → stuck keys; the watchdog drain narrows but does not eliminate it.
6. **Backlight scale change** in the BIOS (0-9 vs 0-255 with mapping) — coarse
   stepping becomes perceptible if we ever drive it finely.
7. **Latest BIOS commit (`1b649c8`) restored the INT ack path** — regression
   happened once already; smoke-test the full handshake on first flash.
8. **RTC in the BIOS is WIP-quality** (day-roll/calendar issues admitted); do not
   build scheduling on it yet.
9. **Audio pin reservations** (Lyra 31/32 M0 audio; RM_IO12/13 PWM audio) must not
   be touched by any DT pinctrl work in this area.
10. Legacy `picocalc_kbd` flavor exists for completeness; keep behavior aligned if
    the shared path is refactored (it has its own workqueue/timer twin).

## 8. Immediate next steps

1. **Close the §2 unknown on metal**: identify the Lyra GPIO on the mainboard
   connector that carries the PC10 / `M_UART3_RX` net (continuity from the
   connector socket; or obtain the Lyra pinout). Everything else is decided.
2. Flash the Calculinux-mirror BIOS on a bench unit; verify TYP=0xCA, FIFO
   handshake, and the IRQ pin pulsing with a logger (pin to multimeter/GPIO
   sampler).
3. Ship **Option A** as a regular PR (pair with the mouse-repeat re-home;
   PR #33 already restructured the event path it must hook).
4. Prototype **Option B** behind the TYP+DT gate using §6-B; keep polling
   default until B soaks on at least one daily driver.
5. Sync with shtirlic (NuttX IRQ consumer) on protocol edge cases — his driver
   is the only known IRQ consumer in the wild.

## Appendix: source map

| Artifact | Location |
|---|---|
| Linux kbd driver (poll, dead IRQ scaffold) | `drivers/picocalc_mfd_kbd/picocalc_mfd_kbd.c` (this repo, main@cf9f01a) |
| Legacy kbd flavor | `drivers/picocalc_kbd/picocalc_kbd.c` (same repo) |
| Register header (Linux) | `drivers/picocalc_mfd/picocalc_reg.h` |
| MFD core (regmap, child populate) | `drivers/picocalc_mfd/picocalc_mfd.c` |
| DT nodes | `luckfox-lyra/picocalc-luckfox-lyra.dtsi` |
| Stock FW | `github.com/clockworkpi/PicoCalc` → `Code/picocalc_keyboard` (master+devel) |
| Schematic (sheet 1/1) | same repo, `clockwork_Mainboard_V2.0_Schematic.pdf` |
| Alternative FW | `github.com/jackcartersmith/picocalc_BIOS`; Calculinux mirror `github.com/Calculinux/picocalc_BIOS` (in sync as of 2026-09-30, HEAD 1b649c8) |
| BIOS IRQ protocol | `Core/Src/main.c` (asserts), `Core/Src/i2cs.c` (masked ack + de-assert), `Core/Inc/hal_interface.h:109` (PC10) |
| BIOS host reference (poll) | `tests/pcsb/` in the BIOS repo |
| Design thread | forum.clockworkpi.com t=17292 |
| Stale-flag fix (event path restructure) | PR Calculinux/picocalc-drivers#33 |
