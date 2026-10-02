// SPDX-License-Identifier: GPL-2.0
/*
 * drmfps [FRAMES]: how fast the display takes full-screen updates through
 * KMS, the way a game or emulator would send them. Puts a dumb buffer on the
 * screen, then changes it and reports it dirty FRAMES times (default 60);
 * each report blocks until the frame has gone out. The console comes back
 * when this exits.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 60, fd, i;
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_get_connector conn = { 0 };
	struct drm_mode_modeinfo mode;
	struct drm_mode_create_dumb cd = { 0 };
	struct drm_mode_fb_cmd fb = { 0 };
	struct drm_mode_map_dumb md = { 0 };
	struct drm_mode_crtc crtc = { 0 };
	struct drm_mode_fb_dirty_cmd dirty = { 0 };
	uint32_t crtcs[8], conns[8], encs[8], fbs[8];
	uint32_t *px;
	double t0, t1;

	fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		perror("/dev/dri/card0");
		return 1;
	}
	if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0))
		perror("SET_MASTER (carrying on)");
	res.count_crtcs = res.count_connectors = res.count_encoders = res.count_fbs = 8;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)conns;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.fb_id_ptr = (uintptr_t)fbs;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs || !res.count_connectors) {
		perror("GETRESOURCES");
		return 1;
	}
	conn.connector_id = conns[0];
	conn.count_modes = 1;
	conn.modes_ptr = (uintptr_t)&mode;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || !conn.count_modes) {
		perror("GETCONNECTOR");
		return 1;
	}
	cd.width = mode.hdisplay;
	cd.height = mode.vdisplay;
	cd.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
		perror("CREATE_DUMB");
		return 1;
	}
	fb.width = cd.width;
	fb.height = cd.height;
	fb.pitch = cd.pitch;
	fb.bpp = 32;
	fb.depth = 24;
	fb.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb)) {
		perror("ADDFB");
		return 1;
	}
	md.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
		perror("MAP_DUMB");
		return 1;
	}
	px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (px == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(px, 0, cd.size);
	crtc.crtc_id = crtcs[0];
	crtc.fb_id = fb.fb_id;
	crtc.set_connectors_ptr = (uintptr_t)conns;
	crtc.count_connectors = 1;
	crtc.mode = mode;
	crtc.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc)) {
		perror("SETCRTC");
		return 1;
	}
	dirty.fb_id = fb.fb_id;
	t0 = now();
	for (i = 0; i < frames; i++) {
		uint32_t x, n = cd.width * cd.height, c = 0x00102030u * (uint32_t)(i + 1);

		for (x = 0; x < n; x++)
			px[x] = c + x * 0x00010307u;	/* every pixel changes every frame */
		if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty)) {
			perror("DIRTYFB");
			return 1;
		}
	}
	t1 = now();
	printf("%ux%u: %d full frames in %.2f s: %.1f fps, %.1f ms a frame\n",
	       cd.width, cd.height, frames, t1 - t0, frames / (t1 - t0), 1000 * (t1 - t0) / frames);
	return 0;
}
