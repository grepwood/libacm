/*
 * Command line tool for ACM manipulating.
 *
 * Copyright (c) 2004-2010, Marko Kreen
 * Copyright (c) 2014, Michael Dec
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libacm.h"

static int cf_raw = 0;
/* static int cf_force_chans = 0; */
static int cf_no_output = 0;
static int cf_quiet = 0;

/* error strings */
static const char *_errlist[] = {
	"No error",
	"ACM error",
	"Cannot open file",
	"Not an ACM file",
	"Read error",
	"Bad format",
	"Corrupt file",
	"Unexcpected EOF",
	"Stream not seekable"
};

const char * libacm_strerror(int err)
{
	int nerr = sizeof(_errlist) / sizeof(char *);
	if ((-err) < 0 || (-err) >= nerr)
		return "Unknown error";
	return _errlist[-err];
}

static void show_header(const char *fn, ACMStream *acm)
{
	int kbps;
	const ACMInfo *inf;
	unsigned m, s, tmp;
	if (cf_quiet)
		return;
	inf = acm_info(acm);
	kbps = acm_bitrate(acm) / 1000;
	tmp = acm_time_total(acm) / 1000;
	s = tmp % 60;
	m = tmp / 60;
	printf("%s: Length:%2d:%02d Chans:%d(%d) Freq:%d A:%d/%d kbps:%d\n",
			fn, m, s, acm_channels(acm), acm->info.acm_channels,
			acm_rate(acm), inf->acm_level, inf->acm_rows, kbps);
}

#ifdef HAVE_AO

/*
 * Audio playback with libao
 */

#include <ao/ao.h>

static ao_device *dev = NULL;
static ao_sample_format old_fmt;

static ao_device *open_audio(ao_sample_format *fmt)
{
	if (dev && memcmp(fmt, &old_fmt, sizeof(old_fmt)) != 0) {
		ao_close(dev);
		dev = NULL;
	}
	if (dev == NULL) {
		int id = ao_default_driver_id();
		if (id < 0) {
			fprintf(stderr, "failed to find audio driver\n");
			exit(1);
		}
		dev = ao_open_live(id, fmt, NULL);
		old_fmt = *fmt;
	}
	if (dev == NULL) {
		fprintf(stderr, "failed to open audio device\n");
		exit(1);
	}
	return dev;
}

static void close_audio(void)
{
	if (dev)
		ao_close(dev);
	dev = NULL;
}

static void play_file(const char *fn)
{
	ACMStream *acm;
	int err, res, buflen;
	ao_sample_format fmt;
	ao_device *dev;
	char *buf;
	unsigned int total_bytes, bytes_done = 0;

	err = acm_open_file(&acm, fn, cf_force_chans);
	if (err < 0) {
		fprintf(stderr, "%s: %s\n", fn, acm_strerror(err));
		return;
	}
	show_header(fn, acm);
	fmt.bits = 16;
	fmt.rate = acm_rate(acm);
	fmt.channels = acm_channels(acm);
	fmt.byte_format = AO_FMT_LITTLE;

	dev = open_audio(&fmt);

	buflen = 4*1024;
	buf = malloc(buflen);

	total_bytes = acm_pcm_total(acm) * acm_channels(acm) * ACM_WORD;
	while (bytes_done < total_bytes) {
		res = acm_read_loop(acm, buf, buflen/ACM_WORD, 0,2,1);
		if (res == 0)
			break;
		if (res > 0) {
			bytes_done += res;
			res = ao_play(dev, buf, res);
		} else {
			fprintf(stderr, "%s: %s\n", fn, acm_strerror(res));
			break;
		}
	}

	memset(buf, 0, buflen);
	if (bytes_done < total_bytes)
		fprintf(stderr, "%s: adding filler_samples: %d\n",
				fn, total_bytes - bytes_done);
	while (bytes_done < total_bytes) {
		int bs;
		if (bytes_done + buflen > total_bytes) {
			bs = total_bytes - bytes_done;
		} else {
			bs = buflen;
		}
		res = ao_play(dev, buf, bs);
		if (res != bs)
			break;
		bytes_done += res;
	}

	acm_close(acm);
	free(buf);
}

#endif /* HAVE_AO */

/*
 * WAV writing
 */

char * libacm_makefn(const char *fn, const char *ext)
{
	char *dstfn, *p;
	dstfn = (char*)malloc(strlen(fn) + strlen(ext) + 2);
	strcpy(dstfn, fn);
	p = strrchr(dstfn, '.');
	if (p != NULL)
		*p = 0;
	strcat(dstfn, ext);
	return dstfn;
}

#define put_word(p, val) do { \
		*p++ = val & 0xFF; \
		*p++ = (val >> 8) & 0xFF; \
	} while (0)

#define put_dword(p, val) do { \
		*p++ = val & 0xFF; \
		*p++ = (val >> 8) & 0xFF; \
		*p++ = (val >> 16) & 0xFF; \
		*p++ = (val >> 24) & 0xFF; \
	} while (0)

#define put_data(p, data, len) do { \
		memcpy(p, data, len); \
		p += len; \
	} while (0)

static int write_wav_header(void *f, ACMStream *acm, char mode) {
	unsigned char hdr[50], *p = hdr;
	int res;
	char * ptrerr;
	unsigned datalen = acm_pcm_total(acm) * ACM_WORD * acm_channels(acm);
	
	int code = 1;
	unsigned n_channels = acm_channels(acm);
	unsigned srate = acm_rate(acm);
	unsigned avg_bps = srate * n_channels * ACM_WORD;
	unsigned significant_bits = ACM_WORD * 8;
	unsigned block_align = significant_bits * n_channels / 8;
	unsigned hdrlen = 16;
	unsigned wavlen = 4 + 8 + hdrlen + 8 + datalen;
	
	memset(hdr, 0, sizeof(hdr));
	
	put_data(p, "RIFF", 4);
	put_dword(p, wavlen);
	put_data(p, "WAVEfmt ", 8);
	put_dword(p, hdrlen);
	put_word(p, code);
	put_word(p, n_channels);
	put_dword(p, srate);
	put_dword(p, avg_bps);
	put_word(p, block_align);
	put_word(p, significant_bits);
	
	put_data(p, "data", 4);
	put_dword(p, datalen);

	switch(mode) {
	case 1:
		res = fwrite(hdr,1,p-hdr,(FILE*)f);
		printf("p-hdr is %l bytes large\n",p-hdr);
		if(res != p-hdr)
			return -1;
		break;
	case 2:
		ptrerr = memcpy((char*)f,hdr,p-hdr);
		if(ptrerr != (char*)f)
			return -2;
		break;
	}
	return 0;
}

static int get_sample_count(const char * fn, uint32_t * wavsize) {
	FILE * acmfile = fopen(fn,"rb");
	if(!acmfile) {
		fprintf(stderr, "%s: empty of nonexistent file\n",fn);
		fclose(acmfile);
		return 1;
	}
	if(fseek(acmfile,4,SEEK_SET)) {
		fprintf(stderr, "%s: can’t fseek in this file\n",fn);
		fclose(acmfile);
		return 2;
	}
	if(fread(wavsize,4,1,acmfile) != 1) {
		fprintf(stderr, "%s: read file incorrect amount of times\n",fn);
		fclose(acmfile);
		return 4;
	}
#ifdef __BIG_ENDIAN__
	*wavsize = acm_swap32(*wavsize);
#endif
	fclose(acmfile);
	return 0;
}

char * libacm_decode_file_to_mem(const char *fn, int cf_force_chans) {
	ACMStream *acm;
	char * buf;
	uint32_t wavsize;
	int res, buflen, err;
	char * res2;
	char * result = NULL;
	int bytes_done = 0, total_bytes;
	int bs;

	if((err = acm_open_file(&acm, fn, cf_force_chans)) < 0) {
		fprintf(stderr,"%s: %s\n",fn,libacm_strerror(err));
		return result;
	}
	if(get_sample_count(fn,&wavsize)) {
		return result;
	}
	if(cf_force_chans)
		wavsize <<= cf_force_chans;
	else
		wavsize <<= acm->info.acm_channels;
	wavsize += 44; /* WAV header is so big? */
	result = (char*)malloc(wavsize);
	if((err = write_wav_header(result,acm,2)) < 0) {
		acm_close(acm);
		fputs("couldn't write to buffer",stderr);
		return result;
	}
	buflen = 16384;
	buf = (char*)malloc(buflen);

	total_bytes = acm_pcm_total(acm) * acm_channels(acm) * ACM_WORD;

	while(bytes_done < total_bytes) {
		res = acm_read_loop(acm,buf,buflen/2,0,2,1);
		if(!res)
			break;
		if(res > 0) {
			res2 = memcpy(result,buf,res);
			if(res2 != result) {
				fputs("buffer write error past wav header",stderr);
				break;
			}
			bytes_done += res;
		} else {
			fprintf(stderr,"%s: %s\n",fn,libacm_strerror(res));
			break;
		}
	}

	memset(buf,0,buflen);
	if(bytes_done < total_bytes)
		fprintf(stderr,"%s: adding filler_samples: %d\n",
			fn,total_bytes-bytes_done);
	while(bytes_done < total_bytes) {
		if(bytes_done + buflen > total_bytes) {
			bs = total_bytes-bytes_done;
		} else {
			bs = buflen;
		}
		res2 = memcpy(result,buf,bs);
		if(res2 != result)
			break;
		bytes_done += bs;
	}

	acm_close(acm);
	return result;
}

void libacm_decode_file(const char *fn, const char *fn2, int cf_force_chans) {
	ACMStream *acm;
	char *buf;
	int res, res2, buflen, err;
	FILE *fo = NULL;
	int bytes_done = 0, total_bytes;

	if ((err = acm_open_file(&acm,fn,cf_force_chans)) < 0) {
		fprintf(stderr, "%s: %s\n", fn, libacm_strerror(err));
		return;
	}

	if (!cf_no_output) {
		if (!strcmp(fn2, "-")) {
			fo = stdout;
			cf_quiet = 1;
		} else {
			fo = fopen(fn2, "wb");
		}
		if (fo == NULL) {
			perror(fn2);
			acm_close(acm);
			return;
		}
	}

	show_header(fn, acm);

	if ((!cf_raw) && (!cf_no_output)) {
		if ((err = write_wav_header(fo, acm,1)) < 0) {
			perror(fn2);
			fclose(fo);
			acm_close(acm);
			return;
		}
	}
	buflen = 16384;
	buf = (char*)malloc(buflen);

	total_bytes = acm_pcm_total(acm) * acm_channels(acm) * ACM_WORD;
	
	while (bytes_done < total_bytes) {
		res = acm_read_loop(acm, buf, buflen/2, 0,2,1);
		if (!res)
			break;
		if (res > 0) {
			if (!cf_no_output) {
				res2 = fwrite(buf, 1, res, fo);
				if (res2 != res) {
					fprintf(stderr, "%s: write error\n", fn2);
					break;
				}
			}
			bytes_done += res;
		} else {
			fprintf(stderr, "%s: %s\n", fn, libacm_strerror(res));
			break;
		}
	}

	memset(buf, 0, buflen);
	if (bytes_done < total_bytes)
		fprintf(stderr, "%s: adding filler_samples: %d\n",
			fn, total_bytes - bytes_done);
	while (bytes_done < total_bytes) {
		int bs;
		if (bytes_done + buflen > total_bytes) {
			bs = total_bytes - bytes_done;
		} else {
			bs = buflen;
		}
		if (!cf_no_output) {
			res2 = fwrite(buf, 1, bs, fo);
			if (res2 != bs)
				break;
		}
		bytes_done += bs;
	}

	acm_close(acm);
	if (!cf_no_output)
		fclose(fo);
	free(buf);
}

/*
 * Modify header
 */

void libacm_set_channels(const char *fn, int n_chan)
{
	FILE *f;
	static const unsigned char acm_id[] = { 0x97, 0x28, 0x03, 0x01 };
	unsigned char hdr[14];
	int oldnum, res;

	if ((f = fopen(fn, "rb+")) == NULL) {
		perror(fn);
		return;
	}
	res = fread(hdr, 1, 14, f);
	if (res != 14) {
		fprintf(stderr, "%s: cannot read header\n", fn);
		return;
	}

	if (memcmp(hdr, acm_id, 4)) {
		fprintf(stderr, "%s: not an ACM file\n", fn);
		return;
	}

	oldnum = (hdr[9] << 8) + hdr[8];
	if (oldnum != 1 && oldnum != 2) {
		fprintf(stderr, "%s: suspicios number of channels: %d\n",
				fn, oldnum);
		return;
	}

	if (fseek(f, 0, SEEK_SET)) {
		perror(fn);
		return;
	}

	hdr[8] = n_chan;
	res = fwrite(hdr, 1, 14, f);
	if (res != 14) {
		perror(fn);
	}
	fclose(f);
}

/*
 * Just show info
 */

void libacm_show_info(const char *fn, int cf_force_chans)
{
	int err;
	ACMStream *acm;

	err = acm_open_file(&acm, fn, cf_force_chans);
	if (err < 0) {
		printf("%s: %s\n", fn, libacm_strerror(err));
		return;
	}

	show_header(fn, acm);
	acm_close(acm);
}
