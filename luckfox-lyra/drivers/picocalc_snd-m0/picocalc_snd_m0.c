// SPDX-License-Identifier: GPL-2.0
/*
 * PicoCalc M0 delta-sigma audio driver
 * Uses RK3506 Cortex-M0 core to drive GPIO4_B2/B3 from shared memory ring buffer.
 */

#include <asm/barrier.h>
#include <asm/div64.h>
#include <linux/delay.h>
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
	void *shmem_virt;  /* WB-mapped for fast ring buffer access */
	size_t shmem_size;
	uint32_t buf_size;
	spinlock_t lock;
	struct hrtimer timer;
	ktime_t period_ktime;
	uint32_t last_read_idx;
	uint32_t copied_bytes;
	bool running;
	struct work_struct start_work;
	struct work_struct stop_work;
};

static struct picocalc_m0 *g_m0;

/*
 * Power management (M0-side WFE idle and optional WIC deep sleep) is currently
 * unreachable: we always rproc_boot() on start and rproc_shutdown() on stop,
 * so the M0 is reloaded from ELF each time and never sits in WFE between
 * play cycles. To use the M0 power paths (and avoid boot/shutdown latency),
 * a future phase could: (a) boot M0 once at probe or first play and keep it
 * running, (b) ioremap the GRF region, (c) assert rxev to wake M0 from WFE/WFI
 * before setting ctrl=PLAY, (d) clear rxev after wake.
 */
static void m0_start_work(struct work_struct *work)
{
	struct picocalc_m0 *m = container_of(work, struct picocalc_m0, start_work);
	unsigned long flags;
	int ret;

	ret = rproc_boot(m->rproc);
	if (ret) {
		dev_err(&m->pdev->dev, "rproc_boot failed: %d\n", ret);
		return;
	}
	spin_lock_irqsave(&m->lock, flags);
	m->running = true;
	hrtimer_start(&m->timer, m->period_ktime, HRTIMER_MODE_REL);
	spin_unlock_irqrestore(&m->lock, flags);
}

static void m0_stop_work(struct work_struct *work)
{
	struct picocalc_m0 *m = container_of(work, struct picocalc_m0, stop_work);

	msleep(2);
	rproc_shutdown(m->rproc);
}

static enum hrtimer_restart m0_timer_cb(struct hrtimer *t)
{
	struct picocalc_m0 *m = container_of(t, struct picocalc_m0, timer);
	struct snd_pcm_substream *ss = m->substream;
	struct snd_pcm_runtime *runtime;
	uint32_t read_idx, write_idx, period_bytes;
	uint32_t space, to_copy, buffer_bytes;
	uint32_t buf_mask, rpos, dma_pos, left, chunk;
	uint8_t *ring;
	const uint8_t *dma_area;

	if (!ss || !m->running)
		return HRTIMER_NORESTART;

	runtime = ss->runtime;
	if (!runtime || !runtime->dma_area)
		return HRTIMER_RESTART;

	period_bytes = m->shmem->period_bytes;
	buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
	ring = (uint8_t *)m->shmem->buffer;
	dma_area = (const uint8_t *)runtime->dma_area;
	buf_mask = m->buf_size - 1;

	read_idx = m->shmem->read_idx;
	write_idx = m->shmem->write_idx;

	/* Free space in ring: up to buf_size - 1 to avoid write_idx == read_idx meaning empty */
	if (write_idx >= read_idx)
		space = m->buf_size - (write_idx - read_idx) - 1;
	else
		space = read_idx - write_idx - 1;

	to_copy = period_bytes;
	if (to_copy > space)
		to_copy = space;
	if (to_copy > 0 && m->copied_bytes + to_copy <= buffer_bytes) {
		rpos = write_idx;
		dma_pos = m->copied_bytes % buffer_bytes;
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
			dma_pos = (dma_pos + chunk) % buffer_bytes;
			left -= chunk;
		}
		dma_wmb(); /* ensure ring data visible to M0 before updating write_idx */
		m->shmem->write_idx = (write_idx + to_copy) & buf_mask;
		m->copied_bytes += to_copy;
	}

	/* Period elapsed? (handle ring wrap with mask) */
	if (((read_idx - m->last_read_idx) & buf_mask) >= period_bytes) {
		m->last_read_idx = read_idx;
		snd_pcm_period_elapsed(ss);
	}

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
		.period_bytes_max = 8192,
		.periods_min = 2,
		.periods_max = 16,
	};
	m->substream = ss;
	return 0;
}

static int m0_pcm_close(struct snd_pcm_substream *ss)
{
	struct picocalc_m0 *m = snd_pcm_substream_chip(ss);

	m->substream = NULL;
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
		if (m->running) {
			ret = -EALREADY;
			break;
		}
		m->shmem->magic = M0_AUDIO_MAGIC;
		m->shmem->ctrl = M0_CTRL_PLAY;
		m->shmem->write_idx = 0;
		m->shmem->read_idx = 0;
		m->shmem->period_bytes = frames_to_bytes(runtime, runtime->period_size);
		m->shmem->buf_size = m->buf_size;  /* must be M0_FIXED_BUF_SIZE */
		m->shmem->sample_rate = M0_FIXED_SAMPLE_RATE_HZ;
		m->shmem->channels = 2;
		m->shmem->format = M0_FMT_S16_LE;
		dma_wmb(); /* ensure header visible to M0 before it sees ctrl/indices */

		m->last_read_idx = 0;
		m->copied_bytes = 0;
		/* Fire once per ALSA period, not per sample (do_div avoids __aeabi_uldivmod on 32-bit ARM) */
		{
			u64 nsec = (u64)NSEC_PER_SEC * runtime->period_size;
			do_div(nsec, runtime->rate);
			m->period_ktime = ns_to_ktime(nsec);
		}

		schedule_work(&m->start_work);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
		if (!m->running)
			break;
		m->running = false;
		hrtimer_cancel(&m->timer);
		m->shmem->ctrl = M0_CTRL_STOP;
		spin_unlock_irqrestore(&m->lock, flags);
		schedule_work(&m->stop_work);
		return 0;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&m->lock, flags);
	return ret;
}

static snd_pcm_uframes_t m0_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct picocalc_m0 *m = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *runtime = ss->runtime;
	unsigned int frame_size = frames_to_bytes(runtime, 1);
	snd_pcm_uframes_t consumed_frames;

	/* Position in ALSA buffer = bytes consumed (handed to M0 ring) in frames, modulo buffer */
	consumed_frames = m->copied_bytes / frame_size;
	return consumed_frames % runtime->buffer_size;
}

static const struct snd_pcm_ops m0_pcm_ops = {
	.open    = m0_pcm_open,
	.close   = m0_pcm_close,
	.ioctl   = snd_pcm_lib_ioctl,
	.hw_params = m0_pcm_hw_params,
	.hw_free = m0_pcm_hw_free,
	.prepare = m0_pcm_prepare,
	.trigger = m0_pcm_trigger,
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
	INIT_WORK(&m->start_work, m0_start_work);
	INIT_WORK(&m->stop_work, m0_stop_work);
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
	m->shmem_size = rmem->size;
	m->shmem_virt = memremap(rmem->base, rmem->size, MEMREMAP_WB);
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
		goto memunmap;

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
memunmap:
	memunmap(m->shmem_virt);
put_rproc:
	rproc_put(m->rproc);
	return ret;
}

static int m0_remove(struct platform_device *pdev)
{
	struct picocalc_m0 *m = platform_get_drvdata(pdev);

	if (!m)
		return 0;
	g_m0 = NULL;
	cancel_work_sync(&m->start_work);
	cancel_work_sync(&m->stop_work);
	hrtimer_cancel(&m->timer);
	if (m->running) {
		m->shmem->ctrl = M0_CTRL_STOP;
		msleep(2);
		rproc_shutdown(m->rproc);
	}
	snd_card_free(m->card);
	memunmap(m->shmem_virt);
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
