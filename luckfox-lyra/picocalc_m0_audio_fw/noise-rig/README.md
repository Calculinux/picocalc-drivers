# Recording the M0 audio noise

How the SD-read noise was measured on 2026-10-11 (see `docs/m0-audio.md`, "What the
noise is"). The ear cannot rank noises this close; a recording can.

Setup: the PicoCalc's headphone out into a line input (a USB recorder here, a
Tascam DR series as `hw:2,0`, S24_3LE 48 kHz stereo), on battery, input level set so
a 1 kHz tone at -20.8 dBFS in the stream reads about -12 dBFS. Record with
`arecord -D hw:2,0 -f S24_3LE -r 48000 -c 2 -d SECONDS file.wav` and start the
board's run a few seconds later.

- `recsess-example.sh`: one run on the board. Each condition is a stream of 1..N
  beeps (1 kHz, 0.15 s) then 12 s of digital silence, with the load started after the
  beeps. It needs the beep files `rec/qN.raw` (S16_LE stereo 48 kHz) next to it; make
  them with a few lines of Python. Settings, throughput and the driver's "behind"
  figure go to a log.
- `analyze_rec.py REC.wav`: finds the beep groups, and reports for the 10 s after
  each: total and A-weighted level, five bands, burstiness, and a spectra plot. Levels
  are relative: compare conditions with each other.
- `blast.py HZ [CHUNK]`: SPI1 clocking all-ones through spidev (unbind `mmc_spi`,
  `driver_override` spidev), a load that needs no card reads.
- `lsperi.py DIV`: set the low-speed peripheral register bus live (`/dev/mem`):
  bus = 375 MHz / (DIV + 1).
- `padmux.py MODE`: connect or disconnect the SPI1 pads (IOMUX to GPIO).
- `spislew.py LEVEL`, `audiopad.py DRIVE SLEW`: pad slew and drive of SPI1 and of the
  audio pins.
