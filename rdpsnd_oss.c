/* -*- c-basic-offset: 8 -*-
   rdesktop: A Remote Desktop Protocol client.
   Sound Channel Process Functions - Open Sound System
   Copyright (C) Matthew Chapman 2003
   Copyright (C) GuoJunBo guojunbo@ict.ac.cn 2003

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* 
   This is a workaround for Esound bug 312665. 
   FIXME: Remove this when Esound is fixed. 
*/
#ifdef _FILE_OFFSET_BITS
#undef _FILE_OFFSET_BITS
#endif

#include "rdesktop.h"
#include "rdpsnd.h"
#include "rdpsnd_dsp.h"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

#define DEFAULTDEVICE	"/dev/dsp"
#define MAX_LEN		512

static int snd_rate;
static short samplewidth;
static char *dsp_dev;
static struct audio_driver oss_driver;

BOOL
oss_open(void)
{
	if ((g_dsp_fd = open(dsp_dev, O_WRONLY)) == -1)
	{
		perror(dsp_dev);
		return False;
	}

	return True;
}

void
oss_close(void)
{
	close(g_dsp_fd);
}

BOOL
oss_format_supported(WAVEFORMATEX * pwfx)
{
	if (pwfx->wFormatTag != WAVE_FORMAT_PCM)
		return False;
	if ((pwfx->nChannels != 1) && (pwfx->nChannels != 2))
		return False;
	if ((pwfx->wBitsPerSample != 8) && (pwfx->wBitsPerSample != 16))
		return False;

	return True;
}

BOOL
oss_set_format(WAVEFORMATEX * pwfx)
{
	int stereo, format, fragments;
	static BOOL driver_broken = False;

	ioctl(g_dsp_fd, SNDCTL_DSP_RESET, NULL);
	ioctl(g_dsp_fd, SNDCTL_DSP_SYNC, NULL);

	if (pwfx->wBitsPerSample == 8)
		format = AFMT_U8;
	else if (pwfx->wBitsPerSample == 16)
		format = AFMT_S16_LE;

	samplewidth = pwfx->wBitsPerSample / 8;

	if (ioctl(g_dsp_fd, SNDCTL_DSP_SETFMT, &format) == -1)
	{
		perror("SNDCTL_DSP_SETFMT");
		close(g_dsp_fd);
		return False;
	}

	if (pwfx->nChannels == 2)
	{
		stereo = 1;
		samplewidth *= 2;
	}
	else
	{
		stereo = 0;
	}

	if (ioctl(g_dsp_fd, SNDCTL_DSP_STEREO, &stereo) == -1)
	{
		perror("SNDCTL_DSP_CHANNELS");
		close(g_dsp_fd);
		return False;
	}

	snd_rate = pwfx->nSamplesPerSec;
	if (ioctl(g_dsp_fd, SNDCTL_DSP_SPEED, &snd_rate) == -1)
	{
		perror("SNDCTL_DSP_SPEED");
		close(g_dsp_fd);
		return False;
	}

	/* try to get 12 fragments of 2^12 bytes size */
	fragments = (12 << 16) + 12;
	ioctl(g_dsp_fd, SNDCTL_DSP_SETFRAGMENT, &fragments);

	if (!driver_broken)
	{
		audio_buf_info info;

		memset(&info, 0, sizeof(info));
		if (ioctl(g_dsp_fd, SNDCTL_DSP_GETOSPACE, &info) == -1)
		{
			perror("SNDCTL_DSP_GETOSPACE");
			close(g_dsp_fd);
			return False;
		}

		if (info.fragments == 0 || info.fragstotal == 0 || info.fragsize == 0)
		{
			fprintf(stderr,
				"Broken OSS-driver detected: fragments: %d, fragstotal: %d, fragsize: %d\n",
				info.fragments, info.fragstotal, info.fragsize);
			driver_broken = True;
		}
	}

	return True;
}

void
oss_volume(uint16 left, uint16 right)
{
	uint32 volume;

	volume = left / (65536 / 100);
	volume |= right / (65536 / 100) << 8;

	if (ioctl(g_dsp_fd, MIXER_WRITE(SOUND_MIXER_PCM), &volume) == -1)
	{
		perror("MIXER_WRITE(SOUND_MIXER_PCM)");
		oss_driver.wave_out_volume = rdpsnd_dsp_softvol_set;
		return;
	}
}

void
oss_play(void)
{
	struct audio_packet *packet;
	ssize_t len;
	STREAM out;
	static long startedat_us;
	static long startedat_s;
	static BOOL started = False;
	struct timeval tv;

	if (rdpsnd_queue_empty())
	{
		g_dsp_busy = 0;
		return;
	}

	packet = rdpsnd_queue_current_packet();
	out = &packet->s;

	if (!started)
	{
		gettimeofday(&tv, NULL);
		startedat_us = tv.tv_usec;
		startedat_s = tv.tv_sec;
		started = True;
	}

	len = out->end - out->p;

	len = write(g_dsp_fd, out->p, (len > MAX_LEN) ? MAX_LEN : len);
	if (len == -1)
	{
		if (errno != EWOULDBLOCK)
			perror("write audio");
		g_dsp_busy = 1;
		return;
	}

	out->p += len;
	if (out->p == out->end)
	{
		long long duration;
		long elapsed;

		gettimeofday(&tv, NULL);
		duration = (out->size * (1000000 / (samplewidth * snd_rate)));
		elapsed = (tv.tv_sec - startedat_s) * 1000000 + (tv.tv_usec - startedat_us);

		if (elapsed >= (duration * 85) / 100)
		{
			/* We need to add 50 to tell windows that time has passed while
			 * playing this packet */
			rdpsnd_send_completion(packet->tick + 50, packet->index);
			rdpsnd_queue_next();
			started = False;
		}
		else
		{
			g_dsp_busy = 1;
			return;
		}
	}
	g_dsp_busy = 1;
	return;
}

struct audio_driver *
oss_register(char *options)
{
	oss_driver.wave_out_write = rdpsnd_queue_write;
	oss_driver.wave_out_open = oss_open;
	oss_driver.wave_out_close = oss_close;
	oss_driver.wave_out_format_supported = oss_format_supported;
	oss_driver.wave_out_set_format = oss_set_format;
	oss_driver.wave_out_volume = oss_volume;
	oss_driver.wave_out_play = oss_play;
	oss_driver.name = xstrdup("oss");
	oss_driver.description =
		xstrdup("OSS output driver, default device: " DEFAULTDEVICE " or $AUDIODEV");
	oss_driver.need_byteswap_on_be = 0;
	oss_driver.next = NULL;

	if (options)
	{
		dsp_dev = xstrdup(options);
	}
	else
	{
		dsp_dev = getenv("AUDIODEV");

		if (dsp_dev == NULL)
		{
			dsp_dev = xstrdup(DEFAULTDEVICE);
		}
	}

	return &oss_driver;
}
