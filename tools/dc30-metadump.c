// SPDX-License-Identifier: GPL-2.0-only
/*
 * dc30-metadump - read the dc30 driver's per-frame metadata records
 * (dc30-meta node, src/dc30_meta.h) and check the audio/video relation.
 *
 * Streams the metadata node until N records arrived (the video node and,
 * for audio positions, an ALSA capture must run at the same time).
 * Prints one line per record with -v, then a
 * summary: sequence gaps, and a least-squares fit of the audio position
 * against the frame sequence number, i.e. audio frames per video frame
 * and how far single values scatter around that line.
 *
 * Usage: dc30-metadump [-d /dev/videoN] [-n records] [-v]
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "../src/dc30_meta.h"

#define NBUFS 16

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

/* The metadata node by its name, as dc30_video.c registers it. */
static int find_meta_node(char *path, size_t len)
{
	char name[64], sysfs[128];
	int i;

	for (i = 0; i < 64; i++) {
		FILE *f;

		snprintf(sysfs, sizeof(sysfs),
			 "/sys/class/video4linux/video%d/name", i);
		f = fopen(sysfs, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) &&
		    !strncmp(name, "dc30-meta", 9)) {
			fclose(f);
			snprintf(path, len, "/dev/video%d", i);
			return 0;
		}
		fclose(f);
	}
	return -1;
}

/* Running least-squares fit of y over x, shifted for precision. */
struct fit {
	double n, sx, sy, sxx, sxy, syy;
	double x0, y0;
	int have0;
};

static void fit_add(struct fit *f, double x, double y)
{
	if (!f->have0) {
		f->x0 = x;
		f->y0 = y;
		f->have0 = 1;
	}
	x -= f->x0;
	y -= f->y0;
	f->n++;
	f->sx += x;
	f->sy += y;
	f->sxx += x * x;
	f->sxy += x * y;
	f->syy += y * y;
}

static int fit_result(const struct fit *f, double *slope, double *resid_sd)
{
	double d = f->n * f->sxx - f->sx * f->sx;
	double b, a, ss;

	if (f->n < 3 || d == 0)
		return -1;
	b = (f->n * f->sxy - f->sx * f->sy) / d;
	a = (f->sy - b * f->sx) / f->n;
	/* sum of squared residuals from the accumulated sums */
	ss = f->syy - 2 * a * f->sy - 2 * b * f->sxy + a * a * f->n +
	     2 * a * b * f->sx + b * b * f->sxx;
	*slope = b;
	*resid_sd = ss > 0 ? sqrt(ss / f->n) : 0;
	return 0;
}

int main(int argc, char **argv)
{
	char path[64] = "";
	unsigned int want = 250, got = 0;
	int verbose = 0, opt, fd, i;
	struct v4l2_requestbuffers req = { 0 };
	void *maps[NBUFS];
	size_t lens[NBUFS];
	enum v4l2_buf_type type = V4L2_BUF_TYPE_META_CAPTURE;
	long long prev_seq = -1, gaps = 0, lost = 0;
	long long prev_pos = 0;
	unsigned int prev_epoch = 0, epochs = 0, no_audio = 0;
	int have_prev_pos = 0;
	struct fit fit_seq = { 0 }, fit_time = { 0 };
	struct dc30_meta last = { 0 };
	double frame_bytes = 4;

	while ((opt = getopt(argc, argv, "d:n:v")) != -1) {
		switch (opt) {
		case 'd':
			snprintf(path, sizeof(path), "%s", optarg);
			break;
		case 'n':
			want = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-d /dev/videoN] [-n records] [-v]\n",
				argv[0]);
			return 1;
		}
	}
	if (!path[0] && find_meta_node(path, sizeof(path))) {
		fprintf(stderr, "no dc30-meta video node found\n");
		return 1;
	}

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	req.count = NBUFS;
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS");
		return 1;
	}
	for (i = 0; i < (int)req.count; i++) {
		struct v4l2_buffer b = { .index = i, .type = type,
					 .memory = V4L2_MEMORY_MMAP };

		if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
			perror("VIDIOC_QUERYBUF");
			return 1;
		}
		lens[i] = b.length;
		maps[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, fd,
			       b.m.offset);
		if (maps[i] == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
			perror("VIDIOC_QBUF");
			return 1;
		}
	}
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON");
		return 1;
	}
	fprintf(stderr, "%s: streaming, waiting for %u records\n", path, want);

	if (verbose)
		printf("   seq  field0 ms    first  audio frame (f0)  +frames  flags\n");

	while (got < want) {
		struct v4l2_buffer b = { .type = type,
					 .memory = V4L2_MEMORY_MMAP };
		const struct dc30_meta *m;
		const struct dc30_meta_field *f0;
		int gap;

		struct pollfd pfd = { .fd = fd, .events = POLLIN };

		/* Records only come while the video node streams; its start
		 * takes a while (MJPEG: codec set-up and settling).
		 */
		if (poll(&pfd, 1, got ? 2000 : 10000) == 0) {
			fprintf(stderr, "no record for %d s, stopping\n",
				got ? 2 : 10);
			break;
		}
		if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
			perror("VIDIOC_DQBUF");
			break;
		}
		m = maps[b.index];
		if (m->version != DC30_META_VERSION ||
		    m->size != sizeof(*m)) {
			fprintf(stderr, "record version %u size %u, expected %u/%zu\n",
				m->version, m->size, DC30_META_VERSION,
				sizeof(*m));
			return 1;
		}
		f0 = &m->field[0];
		got++;
		last = *m;

		gap = prev_seq >= 0 && m->sequence != prev_seq + 1;
		if (gap) {
			gaps++;
			lost += m->sequence - prev_seq - 1;
		}
		prev_seq = m->sequence;

		if ((f0->flags & DC30_META_FIELD_AUDIO) &&
		    (m->field[1].flags & DC30_META_FIELD_AUDIO)) {
			double apos;

			if (m->audio_frame_bytes)
				frame_bytes = m->audio_frame_bytes;
			apos = f0->audio_pos / frame_bytes;
			if (have_prev_pos && f0->audio_epoch != prev_epoch) {
				/* new ALSA stream: positions restart */
				epochs++;
				memset(&fit_seq, 0, sizeof(fit_seq));
				memset(&fit_time, 0, sizeof(fit_time));
				have_prev_pos = 0;
			}
			fit_add(&fit_seq, m->sequence, apos);
			fit_add(&fit_time, f0->vsync_ns / 1e9, apos);
			if (verbose)
				printf("%6u %11.3f %6s %17.2f %8.2f  %s%s\n",
				       m->sequence, f0->vsync_ns / 1e6,
				       (f0->flags & DC30_META_FIELD_BOTTOM) ?
				       "bottom" : "top", apos,
				       have_prev_pos ?
				       (f0->audio_pos - prev_pos) / frame_bytes : 0.0,
				       m->audio_clock == DC30_META_CLOCK_VIDEOLOCK ?
				       "lock" : "xtal",
				       gap ? " GAP" : "");
			prev_pos = f0->audio_pos;
			prev_epoch = f0->audio_epoch;
			have_prev_pos = 1;
		} else {
			no_audio++;
			if (verbose)
				printf("%6u %11.3f %6s   (no audio)\n", m->sequence,
				       f0->vsync_ns / 1e6,
				       (f0->flags & DC30_META_FIELD_BOTTOM) ?
				       "bottom" : "top");
		}

		if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
			perror("VIDIOC_QBUF");
			break;
		}
	}

	xioctl(fd, VIDIOC_STREAMOFF, &type);

	printf("\nrecords          %u (sequence gaps %lld, %lld frames lost)\n",
	       got, gaps, lost);
	printf("without audio    %u, ALSA restarts seen %u\n", no_audio, epochs);
	printf("driver counters  dropped %u, no field %u, late irq %u, field order %u, fifo overflows %u\n",
	       last.frames_dropped, last.no_field, last.late_irq,
	       last.field_order, last.fifo_overflows);
	if (last.audio_rate)
		printf("audio            %u Hz nominal, %s clock\n",
		       last.audio_rate,
		       last.audio_clock == DC30_META_CLOCK_VIDEOLOCK ?
		       "video lock" : "crystal");
	{
		double slope, sd, rslope, rsd;

		if (!fit_result(&fit_seq, &slope, &sd)) {
			printf("audio per frame  %.4f audio frames per video frame (nominal %.1f at 25 fps), scatter %.2f frames rms\n",
			       slope, last.audio_rate ? last.audio_rate / 25.0 : 0,
			       sd);
		}
		if (!fit_result(&fit_time, &rslope, &rsd))
			printf("audio per second %.2f audio frames per second of video time\n",
			       rslope);
	}
	for (i = 0; i < (int)req.count; i++)
		munmap(maps[i], lens[i]);
	close(fd);
	return 0;
}
