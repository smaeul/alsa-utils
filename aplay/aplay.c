/*
 *  aplay.c - plays and records
 *
 *      CREATIVE LABS CHANNEL-files
 *      Microsoft WAVE-files
 *      SPARC AUDIO .AU-files
 *      Raw Data
 *
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *  Based on vplay program by Michael Beck
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdio.h>
#include <malloc.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <sys/asoundlib.h>
#include <assert.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include "aconfig.h"
#include "formats.h"
#include "version.h"

#define DEFAULT_SPEED 		8000

#define FORMAT_DEFAULT		-1
#define FORMAT_RAW		0
#define FORMAT_VOC		1
#define FORMAT_WAVE		2
#define FORMAT_AU		3

/* global data */

static ssize_t (*read_func)(snd_pcm_t *handle, void *buffer, size_t size);
static ssize_t (*write_func)(snd_pcm_t *handle, const void *buffer, size_t size);
static ssize_t (*readv_func)(snd_pcm_t *handle, const struct iovec *vector, unsigned long count);
static ssize_t (*writev_func)(snd_pcm_t *handle, const struct iovec *vector, unsigned long count);

static char *command;
static snd_pcm_t *handle;
static snd_pcm_info_t cinfo;
static snd_pcm_params_info_t cpinfo;
static snd_pcm_format_t rformat, format;
static snd_pcm_setup_t setup;
static int timelimit = 0;
static int quiet_mode = 0;
static int file_type = FORMAT_DEFAULT;
static int mode = SND_PCM_MODE_FRAGMENT;
static int open_mode = 0;
static int stream = SND_PCM_STREAM_PLAYBACK;
static int mmap_flag = 0;
static snd_pcm_mmap_control_t *mmap_control = NULL;
static snd_pcm_mmap_status_t *mmap_status = NULL;
static char *mmap_data = NULL;
static int nonblock = 0;
static char *audiobuf = NULL;
static int align = 1;
static int buffer_size = -1;
static int frag_length = 125;
static int buffer_length = 500;
static int min_avail = 50;
static int dump_pcm = 0;
static int buffer_pos = 0;
static size_t bits_per_sample, bits_per_frame;
static size_t buffer_bytes;

static int count;
static int vocmajor, vocminor;

/* needed prototypes */

static void playback(char *filename);
static void capture(char *filename);
static void playbackv(char **filenames, unsigned int count);
static void capturev(char **filenames, unsigned int count);

static void begin_voc(int fd, size_t count);
static void end_voc(int fd);
static void begin_wave(int fd, size_t count);
static void end_wave(int fd);
static void begin_au(int fd, size_t count);

struct fmt_capture {
	void (*start) (int fd, size_t count);
	void (*end) (int fd);
	char *what;
} fmt_rec_table[] = {
	{	NULL,		end_wave,	"raw data"	},
	{	begin_voc,	end_voc,	"VOC"		},
	{	begin_wave,	end_wave,	"WAVE"		},
	{	begin_au,	end_wave,	"Sparc Audio"	}
};

static void check_new_format(snd_pcm_format_t * format)
{
        if (cpinfo.rates & (SND_PCM_RATE_CONTINUOUS|SND_PCM_RATE_KNOT)) {
                if (format->rate < cpinfo.min_rate ||
                    format->rate > cpinfo.max_rate) {
			fprintf(stderr, "%s: unsupported rate %iHz (valid range is %iHz-%iHz)\n", command, format->rate, cpinfo.min_rate, cpinfo.max_rate);
			exit(EXIT_FAILURE);
		}
        } else {
		unsigned int r;
                switch (format->rate) {
                case 8000:      r = SND_PCM_RATE_8000; break;
                case 11025:     r = SND_PCM_RATE_11025; break;
                case 16000:     r = SND_PCM_RATE_16000; break;
                case 22050:     r = SND_PCM_RATE_22050; break;
                case 32000:     r = SND_PCM_RATE_32000; break;
                case 44100:     r = SND_PCM_RATE_44100; break;
                case 48000:     r = SND_PCM_RATE_48000; break;
                case 88200:     r = SND_PCM_RATE_88200; break;
                case 96000:     r = SND_PCM_RATE_96000; break;
                case 176400:    r = SND_PCM_RATE_176400; break;
                case 192000:    r = SND_PCM_RATE_192000; break;
                default:        r = 0; break;
                }
                if (!(cpinfo.rates & r)) {
			fprintf(stderr, "%s: unsupported rate %iHz\n", command, format->rate);
			exit(EXIT_FAILURE);
		}
	}
	if (cpinfo.min_channels > format->channels || cpinfo.max_channels < format->channels) {
		fprintf(stderr, "%s: unsupported number of channels %i (valid range is %i-%i)\n", command, format->channels, cpinfo.min_channels, cpinfo.max_channels);
		exit(EXIT_FAILURE);
	}
	if (!(cpinfo.formats & (1 << format->format))) {
		fprintf(stderr, "%s: unsupported format %s\n", command, snd_pcm_format_name(format->format));
		exit(EXIT_FAILURE);
	}
	if (format->channels > 1) {
		if (format->interleave) {
			if (!(cinfo.flags & SND_PCM_INFO_INTERLEAVE)) {
				fprintf(stderr, "%s: unsupported interleaved format\n", command);
				exit(EXIT_FAILURE);
			}
		} else if (!(cinfo.flags & SND_PCM_INFO_NONINTERLEAVE)) {
			fprintf(stderr, "%s: unsupported non interleaved format\n", command);
			exit(EXIT_FAILURE);
		}
	}
}

static void usage(char *command)
{
	int k;
	fprintf(stderr, "\
Usage: %s [OPTION]... [FILE]...

--help                   help
--version                print current version
--list-devices           list all soundcards and digital audio devices
-C, --card=#             select card # or card id (0-%i), defaults to 0
-D, --device=#           select device #, defaults to 0
-S, --subdevice=#        select subdevice #, defaults to first available
-P, --direct             don't use plugins for this PCM
-H, --pcm-channels=#     channels for last specified PCM 
-h, --bind-channel=C,S   bind stream channel C to PCM channel S
-q, --quiet              quiet mode
-t, --file-type TYPE     file type (voc, wav or raw)
-c, --channels=#         channels
-f, --format=FORMAT      sample format (case insensitive)
-r, --rate=#             sample rate
-d, --duration=#         interrupt after # seconds
-e, --frame-mode         use frame mode instead of default fragment mode
-M, --mmap               mmap stream
-Q, --multi-direct       don't use plugins on top of multi
-N, --nonblock           nonblocking mode
-F, --fragment-length=#  fragment length is # milliseconds
-B, --buffer-length=#    buffer length is # milliseconds
-A, --min-avail=#        min available space for wakeup is # milliseconds
-v, --verbose            show PCM structure and setup
-I, --separate-channels  one file for each channel
", command, snd_cards()-1);
	fprintf(stderr, "Recognized sample formats are:");
	for (k = 0; k < 32; ++k) {
		const char *s = snd_pcm_format_name(k);
		if (s)
			fprintf(stderr, " %s", s);
	}
	fprintf(stderr, "\nSome of these may not be available on selected hardware\n");
	fprintf(stderr, "The availabled format shortcuts are:\n");
	fprintf(stderr, "cd (16 bit little endian, 44100, stereo)\n");
	fprintf(stderr, "dat (16 bit little endian, 48000, stereo)\n");
}

static void device_list(void)
{
	snd_ctl_t *handle;
	int card, err, dev, idx;
	unsigned int mask;
	snd_ctl_hw_info_t info;
	snd_pcm_info_t pcminfo;

	mask = snd_cards_mask();
	if (!mask) {
		fprintf(stderr, "%s: no soundcards found...\n", command);
		return;
	}
	for (card = 0; card < SND_CARDS; card++) {
		if (!(mask & (1 << card)))
			continue;
		if ((err = snd_ctl_open(&handle, card)) < 0) {
			fprintf(stderr, "Error: control open (%i): %s\n", card, snd_strerror(err));
			continue;
		}
		if ((err = snd_ctl_hw_info(handle, &info)) < 0) {
			fprintf(stderr, "Error: control hardware info (%i): %s\n", card, snd_strerror(err));
			snd_ctl_close(handle);
			continue;
		}
		for (dev = 0; dev < info.pcmdevs; dev++) {
			pcminfo.device = dev;
			pcminfo.stream = -stream - 1;
			pcminfo.subdevice = -1;
			if ((err = snd_ctl_pcm_info(handle, &pcminfo)) < 0) {
				fprintf(stderr, "Error: control digital audio info (%i): %s\n", card, snd_strerror(err));
				continue;
			}
			if (pcminfo.stream != stream)
				continue;
			fprintf(stderr, "%s: %i [%s] / #%i: %s\n",
			       info.name,
			       card + 1,
			       info.id,
			       dev,
			       pcminfo.name);
			fprintf(stderr, "  Subdevices: %i/%i\n", pcminfo.subdevices_avail, pcminfo.subdevices_count);
			for (idx = 0; idx < pcminfo.subdevices_count; idx++) {
				pcminfo.subdevice = idx;
				if ((err = snd_ctl_pcm_info(handle, &pcminfo)) < 0) {
					fprintf(stderr, "Error: control digital audio playback info (%i): %s\n", card, snd_strerror(err));
				} else {
					fprintf(stderr, "  Subdevice #%i: %s\n", idx, pcminfo.subname);
				}
			}
		}
		snd_ctl_close(handle);
	}
}

static void version(void)
{
	fprintf(stderr, "%s: version " SND_UTIL_VERSION_STR " by Jaroslav Kysela <perex@suse.cz>\n", command);
}

#define OPT_HELP 1
#define OPT_VERSION 2

int main(int argc, char *argv[])
{
	int option_index;
	char *short_options = "lC:D:S:H:h:qt:c:f:r:d:eMPQA:B:F:NvI";
	static struct option long_options[] = {
		{"help", 0, 0, OPT_HELP},
		{"version", 0, 0, OPT_VERSION},
		{"list-devices", 0, 0, 'l'},
		{"card", 1, 0, 'C'},
		{"device", 1, 0, 'D'},
		{"subdevice", 1, 0, 'S'},
		{"pcm-channels", 1, 0, 'H'},
		{"bind-channel", 1, 0, 'h'},
		{"quiet", 0, 0, 'q'},
		{"file-type", 1, 0, 't'},
		{"channels", 1, 0, 'c'},
		{"format", 1, 0, 'f'},
		{"rate", 1, 0, 'r'},
		{"duration", 1, 0 ,'d'},
		{"frame-mode", 0, 0, 'e'},
		{"mmap", 0, 0, 'M'},
		{"direct", 0, 0, 'P'},
		{"multi-direct", 0, 0, 'Q'},
		{"nonblock", 0, 0, 'N'},
		{"fragment-length", 1, 0, 'F'},
		{"buffer-length", 1, 0, 'B'},
		{"min-avail", 1, 0, 'A'},
		{"verbose", 0, 0, 'v'},
		{"separate-channels", 0, 0, 'I'},
		{0, 0, 0, 0}
	};
	int binds_pcm[32];
	int binds_client_channel[32];
	int binds_slave_channel[32];
	int pcms_card[32];
	int pcms_dev[32];
	int pcms_subdev[32];
	int pcms_channels[32];
	int pcms_direct[32];
	int direct = 0;
	int multi_direct = 0;
		
	int pcm_card = 0, pcm_dev = 0, pcm_subdev = -1, pcm_channels = -1;
	int tmp, err, c, client_channel, slave_channel;
	int pcm;
	int pcms_count = 0, binds_count = 0;
	int multi;
	char *ptr, *beg;

	int get_pcm() {
		int pcm;
		if (pcm_channels < 0)
			pcm_channels = rformat.channels;
		for (pcm = 0; pcm < pcms_count; ++pcm) {
			if (pcms_card[pcm] == pcm_card &&
			    pcms_dev[pcm] == pcm_dev &&
			    pcms_subdev[pcm] == pcm_subdev)
				break;
		}
		if (pcm == pcms_count) {
			pcms_card[pcm] = pcm_card;
			pcms_dev[pcm] = pcm_dev;
			pcms_subdev[pcm] = pcm_subdev;
			pcms_channels[pcm] = pcm_channels;
			pcms_direct[pcm] = direct;
			direct = 0;
			pcms_count++;
		} else if (pcm_channels != pcms_channels[pcm]) {
			fprintf(stderr, "Error: different channels count specified for the same pcm\n");
			exit(1);
		}
		return pcm;
	}

	command = argv[0];
	file_type = FORMAT_DEFAULT;
	if (strstr(argv[0], "arecord")) {
		stream = SND_PCM_STREAM_CAPTURE;
		file_type = FORMAT_WAVE;
		command = "arecord";
	} else if (strstr(argv[0], "aplay")) {
		stream = SND_PCM_STREAM_PLAYBACK;
		command = "aplay";
	} else {
		fprintf(stderr, "Error: command should be named either arecord or aplay\n");
		return 1;
	}

	buffer_size = -1;
	memset(&rformat, 0, sizeof(rformat));
	rformat.interleave = 1;
	rformat.format = SND_PCM_SFMT_U8;
	rformat.rate = DEFAULT_SPEED;
	rformat.channels = 1;

	while ((c = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) {
		switch (c) {
		case OPT_HELP:
			usage(command);
			return 0;
		case OPT_VERSION:
			version();
			return 0;
		case 'l':
			device_list();
			return 0;
		case 'C':
			pcm_card = snd_card_name(optarg);
			if (pcm_card < 0) {
				fprintf(stderr, "Error: soundcard '%s' not found\n", optarg);
				return 1;
			}
			break;
		case 'D':
			pcm_dev = atoi(optarg);
			if (pcm_dev < 0 || pcm_dev > 32) {
				fprintf(stderr, "Error: device %i is invalid\n", pcm_dev);
				return 1;
			}
			break;
		case 'S':
			pcm_subdev = atoi(optarg);
			if (pcm_subdev < 0 || pcm_subdev > 32) {
				fprintf(stderr, "Error: subdevice %i is invalid\n", pcm_subdev);
				return 1;
			}
			break;
		case 'H':
			pcm_channels = atoi(optarg);
			if (pcm_channels < 1 || pcm_channels > 32) {
				fprintf(stderr, "Error: value %i for channels is invalid\n", pcm_channels);
				return 1;
			}
			break;
		case 'h':
			client_channel = strtol(optarg, &ptr, 10);
			if (*ptr != ',' || ptr == optarg) {
				fprintf(stderr, "Error: invalid channel binding syntax\n");
				return 1;
			}
			beg = ptr + 1;
			slave_channel = strtol(beg, &ptr, 10);
			if (*ptr || ptr == optarg) {
				fprintf(stderr, "Error: invalid channel binding syntax\n");
				return 1;
			}
			if (client_channel >= rformat.channels) {
				fprintf(stderr, "Error: attempt to bind unavailable channel %d\n", client_channel);
				return 1;
			}
			if (slave_channel >= pcm_channels) {
				fprintf(stderr, "Error: attempt to bind to an unavailable PCM channel %d\n", slave_channel);
				return 1;
			}
			pcm = get_pcm();
			binds_pcm[binds_count] = pcm;
			binds_client_channel[binds_count] = client_channel;
			binds_slave_channel[binds_count] = slave_channel;
			++binds_count;
			break;
		case 'q':
			quiet_mode = 1;
			break;
		case 't':
			if (strcasecmp(optarg, "raw") == 0)
				file_type = FORMAT_RAW;
			else if (strcasecmp(optarg, "voc") == 0)
				file_type = FORMAT_VOC;
			else if (strcasecmp(optarg, "wav") == 0)
				file_type = FORMAT_WAVE;
			else {
				fprintf(stderr, "Error: unrecognized file format %s\n", optarg);
				return 1;
			}
			break;
		case 'c':
			rformat.channels = atoi(optarg);
			if (rformat.channels < 1 || rformat.channels > 32) {
				fprintf(stderr, "Error: value %i for channels is invalid\n", rformat.channels);
				return 1;
			}
			break;
		case 'f':
			if (strcasecmp(optarg, "cd") == 0) {
				rformat.format = SND_PCM_SFMT_S16_LE;
				rformat.rate = 44100;
				rformat.channels = 2;
			} else if (strcasecmp(optarg, "dat") == 0) {
				rformat.format = SND_PCM_SFMT_S16_LE;
				rformat.rate = 48000;
				rformat.channels = 2;
			} else {
				rformat.format = snd_pcm_format_value(optarg);
				if (rformat.format < 0) {
					fprintf(stderr, "Error: wrong extended format '%s'\n", optarg);
					exit(EXIT_FAILURE);
				}
			}
			break;
		case 'r':
			tmp = atoi(optarg);
			if (tmp < 300)
				tmp *= 1000;
			rformat.rate = tmp;
			if (tmp < 2000 || tmp > 128000) {
				fprintf(stderr, "Error: bad speed value %i\n", tmp);
				return 1;
			}
			break;
		case 'd':
			timelimit = atoi(optarg);
			break;
		case 'e':
			mode = SND_PCM_MODE_FRAME;
			break;
		case 'M':
			mmap_flag = 1;
			break;
		case 'P':
			direct = 1;
			break;
		case 'Q':
			multi_direct = 1;
			break;
		case 'N':
			nonblock = 1;
			open_mode |= SND_PCM_NONBLOCK;
			break;
		case 'F':
			frag_length = atoi(optarg);
			break;
		case 'B':
			buffer_length = atoi(optarg);
			break;
		case 'A':
			min_avail = atoi(optarg);
			break;
		case 'v':
			dump_pcm = 1;
			break;
		case 'I':
			rformat.interleave = 0;
			break;
		default:
			fprintf(stderr, "Try `%s --help' for more information.\n", command);
			return 1;
		}
	}

	if (binds_count == 0) {
		pcm = get_pcm();
		for (c = 0; c < rformat.channels; ++c) {
			if (c > pcm_channels) {
				fprintf(stderr, "Error: attempt to bind to an unavailable PCM channel %d\n", c);
				return 1;
			}
			binds_pcm[binds_count] = pcm;
			binds_client_channel[binds_count] = c;
			binds_slave_channel[binds_count] = c;
			binds_count++;
		}
	}

	if (!quiet_mode)
		version();

	assert(pcms_count > 0);

	multi = 0;
	if (pcms_count != 1 || rformat.channels != binds_count || 
	    pcm_channels != binds_count)
		multi = 1;
	else {
		char mask[binds_count];
		memset(mask, 0, sizeof(mask));
		for (c = 0; c < binds_count; ++c) {
			if (binds_client_channel[c] != binds_slave_channel[c]) {
				multi = 1;
				break;
			}
			if (mask[c]) {
				multi = 1;
				break;
			}
			mask[c] = 1;
		}
	}
	if (!quiet_mode) {
		for (pcm = 0; pcm < pcms_count; ++pcm) {
			char *cardname;
			if ((err = snd_card_get_longname(pcms_card[pcm], &cardname)) < 0) {
				fprintf(stderr, "Error: unable to obtain longname: %s\n", snd_strerror(err));
				return 1;
			}
			fprintf(stderr, "Using soundcard '%s'\n", cardname);
			free(cardname);
		}
	}

	{
		snd_pcm_t *handles[pcms_count];
		for (pcm = 0; pcm < pcms_count; ++pcm) {
			if (pcms_direct[pcm])
				err = snd_pcm_hw_open_subdevice(&handles[pcm], pcms_card[pcm], pcms_dev[pcm], pcms_subdev[pcm], stream, open_mode);
			else
				err = snd_pcm_plug_open_subdevice(&handles[pcm], pcms_card[pcm], pcms_dev[pcm], pcms_subdev[pcm], stream, open_mode);
			if (err < 0) {
				fprintf(stderr, "Error: audio open error: %s\n", snd_strerror(err));
				return 1;
			}
		}
		if (multi) {
			err = snd_pcm_multi_create(&handle, pcms_count, handles, pcms_channels, binds_count, binds_client_channel, binds_pcm, binds_slave_channel, 1);
			if (err < 0) {
				fprintf(stderr, "Error: audio open error: %s\n", snd_strerror(err));
				return 1;
			}
			if (!multi_direct) {
				snd_pcm_t *h = handle;
				err = snd_pcm_plug_create(&handle, h, 1);
				if (err < 0) {
					fprintf(stderr, "Error: audio open error: %s\n", snd_strerror(err));
					return 1;
				}
			}
		} else {
			handle = handles[0];
		}
	}
	if (nonblock) {
		err = snd_pcm_nonblock(handle, 1);
		if (err < 0) {
			fprintf(stderr, "nonblock setting error: %s\n", snd_strerror(err));
			return 1;
		}
	}
	memset(&cinfo, 0, sizeof(cinfo));
	if ((err = snd_pcm_info(handle, &cinfo)) < 0) {
		fprintf(stderr, "Error: info error: %s\n", snd_strerror(err));
		return 1;
	}

	memset(&cpinfo, 0, sizeof(cpinfo));
	if ((err = snd_pcm_params_info(handle, &cpinfo)) < 0) {
		fprintf(stderr, "Error: params info error: %s\n", snd_strerror(err));
		return 1;
	}

	buffer_size = 1024;
	format = rformat;

	audiobuf = (char *)malloc(1024);
	if (audiobuf == NULL) {
		fprintf(stderr, "Error: not enough memory\n");
		return 1;
	}

	if (mmap_flag) {
		write_func = snd_pcm_mmap_write;
		read_func = snd_pcm_mmap_read;
		writev_func = snd_pcm_mmap_writev;
		readv_func = snd_pcm_mmap_readv;
	} else {
		write_func = snd_pcm_write;
		read_func = snd_pcm_read;
		writev_func = snd_pcm_writev;
		readv_func = snd_pcm_readv;
	}

	if (rformat.interleave) {
		if (optind > argc - 1) {
			if (stream == SND_PCM_STREAM_PLAYBACK)
				playback(NULL);
			else
				capture(NULL);
		} else {
			while (optind <= argc - 1) {
				if (stream == SND_PCM_STREAM_PLAYBACK)
					playback(argv[optind++]);
				else
					capture(argv[optind++]);
			}
		}
	} else {
		if (stream == SND_PCM_STREAM_PLAYBACK)
			playbackv(&argv[optind], argc - optind);
		else
			capturev(&argv[optind], argc - optind);
	}
	snd_pcm_close(handle);
	return EXIT_SUCCESS;
}

/*
 * Test, if it is a .VOC file and return >=0 if ok (this is the length of rest)
 *                                       < 0 if not 
 */
static int test_vocfile(void *buffer)
{
	VocHeader *vp = buffer;

	if (strstr(vp->magic, VOC_MAGIC_STRING)) {
		vocminor = vp->version & 0xFF;
		vocmajor = vp->version / 256;
		if (vp->version != (0x1233 - vp->coded_ver))
			return -2;	/* coded version mismatch */
		return vp->headerlen - sizeof(VocHeader);	/* 0 mostly */
	}
	return -1;		/* magic string fail */
}

/*
 * test, if it's a .WAV file, > 0 if ok (and set the speed, stereo etc.)
 *                            == 0 if not
 * Value returned is bytes to be discarded.
 */
static int test_wavefile(void *buffer, size_t size)
{
	WaveHeader *h = buffer;
	WaveFmtHeader *f;
	WaveChunkHeader *c;

	if (h->magic != WAV_RIFF || h->type != WAV_WAVE)
		return 0;
	c = (WaveChunkHeader*)((char *)buffer + sizeof(WaveHeader));
	while (c->type != WAV_FMT) {
		c = (WaveChunkHeader*)((char*)c + sizeof(*c) + LE_INT(c->length));
		if ((char *)c + sizeof(*c) > (char*) buffer + size) {
			fprintf(stderr, "%s: cannot found WAVE fmt chunk\n", command);
			exit(EXIT_FAILURE);
		}
	}
	f = (WaveFmtHeader*) c;
	if (LE_SHORT(f->format) != WAV_PCM_CODE) {
		fprintf(stderr, "%s: can't play not PCM-coded WAVE-files\n", command);
		exit(EXIT_FAILURE);
	}
	if (LE_SHORT(f->modus) < 1) {
		fprintf(stderr, "%s: can't play WAVE-files with %d tracks\n",
			command, LE_SHORT(f->modus));
		exit(EXIT_FAILURE);
	}
	format.channels = LE_SHORT(f->modus);
	switch (LE_SHORT(f->bit_p_spl)) {
	case 8:
		format.format = SND_PCM_SFMT_U8;
		break;
	case 16:
		format.format = SND_PCM_SFMT_S16_LE;
		break;
	default:
		fprintf(stderr, "%s: can't play WAVE-files with sample %d bits wide\n",
			command, LE_SHORT(f->bit_p_spl));
		exit(EXIT_FAILURE);
	}
	format.rate = LE_INT(f->sample_fq);
	while (c->type != WAV_DATA) {
		c = (WaveChunkHeader*)((char*)c + sizeof(*c) + LE_INT(c->length));
		if ((char *)c + sizeof(*c) > (char*) buffer + size) {
			fprintf(stderr, "%s: cannot found WAVE data chunk\n", command);
			exit(EXIT_FAILURE);
		}
	}

	if (LE_INT(c->length) < count)
		count = LE_INT(c->length);
	check_new_format(&format);
	return (char *)c + sizeof(*c) - (char *) buffer;
}

/*

 */

static int test_au(int fd, void *buffer)
{
	AuHeader *ap = buffer;

	if (ap->magic != AU_MAGIC)
		return -1;
	if (BE_INT(ap->hdr_size) > 128 || BE_INT(ap->hdr_size) < 24)
		return -1;
	count = BE_INT(ap->data_size);
	switch (BE_INT(ap->encoding)) {
	case AU_FMT_ULAW:
		format.format = SND_PCM_SFMT_MU_LAW;
		break;
	case AU_FMT_LIN8:
		format.format = SND_PCM_SFMT_U8;
		break;
	case AU_FMT_LIN16:
		format.format = SND_PCM_SFMT_U16_LE;
		break;
	default:
		return -1;
	}
	format.rate = BE_INT(ap->sample_rate);
	if (format.rate < 2000 || format.rate > 256000)
		return -1;
	format.channels = BE_INT(ap->channels);
	if (format.channels < 1 || format.channels > 128)
		return -1;
	if (read(fd, buffer + sizeof(AuHeader), BE_INT(ap->hdr_size) - sizeof(AuHeader)) < 0) {
		fprintf(stderr, "%s: read error\n", command);
		exit(EXIT_FAILURE);
	}
	check_new_format(&format);
	return 0;
}

static void set_params(void)
{
	snd_pcm_params_t params;

	align = (snd_pcm_format_physical_width(format.format) + 7) / 8;

#if 0
	if (mmap_flag)
		snd_pcm_munmap(handle, stream);
#endif
	snd_pcm_flush(handle);		/* to be in right state */

	memset(&params, 0, sizeof(params));
	params.mode = mode;
	params.format = format;
	if (stream == SND_PCM_STREAM_PLAYBACK) {
		params.start_mode = SND_PCM_START_FULL;
	} else {
		params.start_mode = SND_PCM_START_DATA;
	}
	params.xrun_mode = SND_PCM_XRUN_FLUSH;
	params.frag_size = format.rate * frag_length / 1000;
	params.buffer_size = format.rate * buffer_length / 1000;
	params.frames_min = format.rate * min_avail / 1000;
	params.frames_xrun_max = 0;
	params.fill_mode = SND_PCM_FILL_SILENCE;
	params.frames_fill_max = 1024;
	params.frames_xrun_max = 0;
	if (snd_pcm_params(handle, &params) < 0) {
		fprintf(stderr, "%s: unable to set params\n", command);
		exit(EXIT_FAILURE);
	}
	if (mmap_flag) {
		if (snd_pcm_mmap(handle, &mmap_status, &mmap_control, (void **)&mmap_data)<0) {
			fprintf(stderr, "%s: unable to mmap memory\n", command);
			exit(EXIT_FAILURE);
		}
	}
	if (snd_pcm_prepare(handle) < 0) {
		fprintf(stderr, "%s: unable to prepare PCM\n", command);
		exit(EXIT_FAILURE);
	}
	memset(&setup, 0, sizeof(setup));
	if (snd_pcm_setup(handle, &setup) < 0) {
		fprintf(stderr, "%s: unable to obtain setup\n", command);
		exit(EXIT_FAILURE);
	}

	if (dump_pcm)
		snd_pcm_dump(handle, stderr);

	buffer_size = setup.frag_size;
	bits_per_sample = snd_pcm_format_physical_width(setup.format.format);
	bits_per_frame = bits_per_sample * setup.format.channels;
	buffer_bytes = buffer_size * bits_per_frame / 8;
	audiobuf = malloc(buffer_bytes);
	if (audiobuf == NULL) {
		fprintf(stderr, "%s: not enough memory\n", command);
		exit(EXIT_FAILURE);
	}
	// fprintf(stderr, "real buffer_size = %i, frags = %i, total = %i\n", buffer_size, setup.buf.block.frags, setup.buf.block.frags * buffer_size);
}

/* playback write error hander */

void playback_underrun(void)
{
	snd_pcm_status_t status;
	
	memset(&status, 0, sizeof(status));
	if (snd_pcm_status(handle, &status)<0) {
		fprintf(stderr, "playback status error\n");
		exit(EXIT_FAILURE);
	}
	if (status.state == SND_PCM_STATE_XRUN) {
		fprintf(stderr, "underrun at position %u!!!\n", status.frame_io);
		if (snd_pcm_prepare(handle)<0) {
			fprintf(stderr, "underrun: playback prepare error\n");
			exit(EXIT_FAILURE);
		}
		return;		/* ok, data should be accepted again */
	}
	fprintf(stderr, "write error\n");
	exit(EXIT_FAILURE);
}

/* capture read error hander */

void capture_overrun(void)
{
	snd_pcm_status_t status;
	
	memset(&status, 0, sizeof(status));
	if (snd_pcm_status(handle, &status)<0) {
		fprintf(stderr, "capture status error\n");
		exit(EXIT_FAILURE);
	}
	if (status.state == SND_PCM_STATE_RUNNING)
		return;		/* everything is ok, but the driver is waiting for data */
	if (status.state == SND_PCM_STATE_XRUN) {
		fprintf(stderr, "overrun at position %u!!!\n", status.frame_io);
		if (snd_pcm_prepare(handle)<0) {
			fprintf(stderr, "overrun: capture prepare error\n");
			exit(EXIT_FAILURE);
		}
		return;		/* ok, data should be accepted again */
	}
	fprintf(stderr, "read error\n");
	exit(EXIT_FAILURE);
}

/*
 *  write function
 */

static ssize_t pcm_write(u_char *data, size_t count)
{
	ssize_t r;
	ssize_t result = 0;

	if (mode == SND_PCM_MODE_FRAGMENT &&
	    count < buffer_size) {
		snd_pcm_format_set_silence(format.format, data + count * bits_per_frame / 8, (buffer_size - count) * format.channels);
		count = buffer_size;
	}
	while (count > 0) {
		r = write_func(handle, data, count);
		if (r == -EAGAIN || (r >= 0 && r < count)) {
			struct pollfd pfd;
			pfd.fd = snd_pcm_file_descriptor(handle);
			pfd.events = POLLOUT | POLLERR;
			poll(&pfd, 1, 1000);
		} else if (r == -EPIPE) {
			playback_underrun();
		} else if (r < 0) {
			fprintf(stderr, "write error: %s\n", snd_strerror(r));
			exit(EXIT_FAILURE);
		}
		if (r > 0) {
			result += r;
			count -= r;
			data += r * bits_per_frame / 8;
		}
	}
	return result;
}

static ssize_t pcm_writev(u_char **data, unsigned int channels, size_t count)
{
	ssize_t r;
	size_t result = 0;

	if (mode == SND_PCM_MODE_FRAGMENT &&
	    count != buffer_size) {
		unsigned int channel;
		size_t offset = count;
		size_t remaining = buffer_size - count;
		for (channel = 0; channel < channels; channel++)
			snd_pcm_format_set_silence(format.format, data[channel] + offset * bits_per_sample / 8, remaining);
		count = buffer_size;
	}
	while (count > 0) {
		unsigned int channel;
		struct iovec vec[channels];
		size_t offset = result;
		size_t remaining = count;
		for (channel = 0; channel < channels; channel++) {
			vec[channel].iov_base = data[channel] + offset * bits_per_sample / 8;
			vec[channel].iov_len = remaining;
		}
		r = writev_func(handle, vec, channels);
		if (r == -EAGAIN || (r >= 0 && r < count)) {
			struct pollfd pfd;
			pfd.fd = snd_pcm_file_descriptor(handle);
			pfd.events = POLLOUT | POLLERR;
			poll(&pfd, 1, 1000);
		} else if (r == -EPIPE) {
			playback_underrun();
		} else if (r < 0) {
			fprintf(stderr, "writev error: %s\n", snd_strerror(r));
			exit(EXIT_FAILURE);
		}
		if (r > 0) {
			result += r;
			count -= r;
		}
	}
	return result;
}

/*
 *  read function
 */

static ssize_t pcm_read(u_char *data, size_t rcount)
{
	ssize_t r;
	size_t result = 0;
	size_t count = rcount;

	if (mode == SND_PCM_MODE_FRAGMENT &&
	    count != buffer_size) {
		count = buffer_size;
	}

	while (count > 0) {
		r = read_func(handle, data, count);
		if (r == -EAGAIN || (r >= 0 && r < count)) {
			struct pollfd pfd;
			pfd.fd = snd_pcm_file_descriptor(handle);
			pfd.events = POLLIN | POLLERR;
			poll(&pfd, 1, 1000);
		} else if (r == -EPIPE) {
			capture_overrun();
		} else if (r < 0) {
			fprintf(stderr, "read error: %s\n", snd_strerror(r));
			exit(EXIT_FAILURE);
		}
		if (r > 0) {
			result += r;
			count -= r;
			data += r * bits_per_frame / 8;
		}
	}
	return rcount;
}

static ssize_t pcm_readv(u_char **data, unsigned int channels, size_t rcount)
{
	ssize_t r;
	size_t result = 0;
	size_t count = rcount;

	if (mode == SND_PCM_MODE_FRAGMENT &&
	    count != buffer_size) {
		count = buffer_size;
	}

	while (count > 0) {
		unsigned int channel;
		struct iovec vec[channels];
		size_t offset = result;
		size_t remaining = count;
		for (channel = 0; channel < channels; channel++) {
			vec[channel].iov_base = data[channel] + offset * bits_per_sample / 8;
			vec[channel].iov_len = remaining;
		}
		r = readv_func(handle, vec, channels);
		if (r == -EAGAIN || (r >= 0 && r < count)) {
			struct pollfd pfd;
			pfd.fd = snd_pcm_file_descriptor(handle);
			pfd.events = POLLIN | POLLERR;
			poll(&pfd, 1, 1000);
		} else if (r == -EPIPE) {
			capture_overrun();
		} else if (r < 0) {
			fprintf(stderr, "readv error: %s\n", snd_strerror(r));
			exit(EXIT_FAILURE);
		}
		if (r > 0) {
			result += r;
			count -= r;
		}
	}
	return rcount;
}

/*
 *  ok, let's play a .voc file
 */

static ssize_t voc_pcm_write(u_char *data, size_t count)
{
	ssize_t result = count, r;
	size_t size;

	while (count > 0) {
		size = count;
		if (size > buffer_bytes - buffer_pos)
			size = buffer_bytes - buffer_pos;
		memcpy(audiobuf + buffer_pos, data, size);
		data += size;
		count -= size;
		buffer_pos += size;
		if (buffer_pos == buffer_bytes) {
			if ((r = pcm_write(audiobuf, buffer_size)) != buffer_size)
				return r;
			buffer_pos = 0;
		}
	}
	return result;
}

static void voc_write_silence(unsigned x)
{
	unsigned l;
	char *buf;

	buf = (char *) malloc(buffer_bytes);
	if (buf == NULL) {
		fprintf(stderr, "%s: can allocate buffer for silence\n", command);
		return;		/* not fatal error */
	}
	snd_pcm_format_set_silence(format.format, buf, buffer_size * format.channels);
	while (x > 0) {
		l = x;
		if (l > buffer_size)
			l = buffer_size;
		if (voc_pcm_write(buf, l) != l) {
			fprintf(stderr, "%s: write error\n", command);
			exit(EXIT_FAILURE);
		}
		x -= l;
	}
}

static void voc_pcm_flush(void)
{
	if (buffer_pos > 0) {
		size_t b;
		if (mode == SND_PCM_MODE_FRAGMENT) {
			if (snd_pcm_format_set_silence(format.format, audiobuf + buffer_pos, buffer_bytes - buffer_pos * 8 / bits_per_sample) < 0)
				fprintf(stderr, "voc_pcm_flush - silence error\n");
			b = buffer_size;
		} else {
			b = buffer_pos * 8 / bits_per_frame;
		}
		if (pcm_write(audiobuf, b) != b)
			fprintf(stderr, "voc_pcm_flush error\n");
	}
	snd_pcm_flush(handle);
}

static void voc_play(int fd, int ofs, char *name)
{
	int l;
	VocBlockType *bp;
	VocVoiceData *vd;
	VocExtBlock *eb;
	size_t nextblock, in_buffer;
	u_char *data, *buf;
	char was_extended = 0, output = 0;
	u_short *sp, repeat = 0;
	size_t silence;
	int filepos = 0;

#define COUNT(x)	nextblock -= x; in_buffer -= x; data += x
#define COUNT1(x)	in_buffer -= x; data += x

	data = buf = (u_char *)malloc(64 * 1024);
	buffer_pos = 0;
	if (data == NULL) {
		fprintf(stderr, "malloc error\n");
		exit(EXIT_FAILURE);
	}
	if (!quiet_mode) {
		fprintf(stderr, "Playing Creative Labs Channel file '%s'...\n", name);
	}
	/* first we waste the rest of header, ugly but we don't need seek */
	while (ofs > buffer_bytes) {
		if (read(fd, buf, buffer_bytes) != buffer_bytes) {
			fprintf(stderr, "%s: read error\n", command);
			exit(EXIT_FAILURE);
		}
		ofs -= buffer_bytes;
	}
	if (ofs) {
		if (read(fd, buf, ofs) != ofs) {
			fprintf(stderr, "%s: read error\n", command);
			exit(EXIT_FAILURE);
		}
	}
	format.format = SND_PCM_SFMT_U8;
	format.channels = 1;
	format.rate = DEFAULT_SPEED;
	set_params();

	in_buffer = nextblock = 0;
	while (1) {
	      Fill_the_buffer:	/* need this for repeat */
		if (in_buffer < 32) {
			/* move the rest of buffer to pos 0 and fill the buf up */
			if (in_buffer)
				memcpy(buf, data, in_buffer);
			data = buf;
			if ((l = read(fd, buf + in_buffer, buffer_bytes - in_buffer)) > 0)
				in_buffer += l;
			else if (!in_buffer) {
				/* the file is truncated, so simulate 'Terminator' 
				   and reduce the datablock for safe landing */
				nextblock = buf[0] = 0;
				if (l == -1) {
					perror(name);
					exit(EXIT_FAILURE);
				}
			}
		}
		while (!nextblock) {	/* this is a new block */
			if (in_buffer < sizeof(VocBlockType))
				goto __end;
			bp = (VocBlockType *) data;
			COUNT1(sizeof(VocBlockType));
			nextblock = VOC_DATALEN(bp);
			if (output && !quiet_mode)
				fprintf(stderr, "\n");	/* write /n after ASCII-out */
			output = 0;
			switch (bp->type) {
			case 0:
#if 0
				d_printf("Terminator\n");
#endif
				return;		/* VOC-file stop */
			case 1:
				vd = (VocVoiceData *) data;
				COUNT1(sizeof(VocVoiceData));
				/* we need a SYNC, before we can set new SPEED, STEREO ... */

				if (!was_extended) {
					format.rate = (int) (vd->tc);
					format.rate = 1000000 / (256 - format.rate);
#if 0
					d_printf("Channel data %d Hz\n", dsp_speed);
#endif
					if (vd->pack) {		/* /dev/dsp can't it */
						fprintf(stderr, "%s: can't play packed .voc files\n", command);
						return;
					}
					if (format.channels == 2)		/* if we are in Stereo-Mode, switch back */
						format.channels = 1;
				} else {	/* there was extended block */
					format.channels = 2;
					was_extended = 0;
				}
				set_params();
				break;
			case 2:	/* nothing to do, pure data */
#if 0
				d_printf("Channel continuation\n");
#endif
				break;
			case 3:	/* a silence block, no data, only a count */
				sp = (u_short *) data;
				COUNT1(sizeof(u_short));
				format.rate = (int) (*data);
				COUNT1(1);
				format.rate = 1000000 / (256 - format.rate);
				set_params();
				silence = (((size_t) * sp) * 1000) / format.rate;
#if 0
				d_printf("Silence for %d ms\n", (int) silence);
#endif
				voc_write_silence(*sp);
				break;
			case 4:	/* a marker for syncronisation, no effect */
				sp = (u_short *) data;
				COUNT1(sizeof(u_short));
#if 0
				d_printf("Marker %d\n", *sp);
#endif
				break;
			case 5:	/* ASCII text, we copy to stderr */
				output = 1;
#if 0
				d_printf("ASCII - text :\n");
#endif
				break;
			case 6:	/* repeat marker, says repeatcount */
				/* my specs don't say it: maybe this can be recursive, but
				   I don't think somebody use it */
				repeat = *(u_short *) data;
				COUNT1(sizeof(u_short));
#if 0
				d_printf("Repeat loop %d times\n", repeat);
#endif
				if (filepos >= 0) {	/* if < 0, one seek fails, why test another */
					if ((filepos = lseek(fd, 0, 1)) < 0) {
						fprintf(stderr, "%s: can't play loops; %s isn't seekable\n",
							command, name);
						repeat = 0;
					} else {
						filepos -= in_buffer;	/* set filepos after repeat */
					}
				} else {
					repeat = 0;
				}
				break;
			case 7:	/* ok, lets repeat that be rewinding tape */
				if (repeat) {
					if (repeat != 0xFFFF) {
#if 0
						d_printf("Repeat loop %d\n", repeat);
#endif
						--repeat;
					}
#if 0
					else
						d_printf("Neverending loop\n");
#endif
					lseek(fd, filepos, 0);
					in_buffer = 0;	/* clear the buffer */
					goto Fill_the_buffer;
				}
#if 0
				else
					d_printf("End repeat loop\n");
#endif
				break;
			case 8:	/* the extension to play Stereo, I have SB 1.0 :-( */
				was_extended = 1;
				eb = (VocExtBlock *) data;
				COUNT1(sizeof(VocExtBlock));
				format.rate = (int) (eb->tc);
				format.rate = 256000000L / (65536 - format.rate);
				format.channels = eb->mode == VOC_MODE_STEREO ? 2 : 1;
				if (format.channels == 2)
					format.rate = format.rate >> 1;
				if (eb->pack) {		/* /dev/dsp can't it */
					fprintf(stderr, "%s: can't play packed .voc files\n", command);
					return;
				}
#if 0
				d_printf("Extended block %s %d Hz\n",
					 (eb->mode ? "Stereo" : "Mono"), dsp_speed);
#endif
				break;
			default:
				fprintf(stderr, "%s: unknown blocktype %d. terminate.\n",
					command, bp->type);
				return;
			}	/* switch (bp->type) */
		}		/* while (! nextblock)  */
		/* put nextblock data bytes to dsp */
		l = in_buffer;
		if (nextblock < l)
			l = nextblock;
		if (l) {
			if (output && !quiet_mode) {
				if (write(2, data, l) != l) {	/* to stderr */
					fprintf(stderr, "%s: write error\n", command);
					exit(EXIT_FAILURE);
				}
			} else {
				if (voc_pcm_write(data, l) != l) {
					fprintf(stderr, "%s: write error\n", command);
					exit(EXIT_FAILURE);
				}
			}
			COUNT(l);
		}
	}			/* while(1) */
      __end:
        voc_pcm_flush();
        free(buf);
}
/* that was a big one, perhaps somebody split it :-) */

/* setting the globals for playing raw data */
static void init_raw_data(void)
{
	format = rformat;
}

/* calculate the data count to read from/to dsp */
static size_t calc_count(void)
{
	size_t count;

	if (!timelimit) {
		count = 0x7fffffff;
	} else {
		count = snd_pcm_format_size(format.format,
					    timelimit * format.rate *
					    format.channels);
	}
	return count;
}

/* write a .VOC-header */
static void begin_voc(int fd, size_t cnt)
{
	VocHeader vh;
	VocBlockType bt;
	VocVoiceData vd;
	VocExtBlock eb;

	strncpy(vh.magic, VOC_MAGIC_STRING, 20);
	vh.magic[19] = 0x1A;
	vh.headerlen = sizeof(VocHeader);
	vh.version = VOC_ACTUAL_VERSION;
	vh.coded_ver = 0x1233 - VOC_ACTUAL_VERSION;

	if (write(fd, &vh, sizeof(VocHeader)) != sizeof(VocHeader)) {
		fprintf(stderr, "%s: write error\n", command);
		exit(EXIT_FAILURE);
	}
	if (format.channels > 1) {
		/* write a extended block */
		bt.type = 8;
		bt.datalen = 4;
		bt.datalen_m = bt.datalen_h = 0;
		if (write(fd, &bt, sizeof(VocBlockType)) != sizeof(VocBlockType)) {
			fprintf(stderr, "%s: write error\n", command);
			exit(EXIT_FAILURE);
		}
		eb.tc = (u_short) (65536 - 256000000L / (format.rate << 1));
		eb.pack = 0;
		eb.mode = 1;
		if (write(fd, &eb, sizeof(VocExtBlock)) != sizeof(VocExtBlock)) {
			fprintf(stderr, "%s: write error\n", command);
			exit(EXIT_FAILURE);
		}
	}
	bt.type = 1;
	cnt += sizeof(VocVoiceData);	/* Channel_data block follows */
	bt.datalen = (u_char) (cnt & 0xFF);
	bt.datalen_m = (u_char) ((cnt & 0xFF00) >> 8);
	bt.datalen_h = (u_char) ((cnt & 0xFF0000) >> 16);
	if (write(fd, &bt, sizeof(VocBlockType)) != sizeof(VocBlockType)) {
		fprintf(stderr, "%s: write error\n", command);
		exit(EXIT_FAILURE);
	}
	vd.tc = (u_char) (256 - (1000000 / format.rate));
	vd.pack = 0;
	if (write(fd, &vd, sizeof(VocVoiceData)) != sizeof(VocVoiceData)) {
		fprintf(stderr, "%s: write error\n", command);
		exit(EXIT_FAILURE);
	}
}

/* write a WAVE-header */
static void begin_wave(int fd, size_t cnt)
{
	WaveHeader h;
	WaveFmtHeader f;
	WaveChunkHeader c;
	int bits;
	u_int tmp;
	u_short tmp2;

	bits = 8;
	switch (format.format) {
	case SND_PCM_SFMT_U8:
		bits = 8;
		break;
	case SND_PCM_SFMT_S16_LE:
		bits = 16;
		break;
	default:
		fprintf(stderr, "%s: Wave doesn't support %s format...\n", command, snd_pcm_format_name(format.format));
		exit(EXIT_FAILURE);
	}
	h.magic = WAV_RIFF;
	tmp = cnt + sizeof(WaveHeader) + sizeof(WaveFmtHeader) + sizeof(WaveChunkHeader) - 8;
	h.length = LE_INT(tmp);
	h.type = WAV_WAVE;

	f.type = WAV_FMT;
	f.length = LE_INT(16);
	f.format = LE_INT(WAV_PCM_CODE);
	f.modus = LE_SHORT(format.channels);
	f.sample_fq = LE_INT(format.rate);
#if 0
	tmp2 = (samplesize == 8) ? 1 : 2;
	f.byte_p_spl = LE_SHORT(tmp2);
	tmp2 = dsp_speed * format.channels * tmp2;
	f.byte_p_sec = LE_SHORT(tmp2);
#else
	tmp2 = format.channels * ((bits + 7) / 8);
	f.byte_p_spl = LE_SHORT(tmp2);
	tmp2 = tmp2 * format.rate;
	f.byte_p_sec = LE_SHORT(tmp2);
#endif
	f.bit_p_spl = LE_SHORT(bits);

	c.type = WAV_DATA;
	c.length = LE_INT(cnt);

	if (write(fd, &h, sizeof(WaveHeader)) != sizeof(WaveHeader) ||
	    write(fd, &f, sizeof(WaveFmtHeader)) != sizeof(WaveFmtHeader) ||
	    write(fd, &c, sizeof(WaveChunkHeader)) != sizeof(WaveChunkHeader)) {
		fprintf(stderr, "%s: write error\n", command);
		exit(EXIT_FAILURE);
	}
}

/* write a Au-header */
static void begin_au(int fd, size_t cnt)
{
	AuHeader ah;

	ah.magic = AU_MAGIC;
	ah.hdr_size = BE_INT(24);
	ah.data_size = BE_INT(cnt);
	switch (format.format) {
	case SND_PCM_SFMT_MU_LAW:
		ah.encoding = BE_INT(AU_FMT_ULAW);
		break;
	case SND_PCM_SFMT_U8:
		ah.encoding = BE_INT(AU_FMT_LIN8);
		break;
	case SND_PCM_SFMT_S16_LE:
		ah.encoding = BE_INT(AU_FMT_LIN16);
		break;
	default:
		fprintf(stderr, "%s: Sparc Audio doesn't support %s format...\n", command, snd_pcm_format_name(format.format));
		exit(EXIT_FAILURE);
	}
	ah.sample_rate = BE_INT(format.rate);
	ah.channels = BE_INT(format.channels);
	if (write(fd, &ah, sizeof(AuHeader)) != sizeof(AuHeader)) {
		fprintf(stderr, "%s: write error\n", command);
		exit(EXIT_FAILURE);
	}
}

/* closing .VOC */
static void end_voc(int fd)
{
	char dummy = 0;		/* Write a Terminator */
	if (write(fd, &dummy, 1) != 1) {
		fprintf(stderr, "%s: write error", command);
		exit(EXIT_FAILURE);
	}
	if (fd != 1)
		close(fd);
}

static void end_wave(int fd)
{				/* only close output */
	if (fd != 1)
		close(fd);
}

static void header(int rtype, char *name)
{
	if (!quiet_mode) {
		fprintf(stderr, "%s %s '%s' : ",
			(stream == SND_PCM_STREAM_PLAYBACK) ? "Playing" : "Recording",
			fmt_rec_table[rtype].what,
			name);
		fprintf(stderr, "%s, ", snd_pcm_format_description(format.format));
		fprintf(stderr, "Rate %d Hz, ", format.rate);
		if (format.channels == 1)
			fprintf(stderr, "Mono");
		else if (format.channels == 2)
			fprintf(stderr, "Stereo");
		else
			fprintf(stderr, "Channels %i", format.channels);
		fprintf(stderr, "\n");
	}
}

/* playing raw data */

void playback_go(int fd, size_t loaded, size_t count, int rtype, char *name)
{
	int l, r;
	size_t written = 0;
	size_t c;

	header(rtype, name);
	set_params();

	while (loaded > buffer_bytes && written < count) {
		if (pcm_write(audiobuf + written, buffer_size) <= 0)
			return;
		written += buffer_bytes;
		loaded -= buffer_bytes;
	}
	if (written > 0 && loaded > 0)
		memmove(audiobuf, audiobuf + written, loaded);

	l = loaded;
	while (written < count) {
		do {
			c = count - written;
			if (c > buffer_bytes)
				c = buffer_bytes;
			c -= l;

			if (c == 0)
				break;
			r = read(fd, audiobuf + l, c);
			if (r < 0) {
				perror(name);
				exit(EXIT_FAILURE);
			}
			if (r == 0)
				break;
			l += r;
		} while (mode != SND_PCM_MODE_FRAME && l < buffer_bytes);
		l = l * 8 / bits_per_frame;
		r = pcm_write(audiobuf, l);
		if (r != l)
			break;
		r = r * bits_per_frame / 8;
		written += r;
		l = 0;
	}
	snd_pcm_flush(handle);
}

/* captureing raw data, this proc handels WAVE files and .VOCs (as one block) */

void capture_go(int fd, size_t count, int rtype, char *name)
{
	size_t c;
	ssize_t r;

	header(rtype, name);
	set_params();

	while (count > 0) {
		c = count;
		if (c > buffer_bytes)
			c = buffer_bytes;
		c = c * 8 / bits_per_frame;
		if ((r = pcm_read(audiobuf, c)) != c)
			break;
		r = r * bits_per_frame / 8;
		if (write(fd, audiobuf, r) != r) {
			perror(name);
			exit(EXIT_FAILURE);
		}
		count -= r;
	}
}

/*
 *  let's play or capture it (capture_type says VOC/WAVE/raw)
 */

static void playback(char *name)
{
	int fd, ofs;

	count = calc_count();
	snd_pcm_flush(handle);
	if (!name || !strcmp(name, "-")) {
		fd = 0;
		name = "stdin";
	} else {
		if ((fd = open(name, O_RDONLY, 0)) == -1) {
			perror(name);
			exit(EXIT_FAILURE);
		}
	}
	/* read the file header */
	if (read(fd, audiobuf, sizeof(AuHeader)) != sizeof(AuHeader)) {
		fprintf(stderr, "%s: read error", command);
		exit(EXIT_FAILURE);
	}
	if (test_au(fd, audiobuf) >= 0) {
		rformat.format = SND_PCM_SFMT_MU_LAW;
		playback_go(fd, 0, count, FORMAT_AU, name);
		goto __end;
	}
	if (read(fd, audiobuf + sizeof(AuHeader),
		 sizeof(VocHeader) - sizeof(AuHeader)) !=
		 sizeof(VocHeader) - sizeof(AuHeader)) {
		fprintf(stderr, "%s: read error", command);
		exit(EXIT_FAILURE);
	}
	if ((ofs = test_vocfile(audiobuf)) >= 0) {
		voc_play(fd, ofs, name);
		goto __end;
	}
	/* read bytes for WAVE-header */
	if (read(fd, audiobuf + sizeof(VocHeader),
		 64 - sizeof(VocHeader)) !=
	    64 - sizeof(VocHeader)) {
		fprintf(stderr, "%s: read error", command);
		exit(EXIT_FAILURE);
	}
	if ((ofs = test_wavefile(audiobuf, 64)) > 0) {
		memmove(audiobuf, audiobuf + ofs, 64 - ofs);
		playback_go(fd, 64 - ofs, count, FORMAT_WAVE, name);
	} else {
		/* should be raw data */
		check_new_format(&rformat);
		init_raw_data();
		playback_go(fd, 64, count, FORMAT_RAW, name);
	}
      __end:
	if (fd != 0)
		close(fd);
}

static void capture(char *name)
{
	int fd;

	snd_pcm_flush(handle);
	if (!name || !strcmp(name, "-")) {
		fd = 1;
		name = "stdout";
	} else {
		remove(name);
		if ((fd = open(name, O_WRONLY | O_CREAT, 0644)) == -1) {
			perror(name);
			exit(EXIT_FAILURE);
		}
	}
	count = calc_count();
	count += count % 2;
	/* WAVE-file should be even (I'm not sure), but wasting one byte
	   isn't a problem (this can only be in 8 bit mono) */
	if (fmt_rec_table[file_type].start)
		fmt_rec_table[file_type].start(fd, count);
	check_new_format(&rformat);
	capture_go(fd, count, file_type, name);
	fmt_rec_table[file_type].end(fd);
}

void playbackv_go(int* fds, unsigned int channels, size_t loaded, size_t count, int rtype, char **names)
{
	int r;
	size_t vsize;
	unsigned int channel;
	u_char *bufs[channels];

	header(rtype, names[0]);
	set_params();

	vsize = buffer_bytes / channels;

	// Not yet implemented
	assert(loaded == 0);

	for (channel = 0; channel < channels; ++channel)
		bufs[channel] = audiobuf + vsize * channel;

	while (count > 0) {
		size_t c = 0;
		size_t expected = count / channels;
		if (expected > vsize)
			expected = vsize;
		do {
			r = read(fds[0], bufs[0], expected);
			if (r < 0) {
				perror(names[channel]);
				exit(EXIT_FAILURE);
			}
			for (channel = 1; channel < channels; ++channel) {
				if (read(fds[channel], bufs[channel], r) != r) {
					perror(names[channel]);
					exit(EXIT_FAILURE);
				}
			}
			if (r == 0)
				break;
			c += r;
		} while (mode != SND_PCM_MODE_FRAME && c < expected);
		c = c * 8 / bits_per_sample;
		r = pcm_writev(bufs, channels, c);
		if (r != c)
			break;
		r = r * bits_per_frame / 8;
		count -= r;
	}
	snd_pcm_flush(handle);
}

void capturev_go(int* fds, unsigned int channels, size_t count, int rtype, char **names)
{
	size_t c;
	ssize_t r;
	unsigned int channel;
	size_t vsize;
	u_char *bufs[channels];

	header(rtype, names[0]);
	set_params();

	vsize = buffer_bytes / channels;

	for (channel = 0; channel < channels; ++channel)
		bufs[channel] = audiobuf + vsize * channel;

	while (count > 0) {
		size_t rv;
		c = count;
		if (c > buffer_bytes)
			c = buffer_bytes;
		c = c * 8 / bits_per_frame;
		if ((r = pcm_readv(bufs, channels, c)) != c)
			break;
		rv = r * bits_per_sample / 8;
		for (channel = 0; channel < channels; ++channel) {
			if (write(fds[channel], bufs[channel], rv) != rv) {
				perror(names[channel]);
				exit(EXIT_FAILURE);
			}
		}
		r = r * bits_per_frame / 8;
		count -= r;
	}
}

static void playbackv(char **names, unsigned int count)
{
	int ret = 0;
	unsigned int channel;
	unsigned int channels = rformat.channels;
	int alloced = 0;
	int fds[channels];
	for (channel = 0; channel < channels; ++channel)
		fds[channel] = -1;

	snd_pcm_flush(handle);
	if (count == 1) {
		size_t len = strlen(names[0]);
		char format[1024];
		memcpy(format, names[0], len);
		strcpy(format + len, ".%d");
		len += 4;
		names = malloc(sizeof(*names) * channels);
		for (channel = 0; channel < channels; ++channel) {
			names[channel] = malloc(len);
			sprintf(names[channel], format, channel);
		}
		alloced = 1;
	} else if (count != channels) {
		fprintf(stderr, "You need to specify %d files\n", channels);
		exit(EXIT_FAILURE);
	}

	for (channel = 0; channel < channels; ++channel) {
		fds[channel] = open(names[channel], O_RDONLY, 0);
		if (fds[channel] < 0) {
			perror(names[channel]);
			ret = EXIT_FAILURE;
			goto __end;
		}
	}
	/* should be raw data */
	check_new_format(&rformat);
	init_raw_data();
	count = calc_count();
	playbackv_go(fds, channels, 0, count, FORMAT_RAW, names);

      __end:
	for (channel = 0; channel < channels; ++channel) {
		if (fds[channel] >= 0)
			close(fds[channel]);
		if (alloced)
			free(names[channel]);
	}
	if (alloced)
		free(names);
	if (ret)
		exit(ret);
}

static void capturev(char **names, unsigned int count)
{
	int ret = 0;
	unsigned int channel;
	unsigned int channels = rformat.channels;
	int alloced = 0;
	int fds[channels];
	for (channel = 0; channel < channels; ++channel)
		fds[channel] = -1;

	snd_pcm_flush(handle);
	if (count == 1) {
		size_t len = strlen(names[0]);
		char format[1024];
		memcpy(format, names[0], len);
		strcpy(format + len, ".%d");
		len += 4;
		names = malloc(sizeof(*names) * channels);
		for (channel = 0; channel < channels; ++channel) {
			names[channel] = malloc(len);
			sprintf(names[channel], format, channel);
		}
		alloced = 1;
	} else if (count != channels) {
		fprintf(stderr, "You need to specify %d files\n", channels);
		exit(EXIT_FAILURE);
	}

	for (channel = 0; channel < channels; ++channel) {
		fds[channel] = open(names[channel], O_WRONLY + O_CREAT, 0644);
		if (fds[channel] < 0) {
			perror(names[channel]);
			ret = EXIT_FAILURE;
			goto __end;
		}
	}
	/* should be raw data */
	check_new_format(&rformat);
	init_raw_data();
	count = calc_count();
	capturev_go(fds, channels, count, FORMAT_RAW, names);

      __end:
	for (channel = 0; channel < channels; ++channel) {
		if (fds[channel] >= 0)
			close(fds[channel]);
		if (alloced)
			free(names[channel]);
	}
	if (alloced)
		free(names);
	if (ret)
		exit(ret);
}

