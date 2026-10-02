// SPDX-License-Identifier: GPL-2.0
/*
 * PicoCalc M0 delta-sigma audio driver
 * Uses RK3506 Cortex-M0 core to drive GPIO4_B2/B3 from a shared ring buffer.
 * The header and ring live in system SRAM next to the M0 firmware image (the
 * node memory-region points at), so the M0 never reads DDR while playing.
 * One output bit per SysTick tick on the M0; this driver tells the firmware
 * how many core clock cycles a tick is and how many ticks a sample lasts.
 *
 * The firmware is booted once, at the first stream, and left running: in TCM
 * mode it cannot be loaded a second time before the next reboot. Between
 * streams it idles at a slow tick. Streams are handed over through the
 * header: wait for m0_state == IDLE, fill the header in, ctrl = PLAY; to end
 * one, ctrl = STOP and the firmware returns to IDLE.
 */

#include <asm/barrier.h>
#include <asm/div64.h>
#include <linux/clk.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>

/* Must match picocalc_m0_audio_fw/shmem.h and main.c fixed config */
#define M0_AUDIO_MAGIC    0x4D304431U
#define M0_CTRL_PLAY      (1u << 0)
#define M0_CTRL_STOP      0u
#define M0_STATE_IDLE     0x49444C45U  /* "IDLE", written by the firmware */
#define M0_STATE_PLAY     0x504C4159U  /* "PLAY" */
#define M0_FMT_U8         0
#define M0_FMT_S16_LE     1
#define M0_HEADER_SIZE    64
/* Fixed M0 config: ALSA does SRC to this rate; ring size must match firmware */
#define M0_FIXED_SAMPLE_RATE_HZ  48000U
#define M0_FIXED_BUF_SIZE        8192U
/* The firmware gives each step of its per-sample work a tick of its own */
#define M0_MIN_TICKS_PER_SAMPLE  19U
#define M0_FLAG_NO_INTERP        1U
#define M0_FLAG_COMP             2U
#define M0_MAX_TICK_CYCLES       (1U << 24)  /* SysTick is 24 bits */

/*
 * Output bit rate. Higher is better audio (about 12 dB per doubling) as long
 * as the M0 finishes a tick's work in time, which depends on the core clock
 * and on whether the firmware runs as TCM (docs/m0-audio.md has measured
 * figures). The default is safe for bus mode; the m0-audio overlay, which
 * selects TCM, raises it with tick-rate-hz. A firmware built with
 * PROFILE=1 reports the worst tick and the number of overruns, printed here
 * when a stream ends. Takes effect at the next playback start.
 */
static unsigned int tick_hz = 1000000;
module_param(tick_hz, uint, 0644);
MODULE_PARM_DESC(tick_hz, "M0 output bit rate in Hz (default 1000000)");

/*
 * The GPIO block the M0 writes its pins through is clocked at half the M0's
 * core clock, so a pin can only change on every second core cycle. With an
 * odd number of core cycles per tick every other bit comes out a core cycle
 * late, and a regular error like that brings the modulator's ultrasonic noise
 * down into the audio band (in simulation: from -79 to -40 dBFS at 63
 * cycles). So the tick is made an even number of cycles. Off: the nearest
 * number, even or odd, to hear the difference. Next playback start.
 */
static bool tick_even = true;
module_param(tick_even, bool, 0644);
MODULE_PARM_DESC(tick_even, "Round the tick to an even number of core clock cycles (default on)");

/*
 * Between two samples the firmware moves its modulator input towards the
 * new one a little every tick, instead of holding each sample for its whole
 * length. That takes the images of the signal around multiples of the sample
 * rate down by 15 to 30 dB; the noise in the audio band is the same. Off:
 * hold, to compare. Next playback start.
 */
/*
 * The M0's pin writes wait while this CPU's SPI controllers or GPIO writes
 * have the peripheral bus: a redraw of the display makes a quarter of them
 * late by up to 0.4 us, which is heard as a click per redraw. With comp the
 * firmware times every pin write and puts what a late one cost back into the
 * modulator, which moves the error out of the audio band. The price is a
 * longer tick (the pin write has to be waited for), so the corrected mode has
 * its own rate, tick_hz_comp (comp-tick-rate-hz in the device tree). Both
 * take effect at the next playback start.
 */
static bool comp = true;
module_param(comp, bool, 0644);
MODULE_PARM_DESC(comp, "Correct for pin writes delayed by other bus traffic (default on)");

static unsigned int tick_hz_comp = 1000000;
module_param(tick_hz_comp, uint, 0644);
MODULE_PARM_DESC(tick_hz_comp, "M0 output bit rate in Hz with comp on (default 1000000)");

static bool interp = true;
module_param(interp, bool, 0644);
MODULE_PARM_DESC(interp, "Interpolate between samples (default on)");

/*
 * When the ring is written. The M0 touches the shared SRAM in the first few
 * ticks of every sample and nothing but its pins in the rest. Our writes to
 * that SRAM hold its accesses up (our reads do not), by enough to make the
 * tick late: a click, even in silence. So while a stream plays we watch for
 * read_idx to change, which the firmware does at the end of its per-sample
 * work, and write only in the quiet ticks that follow, a part of the period
 * per sample. Turning this off writes the whole period at once, wherever in
 * the sample that falls. Can be changed while playing.
 */
static bool ring_sync = true;
module_param(ring_sync, bool, 0644);
MODULE_PARM_DESC(ring_sync, "Write the ring only between the M0's own accesses to it (default on)");

/* Ticks at the start of a sample in which the firmware uses the SRAM: six
 * (read_idx is written in the sixth), and three more in a PROFILE build,
 * which we cannot tell apart from here. play.h: M0_BUS_STEPS. */
#define M0_STEP_TICKS     6U
#define M0_PROFILE_TICKS  3U
#define M0_SAMPLE_NS      (NSEC_PER_SEC / M0_FIXED_SAMPLE_RATE_HZ)
/* Words written between two looks at the clock */
#define M0_SYNC_BURST     32U

struct m0_audio_shmem {
	volatile uint32_t magic;
	volatile uint32_t ctrl;
	volatile uint32_t write_idx;
	volatile uint32_t read_idx;
	volatile uint32_t m0_state;      /* written by the M0: M0_STATE_* */
	volatile uint32_t buf_size;
	volatile uint32_t sample_rate;
	volatile uint32_t channels;
	volatile uint32_t format;
	volatile uint32_t flags;   /* M0_FLAG_* */
	volatile uint32_t tick_cycles;   /* core clock cycles per tick */
	volatile uint32_t ticks_base;    /* ticks per sample: whole part ... */
	volatile uint32_t ticks_frac;    /* ... and fraction, in 2^-32 tick */
	volatile uint32_t _reserved;
	volatile uint32_t stat_min_cvr;  /* PROFILE firmware: see m0_report_profile() */
	volatile uint32_t stat_overruns;
	uint8_t           buffer[];
};

static const struct of_device_id picocalc_snd_m0_dt_ids[] = {
	{ .compatible = "picocalc,snd-m0", },
	{ }
};
MODULE_DEVICE_TABLE(of, picocalc_snd_m0_dt_ids);

struct picocalc_m0 {
	struct platform_device *pdev;
	struct snd_card *card;
	struct snd_pcm_substream *substream;
	struct rproc *rproc;
	struct clk *core_clk;      /* hclk_m0: what the M0's SysTick counts */
	uint32_t tick_cycles;      /* as last given to the firmware */
	bool comp;                 /* and the mode it was worked out for */
	struct m0_audio_shmem *shmem;
	void *shmem_virt;  /* device mapping of the SRAM: header and ring */
	size_t shmem_size;
	uint32_t buf_size;
	spinlock_t lock;
	struct hrtimer timer;
	ktime_t period_ktime;
	uint32_t last_read_idx;
	/* All three count frames in appl_ptr's wrap domain (runtime->boundary) */
	snd_pcm_uframes_t copied_frames; /* handed to the M0 ring */
	snd_pcm_uframes_t played_frames; /* consumed by the M0: the hw pointer */
	snd_pcm_uframes_t elapsed_mark;  /* played_frames at the last period_elapsed */
	bool running;
	bool want_play;
	bool rproc_up;             /* firmware booted; stays up until remove */
	/* Frames played against the clock, between the first and the last timer
	 * callback of a stream: see m0_report_pace() */
	bool pace_valid;
	u64 pace_t0, pace_t1;      /* CLOCK_MONOTONIC_RAW, ns */
	u64 pace_frames;
	snd_pcm_uframes_t pace_mark; /* played_frames when last added to pace_frames */
	/* ring_sync: from read_idx changing, when writing may start and must end */
	uint32_t sync_skip_ns, sync_len_ns;
	uint32_t stat_windows;     /* samples whose quiet ticks were written in */
	uint32_t stat_unsynced;    /* ring updates written without waiting */
	uint32_t stat_update_ns;   /* longest ring update */
	struct work_struct rproc_work;
};

static struct picocalc_m0 *g_m0;

static uint32_t m0_ring_space(uint32_t write_idx, uint32_t read_idx, uint32_t buf_size)
{
	if (write_idx >= read_idx)
		return buf_size - (write_idx - read_idx) - 1;
	return read_idx - write_idx - 1;
}

/*
 * Caller holds m->lock. A tick is a whole number of core clock cycles as
 * close to tick_hz as that allows; a sample then lasts hclk / (cycles * rate)
 * ticks, which the firmware realises as base ticks plus one more whenever
 * the fraction, accumulated in 32 bits, carries. Left at zero (firmware
 * defaults) if the core clock is unknown or the request cannot be met.
 */
static void m0_set_tick_timing_locked(struct picocalc_m0 *m)
{
	unsigned long hclk = m->core_clk ? clk_get_rate(m->core_clk) : 0;
	bool with_comp = READ_ONCE(comp);
	uint32_t hz = with_comp ? READ_ONCE(tick_hz_comp) : READ_ONCE(tick_hz);
	uint32_t cycles = 0, den = 0, base = 0;
	u64 den64;

	if (hclk && hz) {
		if (READ_ONCE(tick_even))
			cycles = 2 * DIV_ROUND_CLOSEST(hclk, 2 * (unsigned long)hz);
		else
			cycles = DIV_ROUND_CLOSEST(hclk, hz);
		den64 = (u64)cycles * M0_FIXED_SAMPLE_RATE_HZ;
		if (cycles >= 2 && cycles <= M0_MAX_TICK_CYCLES && den64 <= U32_MAX) {
			den = den64;
			base = hclk / den;
		}
	}
	if (base < M0_MIN_TICKS_PER_SAMPLE) {
		if (hclk)
			dev_warn(&m->pdev->dev,
				 "tick_hz %u not usable with a %lu Hz core clock, using firmware defaults\n",
				 hz, hclk);
		cycles = den = base = 0;
	}
	m->tick_cycles = cycles;
	m->comp = with_comp;
	m->sync_skip_ns = m->sync_len_ns = 0;
	if (cycles) {
		/* Core cycles to ns; a tick is at most 2^24 cycles */
		uint32_t tick_ns = div_u64((u64)cycles * NSEC_PER_SEC, hclk);
		uint32_t guard_ns = tick_ns + 300;

		/* read_idx is written in the last of those ticks; the next
		 * sample's first is at least base - M0_STEP_TICKS ticks later. */
		m->sync_skip_ns = M0_PROFILE_TICKS * tick_ns + 200;
		if (base > M0_STEP_TICKS &&
		    (u64)(base - M0_STEP_TICKS) * tick_ns > m->sync_skip_ns + guard_ns + 2000)
			m->sync_len_ns = (base - M0_STEP_TICKS) * tick_ns - guard_ns;
	}
	m->stat_windows = m->stat_unsynced = m->stat_update_ns = 0;
	m->shmem->tick_cycles = cycles;
	m->shmem->ticks_base = base;
	m->shmem->ticks_frac = den ? div_u64((u64)(hclk % den) << 32, den) : 0;
	m->shmem->stat_min_cvr = U32_MAX;
	m->shmem->stat_overruns = 0;
}

/* What a PROFILE=1 firmware measured during the stream that just ended */
static void m0_report_profile(struct picocalc_m0 *m)
{
	uint32_t min_cvr = m->shmem->stat_min_cvr;
	uint32_t overruns = m->shmem->stat_overruns;

	if (min_cvr == U32_MAX || !m->tick_cycles)
		return;
	dev_info(&m->pdev->dev,
		 "M0 tick: longest %u of %u core cycles, %u ticks overran\n",
		 m->tick_cycles - 1 - min_cvr, m->tick_cycles, overruns);
	m->shmem->stat_min_cvr = U32_MAX;
}

/*
 * The core clock and this CPU's clock come from the same crystal, so a
 * firmware that is never held up plays exactly one frame per sample period
 * of ours. It falls behind by however long it is stalled waiting for the
 * bus, and by one sample for each one it finds the ring empty at in mid
 * stream. Good to a frame (21 us) over the stream.
 */
static void m0_report_pace(struct picocalc_m0 *m)
{
	unsigned long flags;
	u64 ns, frames, due_us, got_us;
	bool valid;

	spin_lock_irqsave(&m->lock, flags);
	valid = m->pace_valid;
	ns = m->pace_t1 - m->pace_t0;
	frames = m->pace_frames;
	m->pace_valid = false;
	spin_unlock_irqrestore(&m->lock, flags);
	if (!valid || ns < NSEC_PER_SEC / 10)
		return;
	due_us = div_u64(ns, NSEC_PER_USEC);
	got_us = div_u64(frames * USEC_PER_SEC, M0_FIXED_SAMPLE_RATE_HZ);
	dev_info(&m->pdev->dev,
		 "M0 played %llu frames in %llu us: %lld us behind (%lld us per second)\n",
		 frames, due_us, (s64)(due_us - got_us),
		 div64_s64((s64)(due_us - got_us) * USEC_PER_SEC, due_us));
	dev_info(&m->pdev->dev,
		 "ring updates: longest %u us, written in %u samples' quiet ticks, %u not synchronised\n",
		 m->stat_update_ns / 1000, m->stat_windows, m->stat_unsynced);
}

struct m0_sync {
	bool on;
	u64 end;   /* writing must stop here: the M0's next sample is near */
};

/*
 * Caller holds m->lock. Returns when the ring may be written: at once if the
 * current sample's quiet ticks have time left, else when the M0 has finished
 * the SRAM work of its next sample. If read_idx stops changing the M0 has
 * nothing to play and there is nothing to keep out of the way of.
 */
static void m0_sync_wait(struct picocalc_m0 *m, struct m0_sync *sync)
{
	u64 t0, before, now;
	uint32_t idx;

	if (!sync->on)
		return;
	now = ktime_get_raw_ns();
	if (now < sync->end)
		return;
	idx = m->shmem->read_idx;
	t0 = before = now;
	for (;;) {
		now = ktime_get_raw_ns();
		if (m->shmem->read_idx != idx)
			break;
		if (now - t0 > 3 * M0_SAMPLE_NS) {
			sync->on = false;
			return;
		}
		before = now;
	}
	/* It changed between 'before' and now */
	sync->end = before + m->sync_len_ns;
	now = ktime_get_raw_ns() + m->sync_skip_ns;
	while (ktime_get_raw_ns() < now)
		cpu_relax();
	m->stat_windows++;
}

/* Caller holds m->lock. Whole frames at multiples of the frame size: words. */
static void m0_ring_write(struct picocalc_m0 *m, struct m0_sync *sync,
			  uint32_t rpos, const uint8_t *from, uint32_t bytes)
{
	void __iomem *dst = (void __iomem __force *)(m->shmem->buffer + rpos);
	const u32 *src = (const u32 *)from;
	uint32_t words = bytes / 4, n, i;

	while (words) {
		n = min(words, M0_SYNC_BURST);
		m0_sync_wait(m, sync);
		for (i = 0; i < n; i++)
			__raw_writel(src[i], dst + 4 * i);
		src += n;
		dst += 4 * n;
		words -= n;
	}
}

/* Caller holds m->lock. PLAY + zeroed indices only while the M0 is idle. */
static void m0_init_header_locked(struct picocalc_m0 *m)
{
	struct snd_pcm_runtime *runtime;

	if (!m->substream || !m->substream->runtime || !m->shmem)
		return;
	runtime = m->substream->runtime;
	m0_set_tick_timing_locked(m);
	/* The frame before read_idx is what plays until the first frame is
	 * fetched, and straight away if the ring starts out empty. */
	memset(m->shmem->buffer + m->buf_size - 4, 0, 4);
	m->shmem->magic = M0_AUDIO_MAGIC;
	m->shmem->write_idx = 0;
	m->shmem->read_idx = 0;
	m->shmem->buf_size = m->buf_size;
	m->shmem->sample_rate = M0_FIXED_SAMPLE_RATE_HZ;
	m->shmem->channels = 2;
	m->shmem->format = M0_FMT_S16_LE;
	m->shmem->flags = (READ_ONCE(interp) ? 0 : M0_FLAG_NO_INTERP) |
			  (m->comp ? M0_FLAG_COMP : 0);
	m->shmem->ctrl = M0_CTRL_PLAY;
	dma_wmb();
	m->last_read_idx = 0;
	m->pace_valid = false;
}

static snd_pcm_uframes_t m0_frames_since(struct snd_pcm_runtime *runtime,
					 snd_pcm_uframes_t now, snd_pcm_uframes_t then)
{
	return now >= then ? now - then : runtime->boundary - then + now;
}

/* Caller holds m->lock. Fold the M0's read_idx progress into played_frames. */
static void m0_update_played_locked(struct picocalc_m0 *m, struct snd_pcm_runtime *runtime)
{
	uint32_t read_idx = m->shmem->read_idx;
	uint32_t delta = (read_idx - m->last_read_idx) & (m->buf_size - 1);

	m->last_read_idx = read_idx;
	m->played_frames += bytes_to_frames(runtime, delta);
	if (m->played_frames >= runtime->boundary)
		m->played_frames -= runtime->boundary;
}

/*
 * Caller holds m->lock. Top the ring up from the PCM buffer, so it does not
 * run one period from empty. At most buffer_size - period_size frames are kept
 * between the PCM buffer and the M0: the hw pointer (played_frames) must never
 * move a whole buffer between two looks at it, or ALSA cannot tell where it is.
 * appl_ptr and copied_frames share runtime->boundary so a full PCM buffer is
 * not mistaken for empty. playing: the M0 is reading the ring, see ring_sync.
 */
static void m0_copy_to_ring_locked(struct picocalc_m0 *m, bool playing)
{
	struct m0_sync sync = {
		.on = playing && m->sync_len_ns && READ_ONCE(ring_sync),
	};
	u64 start = ktime_get_raw_ns();
	uint32_t took;
	struct snd_pcm_substream *ss = m->substream;
	struct snd_pcm_runtime *runtime;
	uint32_t read_idx, write_idx, space, to_copy;
	uint32_t buffer_bytes, buf_mask, rpos, dma_pos, left, chunk, frame_bytes;
	snd_pcm_uframes_t appl, copied, avail_fr, to_fr, space_fr, in_flight, max_flight;
	const uint8_t *dma_area;

	if (!ss)
		return;
	runtime = ss->runtime;
	if (!runtime || !runtime->dma_area || !runtime->boundary)
		return;

	buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
	frame_bytes = frames_to_bytes(runtime, 1);
	if (!buffer_bytes || !frame_bytes)
		return;

	dma_area = (const uint8_t *)runtime->dma_area;
	buf_mask = m->buf_size - 1;

	m0_update_played_locked(m, runtime);
	read_idx = m->last_read_idx;
	dma_rmb();
	write_idx = m->shmem->write_idx;

	space = m0_ring_space(write_idx, read_idx, m->buf_size);
	appl = READ_ONCE(runtime->control->appl_ptr);
	copied = m->copied_frames;
	avail_fr = m0_frames_since(runtime, appl, copied);
	space_fr = space / frame_bytes;
	to_fr = space_fr;
	if (to_fr > avail_fr)
		to_fr = avail_fr;
	in_flight = m0_frames_since(runtime, copied, m->played_frames);
	max_flight = runtime->buffer_size - runtime->period_size;
	if (in_flight >= max_flight)
		return;
	if (to_fr > max_flight - in_flight)
		to_fr = max_flight - in_flight;
	to_copy = frames_to_bytes(runtime, to_fr);
	if (!to_copy)
		return;

	rpos = write_idx;
	dma_pos = frames_to_bytes(runtime, copied % runtime->buffer_size);
	left = to_copy;
	while (left > 0) {
		uint32_t ring_chunk = m->buf_size - rpos;
		uint32_t dma_chunk = buffer_bytes - dma_pos;

		chunk = left;
		if (chunk > ring_chunk)
			chunk = ring_chunk;
		if (chunk > dma_chunk)
			chunk = dma_chunk;
		m0_ring_write(m, &sync, rpos, dma_area + dma_pos, chunk);
		rpos = (rpos + chunk) & buf_mask;
		dma_pos += chunk;
		if (dma_pos >= buffer_bytes)
			dma_pos -= buffer_bytes;
		left -= chunk;
	}
	m0_sync_wait(m, &sync);
	m->shmem->write_idx = (write_idx + to_copy) & buf_mask;
	if (playing) {
		if (!sync.on)
			m->stat_unsynced++;
		took = ktime_get_raw_ns() - start;
		if (took > m->stat_update_ns)
			m->stat_update_ns = took;
	}
	m->copied_frames += bytes_to_frames(runtime, to_copy);
	if (m->copied_frames >= runtime->boundary)
		m->copied_frames -= runtime->boundary;
}

/*
 * The firmware looks at ctrl every few milliseconds when idle and once per
 * sample when playing, so this normally returns within a few milliseconds.
 */
static bool m0_wait_idle(struct picocalc_m0 *m)
{
	int i;

	for (i = 0; i < 250; i++) {
		if (READ_ONCE(m->shmem->m0_state) == M0_STATE_IDLE)
			return true;
		usleep_range(1000, 2000);
	}
	return false;
}

/*
 * Runs for every START and STOP (trigger cannot sleep). Whatever was going
 * on, bring the firmware to IDLE first; then, if a stream is wanted, hand it
 * over. The M0 is booted here the first time and never shut down between
 * streams.
 */
static void m0_rproc_work(struct work_struct *work)
{
	struct picocalc_m0 *m = container_of(work, struct picocalc_m0, rproc_work);
	unsigned long flags;
	bool want;
	int ret;

	spin_lock_irqsave(&m->lock, flags);
	want = m->want_play;
	m->running = false;
	m->shmem->ctrl = M0_CTRL_STOP;
	spin_unlock_irqrestore(&m->lock, flags);
	hrtimer_cancel(&m->timer);

	if (!m->rproc_up) {
		if (!want)
			return;
		/* Nothing in the header may look like a stream yet */
		m->shmem->magic = 0;
		m->shmem->m0_state = 0;
		m->shmem->stat_min_cvr = U32_MAX;
		ret = rproc_boot(m->rproc);
		if (ret) {
			dev_err(&m->pdev->dev, "rproc_boot failed: %d\n", ret);
			goto fail;
		}
		m->rproc_up = true;
	}

	if (!m0_wait_idle(m)) {
		dev_err(&m->pdev->dev, "M0 firmware is not responding (state %08x)\n",
			m->shmem->m0_state);
		goto fail;
	}
	m0_report_profile(m);
	m0_report_pace(m);

	spin_lock_irqsave(&m->lock, flags);
	want = m->want_play;
	if (want) {
		m0_init_header_locked(m);
		m0_copy_to_ring_locked(m, false);
		m->running = true;
		hrtimer_start(&m->timer, m->period_ktime, HRTIMER_MODE_REL);
	}
	spin_unlock_irqrestore(&m->lock, flags);
	return;

fail:
	spin_lock_irqsave(&m->lock, flags);
	want = m->want_play;
	m->want_play = false;
	m->running = false;
	spin_unlock_irqrestore(&m->lock, flags);
	if (want && m->substream)
		snd_pcm_stop_xrun(m->substream);
}

static enum hrtimer_restart m0_timer_cb(struct hrtimer *t)
{
	struct picocalc_m0 *m = container_of(t, struct picocalc_m0, timer);
	struct snd_pcm_substream *ss;
	unsigned long flags;
	struct snd_pcm_runtime *runtime;
	snd_pcm_uframes_t since;
	bool elapsed = false, queued;
	u64 now;

	spin_lock_irqsave(&m->lock, flags);
	ss = m->substream;
	if (!ss || !m->running || !ss->runtime || !ss->runtime->dma_area) {
		spin_unlock_irqrestore(&m->lock, flags);
		return HRTIMER_NORESTART;
	}
	runtime = ss->runtime;
	/* The firmware stops one frame short of write_idx. Once it is there
	 * (the end of a stream, normally) it is waiting for data, not late. */
	queued = ((m->shmem->write_idx - m->shmem->read_idx) & (m->buf_size - 1)) > 4;
	now = ktime_get_raw_ns();
	m0_copy_to_ring_locked(m, true); /* first of all brings played_frames up to date */
	if (!m->pace_valid) {
		m->pace_valid = true;
		m->pace_t0 = m->pace_t1 = now;
		m->pace_frames = 0;
		m->pace_mark = m->played_frames;
	} else if (queued) {
		m->pace_frames += m0_frames_since(runtime, m->played_frames, m->pace_mark);
		m->pace_mark = m->played_frames;
		m->pace_t1 = now;
	}
	since = m0_frames_since(runtime, m->played_frames, m->elapsed_mark);
	if (since >= runtime->period_size) {
		m->elapsed_mark += since - since % runtime->period_size;
		if (m->elapsed_mark >= runtime->boundary)
			m->elapsed_mark -= runtime->boundary;
		elapsed = true;
	}
	spin_unlock_irqrestore(&m->lock, flags);
	if (elapsed)
		snd_pcm_period_elapsed(ss);

	hrtimer_forward(t, hrtimer_cb_get_time(t), m->period_ktime);
	return HRTIMER_RESTART;
}

static int m0_pcm_open(struct snd_pcm_substream *ss)
{
	struct picocalc_m0 *m = ss->pcm->card->private_data;

	ss->private_data = m;

	/* Fixed config: M0 firmware is 48 kHz S16 LE stereo only; ALSA SRC handles other rates */
	ss->runtime->hw = (struct snd_pcm_hardware){
		.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
			SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_HALF_DUPLEX,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
		.rates = SNDRV_PCM_RATE_48000,
		.rate_min = M0_FIXED_SAMPLE_RATE_HZ,
		.rate_max = M0_FIXED_SAMPLE_RATE_HZ,
		.channels_min = 2,
		.channels_max = 2,
		.buffer_bytes_max = 32768,
		.period_bytes_min = 1024,
		/* Half the ring: a period of data can then always be queued
		 * ahead of the M0 while another is being played. */
		.period_bytes_max = M0_FIXED_BUF_SIZE / 2,
		.periods_min = 2,
		.periods_max = 16,
	};
	m->substream = ss;
	return 0;
}

static int m0_pcm_close(struct snd_pcm_substream *ss)
{
	struct picocalc_m0 *m = snd_pcm_substream_chip(ss);
	unsigned long flags;

	spin_lock_irqsave(&m->lock, flags);
	m->substream = NULL;
	spin_unlock_irqrestore(&m->lock, flags);
	return 0;
}

/*
 * Called (sleepable) after a stop and before hw_free/prepare/close. Trigger
 * only flags the stop; wait here until the worker has shut the M0 down and
 * the timer callback can no longer be copying out of the PCM buffer.
 */
static int m0_pcm_sync_stop(struct snd_pcm_substream *ss)
{
	struct picocalc_m0 *m = snd_pcm_substream_chip(ss);

	flush_work(&m->rproc_work);
	hrtimer_cancel(&m->timer);
	return 0;
}

static int m0_pcm_hw_params(struct snd_pcm_substream *ss,
			    struct snd_pcm_hw_params *hw_params)
{
	return snd_pcm_lib_malloc_pages(ss, params_buffer_bytes(hw_params));
}

static int m0_pcm_hw_free(struct snd_pcm_substream *ss)
{
	return snd_pcm_lib_free_pages(ss);
}

static int m0_pcm_prepare(struct snd_pcm_substream *ss)
{
	return 0;
}

static int m0_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct picocalc_m0 *m = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *runtime = ss->runtime;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&m->lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (m->want_play) {
			ret = -EALREADY;
			break;
		}
		m->copied_frames = 0;
		m->played_frames = 0;
		m->elapsed_mark = 0;
		/* Header/PLAY are written in work once the M0 is confirmed idle. */
		m->shmem->ctrl = M0_CTRL_STOP;
		/* Fire once per ALSA period (do_div avoids __aeabi_uldivmod on 32-bit ARM) */
		{
			u64 nsec = (u64)NSEC_PER_SEC * runtime->period_size;
			do_div(nsec, runtime->rate);
			m->period_ktime = ns_to_ktime(nsec);
		}

		m->want_play = true;
		schedule_work(&m->rproc_work);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
		m->want_play = false;
		m->running = false; /* timer cb returns NORESTART; no cancel here */
		m->shmem->ctrl = M0_CTRL_STOP;
		schedule_work(&m->rproc_work);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	spin_unlock_irqrestore(&m->lock, flags);
	return ret;
}

static snd_pcm_uframes_t m0_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct picocalc_m0 *m = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *runtime = ss->runtime;
	snd_pcm_uframes_t pos;
	unsigned long flags;

	if (!runtime->buffer_size)
		return 0;
	/* The hw pointer is what the M0 has actually played (it publishes its
	 * ring read index every frame), not what has been queued for it. */
	spin_lock_irqsave(&m->lock, flags);
	if (m->running && m->shmem)
		m0_update_played_locked(m, runtime);
	pos = m->played_frames % runtime->buffer_size;
	spin_unlock_irqrestore(&m->lock, flags);
	return pos;
}

static const struct snd_pcm_ops m0_pcm_ops = {
	.open    = m0_pcm_open,
	.close   = m0_pcm_close,
	.ioctl   = snd_pcm_lib_ioctl,
	.hw_params = m0_pcm_hw_params,
	.hw_free = m0_pcm_hw_free,
	.prepare = m0_pcm_prepare,
	.trigger = m0_pcm_trigger,
	.sync_stop = m0_pcm_sync_stop,
	.pointer = m0_pcm_pointer,
};

static int m0_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct picocalc_m0 *m;
	struct resource res;
	struct device_node *rproc_np;
	struct device_node *mem_np;
	int ret;

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	m->pdev = pdev;
	spin_lock_init(&m->lock);
	INIT_WORK(&m->rproc_work, m0_rproc_work);
	hrtimer_init(&m->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	m->timer.function = m0_timer_cb;

	rproc_np = of_parse_phandle(np, "remote-proc", 0);
	if (!rproc_np) {
		dev_err(dev, "missing remote-proc phandle\n");
		return -EINVAL;
	}
	m->rproc = rproc_get_by_phandle(rproc_np->phandle);
	of_node_put(rproc_np);
	if (!m->rproc) {
		dev_err(dev, "rproc_get_by_phandle failed\n");
		return -EPROBE_DEFER;
	}

	mem_np = of_parse_phandle(np, "memory-region", 0);
	if (!mem_np) {
		dev_err(dev, "missing memory-region phandle\n");
		ret = -EINVAL;
		goto put_rproc;
	}
	/* Any node with a reg will do: the ring is SRAM, not memory Linux
	 * manages, so it need not be a boot-time reserved-memory entry and an
	 * overlay can describe it. */
	ret = of_address_to_resource(mem_np, 0, &res);
	of_node_put(mem_np);
	if (ret) {
		dev_err(dev, "memory-region has no usable reg\n");
		goto put_rproc;
	}
	if (resource_size(&res) < M0_HEADER_SIZE + M0_FIXED_BUF_SIZE) {
		dev_err(dev, "memory-region too small (%pR)\n", &res);
		ret = -EINVAL;
		goto put_rproc;
	}
	m->shmem_size = resource_size(&res);
	/* SRAM the M0 is using too: as device memory, every write goes out on
	 * its own and when we make it (m0_ring_write) */
	m->shmem_virt = (void __force *)devm_ioremap(dev, res.start, resource_size(&res));
	if (!m->shmem_virt) {
		ret = -ENOMEM;
		goto put_rproc;
	}
	m->shmem = (struct m0_audio_shmem *)m->shmem_virt;

	/* Optional: without it the firmware falls back to its built-in timing */
	m->core_clk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(m->core_clk)) {
		ret = PTR_ERR(m->core_clk);
		goto put_rproc;
	}
	of_property_read_u32(np, "tick-rate-hz", &tick_hz);
	of_property_read_u32(np, "comp-tick-rate-hz", &tick_hz_comp);

	if (of_property_read_u32(np, "ring-buffer-bytes", &m->buf_size))
		m->buf_size = M0_FIXED_BUF_SIZE;
	if (m->buf_size != M0_FIXED_BUF_SIZE || m->buf_size > m->shmem_size - M0_HEADER_SIZE)
		m->buf_size = M0_FIXED_BUF_SIZE;

	ret = snd_card_new(dev, 0, "picocalc-m0",
			   THIS_MODULE, 0, &m->card);
	if (ret < 0)
		goto put_rproc;

	m->card->private_data = m;
	strscpy(m->card->driver, "picocalc-snd-m0", sizeof(m->card->driver));
	strscpy(m->card->shortname, "PicoCalc M0 Audio", sizeof(m->card->shortname));
	strscpy(m->card->longname, "PicoCalc M0 delta-sigma audio", sizeof(m->card->longname));

	{
		struct snd_pcm *pcm;
		ret = snd_pcm_new(m->card, "M0 PCM", 0, 1, 0, &pcm);
		if (ret < 0)
			goto card_free;
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &m0_pcm_ops);
		pcm->private_data = m;
		strscpy(pcm->name, "M0 DAC", sizeof(pcm->name));
		snd_pcm_lib_preallocate_pages_for_all(pcm,
					      SNDRV_DMA_TYPE_CONTINUOUS,
					      dev, 32768, 32768);
	}

	ret = snd_card_register(m->card);
	if (ret < 0)
		goto card_free;

	platform_set_drvdata(pdev, m);
	g_m0 = m;
	dev_info(dev, "PicoCalc M0 audio registered (ring %u bytes)\n", m->buf_size);
	return 0;

card_free:
	snd_card_free(m->card);
put_rproc:
	rproc_put(m->rproc);
	return ret;
}

static int m0_remove(struct platform_device *pdev)
{
	struct picocalc_m0 *m = platform_get_drvdata(pdev);
	unsigned long flags;

	if (!m)
		return 0;
	g_m0 = NULL;
	spin_lock_irqsave(&m->lock, flags);
	m->want_play = false;
	m->running = false;
	if (m->shmem)
		m->shmem->ctrl = M0_CTRL_STOP;
	spin_unlock_irqrestore(&m->lock, flags);
	cancel_work_sync(&m->rproc_work);
	hrtimer_cancel(&m->timer);
	if (m->rproc_up)
		rproc_shutdown(m->rproc);
	snd_card_free(m->card);
	rproc_put(m->rproc);
	return 0;
}

static struct platform_driver picocalc_snd_m0_driver = {
	.driver = {
		.name = "picocalc-snd-m0",
		.of_match_table = picocalc_snd_m0_dt_ids,
	},
	.probe = m0_probe,
	.remove = m0_remove,
};

module_platform_driver(picocalc_snd_m0_driver);

MODULE_AUTHOR("Ben Klopfenstein");
MODULE_DESCRIPTION("PicoCalc M0 delta-sigma audio driver");
MODULE_LICENSE("GPL v2");
