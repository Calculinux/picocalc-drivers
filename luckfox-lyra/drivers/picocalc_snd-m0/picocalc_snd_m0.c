// SPDX-License-Identifier: GPL-2.0
/*
 * PicoCalc M0 delta-sigma audio driver
 * Uses RK3506 Cortex-M0 core to drive GPIO4_B2/B3 from a shared ring buffer.
 * The header and ring live in system SRAM next to the M0 firmware image (the
 * m0_shmem reserved-memory node), so the M0 never reads DDR while playing.
 */

#include <asm/barrier.h>
#include <asm/div64.h>
#include <linux/compiler.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/of_reserved_mem.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>

/* Must match picocalc_m0_audio_fw/shmem.h and main.c fixed config */
#define M0_AUDIO_MAGIC    0x4D304431U
#define M0_CTRL_PLAY      (1u << 0)
#define M0_CTRL_STOP      0u
#define M0_FMT_U8         0
#define M0_FMT_S16_LE     1
#define M0_HEADER_SIZE    64
/* Fixed M0 config: ALSA does SRC to this rate; ring size must match firmware */
#define M0_FIXED_SAMPLE_RATE_HZ  48000U
#define M0_FIXED_BUF_SIZE        8192U

struct m0_audio_shmem {
	volatile uint32_t magic;
	volatile uint32_t ctrl;
	volatile uint32_t write_idx;
	volatile uint32_t read_idx;
	volatile uint32_t period_bytes;
	volatile uint32_t buf_size;
	volatile uint32_t sample_rate;
	volatile uint32_t channels;
	volatile uint32_t format;
	volatile uint32_t flags;   /* M0_SHMEM_FLAG_WIC_WAKE etc. */
	uint32_t          _reserved[6];
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
	struct m0_audio_shmem *shmem;
	void *shmem_virt;  /* WC mapping of the SRAM ring */
	size_t shmem_size;
	uint32_t buf_size;
	spinlock_t lock;
	struct hrtimer timer;
	ktime_t period_ktime;
	uint32_t last_read_idx;
	snd_pcm_uframes_t copied_frames; /* same wrap domain as appl_ptr (boundary) */
	bool running;
	bool want_play;
	bool rproc_up;
	struct work_struct rproc_work;
};

static struct picocalc_m0 *g_m0;

static uint32_t m0_ring_space(uint32_t write_idx, uint32_t read_idx, uint32_t buf_size)
{
	if (write_idx >= read_idx)
		return buf_size - (write_idx - read_idx) - 1;
	return read_idx - write_idx - 1;
}

/* Caller holds m->lock. PLAY + zeroed indices only after M0 is down. */
static void m0_init_header_locked(struct picocalc_m0 *m)
{
	struct snd_pcm_runtime *runtime;

	if (!m->substream || !m->substream->runtime || !m->shmem)
		return;
	runtime = m->substream->runtime;
	m->shmem->magic = M0_AUDIO_MAGIC;
	m->shmem->write_idx = 0;
	m->shmem->read_idx = 0;
	m->shmem->period_bytes = frames_to_bytes(runtime, runtime->period_size);
	m->shmem->buf_size = m->buf_size;
	m->shmem->sample_rate = M0_FIXED_SAMPLE_RATE_HZ;
	m->shmem->channels = 2;
	m->shmem->format = M0_FMT_S16_LE;
	m->shmem->ctrl = M0_CTRL_PLAY;
	dma_wmb();
	m->last_read_idx = 0;
}

/*
 * Caller holds m->lock. Copy as much as the ring will take, so it stays
 * topped up between timer callbacks instead of running one period from empty.
 * appl_ptr and copied_frames share runtime->boundary so a full PCM buffer is
 * not mistaken for empty.
 */
static void m0_copy_to_ring_locked(struct picocalc_m0 *m)
{
	struct snd_pcm_substream *ss = m->substream;
	struct snd_pcm_runtime *runtime;
	uint32_t read_idx, write_idx, space, to_copy;
	uint32_t buffer_bytes, buf_mask, rpos, dma_pos, left, chunk, frame_bytes;
	snd_pcm_uframes_t appl, copied, avail_fr, to_fr, space_fr;
	uint8_t *ring;
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

	ring = (uint8_t *)m->shmem->buffer;
	dma_area = (const uint8_t *)runtime->dma_area;
	buf_mask = m->buf_size - 1;

	read_idx = m->shmem->read_idx;
	dma_rmb();
	write_idx = m->shmem->write_idx;

	space = m0_ring_space(write_idx, read_idx, m->buf_size);
	appl = READ_ONCE(runtime->control->appl_ptr);
	copied = m->copied_frames;
	if (appl >= copied)
		avail_fr = appl - copied;
	else
		avail_fr = runtime->boundary - copied + appl;
	space_fr = space / frame_bytes;
	to_fr = space_fr;
	if (to_fr > avail_fr)
		to_fr = avail_fr;
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
		memcpy(ring + rpos, dma_area + dma_pos, chunk);
		rpos = (rpos + chunk) & buf_mask;
		dma_pos += chunk;
		if (dma_pos >= buffer_bytes)
			dma_pos -= buffer_bytes;
		left -= chunk;
	}
	dma_wmb(); /* ring data visible to M0 before write_idx (WC map) */
	m->shmem->write_idx = (write_idx + to_copy) & buf_mask;
	m->copied_frames += bytes_to_frames(runtime, to_copy);
	if (m->copied_frames >= runtime->boundary)
		m->copied_frames -= runtime->boundary;
}

/*
 * Power management (M0-side WFE idle and optional WIC deep sleep) is currently
 * unreachable: we always rproc_boot() on start and rproc_shutdown() on stop,
 * so the M0 is reloaded from ELF each time and never sits in WFE between
 * play cycles. To use the M0 power paths (and avoid boot/shutdown latency),
 * a future phase could: (a) boot M0 once at probe or first play and keep it
 * running, (b) ioremap the GRF region, (c) assert rxev to wake M0 from WFE/WFI
 * before setting ctrl=PLAY, (d) clear rxev after wake.
 */
static void m0_rproc_work(struct work_struct *work)
{
	struct picocalc_m0 *m = container_of(work, struct picocalc_m0, rproc_work);
	unsigned long flags;
	int ret;

	for (;;) {
		bool want;

		spin_lock_irqsave(&m->lock, flags);
		want = m->want_play;
		spin_unlock_irqrestore(&m->lock, flags);

		if (want) {
			/* Reboot M0 so its local read_idx cannot outlive host index reset. */
			if (m->rproc_up) {
				spin_lock_irqsave(&m->lock, flags);
				m->running = false;
				if (m->shmem)
					m->shmem->ctrl = M0_CTRL_STOP;
				spin_unlock_irqrestore(&m->lock, flags);
				hrtimer_cancel(&m->timer);
				rproc_shutdown(m->rproc);
				m->rproc_up = false;
				continue;
			}
			hrtimer_cancel(&m->timer);
			spin_lock_irqsave(&m->lock, flags);
			m0_init_header_locked(m);
			m0_copy_to_ring_locked(m);
			spin_unlock_irqrestore(&m->lock, flags);
			ret = rproc_boot(m->rproc);
			if (ret) {
				dev_err(&m->pdev->dev, "rproc_boot failed: %d\n", ret);
				spin_lock_irqsave(&m->lock, flags);
				m->want_play = false;
				m->running = false;
				spin_unlock_irqrestore(&m->lock, flags);
				if (m->substream)
					snd_pcm_stop_xrun(m->substream);
				return;
			}
			m->rproc_up = true;
			spin_lock_irqsave(&m->lock, flags);
			if (m->want_play) {
				m->running = true;
				hrtimer_start(&m->timer, m->period_ktime, HRTIMER_MODE_REL);
			}
			want = m->want_play;
			spin_unlock_irqrestore(&m->lock, flags);
			if (want)
				break;
			continue;
		}

		spin_lock_irqsave(&m->lock, flags);
		m->running = false;
		spin_unlock_irqrestore(&m->lock, flags);
		hrtimer_cancel(&m->timer);
		if (m->rproc_up) {
			rproc_shutdown(m->rproc);
			m->rproc_up = false;
		}
		spin_lock_irqsave(&m->lock, flags);
		want = m->want_play;
		spin_unlock_irqrestore(&m->lock, flags);
		if (want)
			continue; /* START arrived during shutdown */
		break;
	}
}

static enum hrtimer_restart m0_timer_cb(struct hrtimer *t)
{
	struct picocalc_m0 *m = container_of(t, struct picocalc_m0, timer);
	struct snd_pcm_substream *ss;
	unsigned long flags;
	uint32_t read_idx, period_bytes, buf_mask;
	bool elapsed = false;

	spin_lock_irqsave(&m->lock, flags);
	ss = m->substream;
	if (!ss || !m->running || !ss->runtime || !ss->runtime->dma_area) {
		spin_unlock_irqrestore(&m->lock, flags);
		return HRTIMER_NORESTART;
	}
	m0_copy_to_ring_locked(m);
	period_bytes = m->shmem->period_bytes;
	buf_mask = m->buf_size - 1;
	read_idx = m->shmem->read_idx;
	dma_rmb();
	/* The masked difference is at most buf_size - 1, so this can only fire
	 * for periods shorter than the ring; hw.period_bytes_max enforces that. */
	if (((read_idx - m->last_read_idx) & buf_mask) >= period_bytes) {
		m->last_read_idx = read_idx;
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
		/* Must stay below the ring size: see the period check in
		 * m0_timer_cb(). Half the ring also leaves a period of margin. */
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
		/* Header/PLAY are written in work after M0 is confirmed down. */
		if (m->rproc_up && m->shmem)
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

	/* Copy-to-ring model: hw_ptr is frames handed to the M0 ring */
	if (!ss->runtime->buffer_size)
		return 0;
	return m->copied_frames % ss->runtime->buffer_size;
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
	struct reserved_mem *rmem;
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
	rmem = of_reserved_mem_lookup(mem_np);
	of_node_put(mem_np);
	if (!rmem) {
		dev_err(dev, "missing memory-region / reserved-mem\n");
		ret = -EINVAL;
		goto put_rproc;
	}
	if (rmem->size < M0_HEADER_SIZE + M0_FIXED_BUF_SIZE) {
		dev_err(dev, "memory-region too small (%zu)\n", (size_t)rmem->size);
		ret = -EINVAL;
		goto put_rproc;
	}
	m->shmem_size = rmem->size;
	/* SRAM, not system RAM: this is an ioremap_wc underneath */
	m->shmem_virt = devm_memremap(dev, rmem->base, rmem->size, MEMREMAP_WC);
	if (!m->shmem_virt) {
		ret = -ENOMEM;
		goto put_rproc;
	}
	m->shmem = (struct m0_audio_shmem *)m->shmem_virt;

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
