# Model of the corrected loop's timing error (not firmware)

`sim.c` is the second-order modulator of play.S with silence in, a pin that
follows the write's issue time on the 10 MHz (37.5-cycle) grid, stalls inside
the write that do not move the pin, and four ways of measuring a level's length:

    gcc -O2 -o sim sim.c -lm
    ./sim N p hmax stall pstall mode x_true x_est seed [tick catchup back] ; python3 psd.py   # noise, dB re a full-scale sine, 20 Hz-20 kHz

mode 0 none, 1 count after the write (the loop before issue-time), 2 count
before it (issue time), 3 issue time plus a model of the grid with phase x_est.
Results (N = 6e6 ticks; "reads-like": p 0.1, hmax 40, writes stalled up to 60 cycles 27 % of the time):

| | silence | reads-like |
|---|---|---|
| none | -47.0 | -30.4 |
| count after the write | -47.0 | -27.6 |
| count before the write | -47.0 | -34.3 |
| + grid model, x right | -77.5 | -75.5 |
| + grid model, x 13 cycles wrong | -20.7 | -29.7 |

The jack tip measures about -40 dB re a full-scale sine while the card is read.
The grid model needs x (the grid's phase against SysTick, new at every stream
start) right to a few cycles: a wrong one is worse than none. This is a model
with the lateness profile made up to look like the diagnostic's; it says what
the quantisation costs, not what the board does.

Tick length (128 against 150 cycles = four grid steps = 2.5 MHz; catch-up per
tick 14 against 36, window lower bound 28 against 40; reads-like as above):

| | 128 | 150 |
|---|---|---|
| silence, any mode | -47.0 | no noise (the grid steps repeat exactly) |
| reads, none | -30.4 | -32.5 |
| reads, count after the write | -27.6 | -30.3 |
| reads, count before the write | -34.3 | -36.7 |
| reads, + grid model, x right | -75.5 | -77.2 |
| reads, + grid model, x 13 cycles wrong | -29.7 | -33.0 |

With a 150-cycle tick the lower window bound has to follow the extra spare
cycles: with 28 the catch-up of a late write aliases and the correction is
worse than none.
