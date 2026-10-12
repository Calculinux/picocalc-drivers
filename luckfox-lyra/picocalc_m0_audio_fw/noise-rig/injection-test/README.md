# Injection test: how well does the audio loop's lateness correction cancel a known timing error?

`inject-*.patch` (against this firmware directory, 2026-10-12) build a test firmware (`make INJECT=1`): the corrected
loop runs at a 256-cycle tick (`M0_COMP_LOG2_CYCLES 8`, window widened to +-128 cycles with `M0_COMP_BACK 128`; with the
production value 28 the window is too narrow for the early interval that follows a delayed write when the loop has
~147 spare cycles), and on a pseudo-random subset of plain ticks the pin write is delayed by a known number of cycles.
`inject.py LOOPS MASK MODE` sets it from user space (delay = LOOPS x 4 cycles, inject when `lfsr & MASK == 0`,
MODE 0 every write / 1 only writes with B2 high / 2 only writes with B2 low); `inject.py count` gives the number of
injections of the last stream. Run it with `comp=N` and `tick_hz=1464844` (256 cycles: corrected) or `1456311`
(258 cycles: uncorrected). `injrun.py` / `injsweep2.py` measure the jack-tip noise (scope CH4, AC, 20 MHz) for each point.
Results: docs/m0-audio.md. Do not ship this build: `/lib/firmware` is on a persistent overlay, restore the stock ELF.
