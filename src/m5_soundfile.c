/* Copyright (c) 1997-1999 Miller Puckette. Updated 2019 Dan Wilcox.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* With additional modifications by Michael Spears, 2025 */

/* this file contains, first, a collection of soundfile access routines, a
sort of soundfile library.  Second, the "soundfiler" object is defined which
uses the routines to read or write soundfiles, synchronously, from garrays.
These operations are not to be done in "real time" as they may have to wait
for disk accesses (even the write routine.)  Finally, the realtime objects
readsf~ and writesf~ are defined which confine disk operations to a separate
thread so that they can be used in real time.  The readsf~ and writesf~
objects use Posix-like threads. */

#include "m5_soundfile.h"
#include "m5_timeanchor.h"
#include "m5_timeanchor.h"
#include "g_canvas.h"
#include "s_stuff.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>

#include <m_pd.h>

/* Supported sample formats: LPCM (16 or 24 bit int) & 32 or 64 bit float */

#define VALID_BYTESPERSAMPLE(b) ((b) == 2 || (b) == 3 || (b) == 4 || (b) == 8)

#define MAXSFCHANS 64

/* GLIBC large file support */
#ifdef _LARGEFILE64_SOURCE
#define open open64
#endif

/* MSVC uses different naming for these */
#ifdef _MSC_VER
#define O_CREAT  _O_CREAT
#define O_TRUNC  _O_TRUNC
#define O_WRONLY _O_WRONLY
#endif

#define SCALE (1. / (1024. * 1024. * 1024. * 2.))

#define NOT_FOUND -1

#define RAMP_FRAMES 10
const t_sample ramp_up[RAMP_FRAMES] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9};
const t_sample ramp_down[RAMP_FRAMES] = {0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.0};

// MWS other functions from PD not in .h files here:
// note this function is 'static' in Pd 0.51 - need to 
// find a local replacement here...
// extern int do_open_via_path(const char *dir, const char *name,
	// const char *ext, char *dirresult, char **nameresult, unsigned int size,
	// int bin, t_namelist *searchpath, int okgui); 

	/* float sample conversion wrappers */
typedef union _floatuint
{
  float f;
  uint32_t ui;
} t_floatuint;
typedef union _doubleuint
{
  double d;
  uint64_t ui;
} t_doubleuint;

/* ----- soundfile ----- */

void m5_soundfile_clear(t_soundfile *sf)
{
	memset(sf, 0, sizeof(t_soundfile));
	sf->sf_fd = -1;
	sf->sf_type = NULL;
	sf->sf_bytelimit = SFMAXBYTES;
}

void m5_soundfile_copy(t_soundfile *dst, const t_soundfile *src)
{
	memcpy((char *)dst, (char *)src, sizeof(t_soundfile));
}

int m5_soundfile_needsbyteswap(const t_soundfile *sf)
{
	return sf->sf_bigendian != m5_sys_isbigendian();
}

const char* m5_soundfile_strerror(int errnum)
{
	switch (errnum)
	{
		case SOUNDFILE_ERRUNKNOWN:
			return "unknown header format";
		case SOUNDFILE_ERRMALFORMED:
			return "bad header format";
		case SOUNDFILE_ERRVERSION:
			return "unsupported header format version";
		case SOUNDFILE_ERRSAMPLEFMT:
			return "unsupported sample format";
		case SOUNDFILE_M5_ERREMPTY:
			return "the sound file has 0 frames";
		default: /* C/POSIX error */
			return strerror(errnum);
	}
}

	/** output soundfile format info as a list */
static void m5_outlet_soundfileinfo(t_outlet *out, t_soundfile *sf)
{
	t_atom info_list[5];
	SETFLOAT((t_atom *)info_list, (t_float)sf->sf_samplerate);
	SETFLOAT((t_atom *)info_list+1,
		(t_float)(sf->sf_headersize < 0 ? 0 : sf->sf_headersize));
	SETFLOAT((t_atom *)info_list+2, (t_float)sf->sf_nchannels);
	SETFLOAT((t_atom *)info_list+3, (t_float)sf->sf_bytespersample);
	SETSYMBOL((t_atom *)info_list+4, gensym((sf->sf_bigendian ? "b" : "l")));
	outlet_list(out, &s_list, 5, (t_atom *)info_list);
}

	/* post soundfile error, try to print type name */
static void m5_object_sferror(const void *x, const char *header,
	const char *filename, int errnum, const t_soundfile *sf)
{
	if (sf && sf->sf_type)
		pd_error(x, "%s %s: %s: %s", header, sf->sf_type->t_name, filename,
			m5_soundfile_strerror(errnum));
	else
		pd_error(x, "%s: %s: %s", header, filename, m5_soundfile_strerror(errnum));
}

/* ----- soundfile type ----- */

// #define SFMAXTYPES 4
#define SFMAXTYPES 1

/* should these globals be PERTHREAD? */

	/** supported type implementations */
static t_soundfile_type *m5_sf_types[SFMAXTYPES] = {0};

	/** number of types */
static size_t m5_sf_numtypes = 0;

	/** min required header size, largest among the current types */
static size_t m5_sf_minheadersize = 0;

	/** printable type argument list,
	   dash prepended and separated by spaces */
static char m5_sf_typeargs[MAXPDSTRING] = {0};

	/* built-in type implementations */
void m5_soundfile_wave_setup(void);
// void soundfile_aiff_setup(void);
// void soundfile_caf_setup(void);
// void soundfile_next_setup(void);

	/** set up built-in types */
void m5_soundfile_type_setup(void)
{
	m5_soundfile_wave_setup(); /* default first */
	// soundfile_aiff_setup();
	// soundfile_caf_setup();
	// soundfile_next_setup();
}

int m5_soundfile_addtype(const t_soundfile_type *type)
{
	int i;
	if (m5_sf_numtypes == SFMAXTYPES)
	{
		pd_error(0, "soundfile: max number of type implementations reached");
		return 0;
	}
	m5_sf_types[m5_sf_numtypes] = (t_soundfile_type *)type;
	m5_sf_numtypes++;
	if (type->t_minheadersize > m5_sf_minheadersize)
		m5_sf_minheadersize = type->t_minheadersize;
	strcat(m5_sf_typeargs, (m5_sf_numtypes > 1 ? " -" : "-"));
	strcat(m5_sf_typeargs, type->t_name);
	return 1;
}

	/** return type list head pointer */
static t_soundfile_type **m5_soundfile_firsttype(void)
{
	return &m5_sf_types[0];
}

	/** return next type list pointer or NULL if at the end */
static t_soundfile_type **m5_soundfile_nexttype(t_soundfile_type **t)
{
	return (t == &m5_sf_types[m5_sf_numtypes-1] ? NULL : ++t);
}

	/** find type by name, returns NULL if not found */
static t_soundfile_type *m5_soundfile_findtype(const char *name)
{
	t_soundfile_type **t = m5_soundfile_firsttype();
	while (t)
	{
		if (!strcmp(name, (*t)->t_name))
			break;
		t = m5_soundfile_nexttype(t);
	}
	return (t ? *t : NULL);
}

/* ----- ASCII ----- */

	/** compound ascii read/write args  */
typedef struct _asciiargs
{
	ssize_t aa_onsetframe;  /* start frame */
	ssize_t aa_nframes;     /* nframes to read/write */
	int aa_nchannels;       /* number of channels to read/write */
	t_word **aa_vectors;    /* vectors to read into/write out of */
	t_garray **aa_garrays;  /* read: arrays to resize & read into */
	int aa_resize;          /* read: resize when reading? */
	size_t aa_maxsize;      /* read: max size to read/resize to */
	t_sample aa_normfactor; /* write: normalization factor */
} t_asciiargs;

static int m5_ascii_hasextension(const char *filename, size_t size)
{
	int len = strnlen(filename, size);
	if (len >= 5 && !strncmp(filename + (len - 4), ".txt", 4))
		return 1;
	return 0;
}

static int m5_ascii_addextension(char *filename, size_t size)
{
	size_t len = strnlen(filename, size);
	if (len + 4 >= size)
		return 0;
	strcpy(filename + len, ".txt");
	return 1;
}

/* ----- read write ----- */

ssize_t m5_fd_read(int fd, off_t offset, void *dst, size_t size)
{
	if (lseek(fd, offset, SEEK_SET) != offset)
		return -1;
	return read(fd, dst, size);
}

ssize_t m5_fd_write(int fd, off_t offset, const void *src, size_t size)
{
	if (lseek(fd, offset, SEEK_SET) != offset)
		return -1;
	return write(fd, src, size);
}

/* ----- byte swappers ----- */

int m5_sys_isbigendian(void)
{
	unsigned short s = 1;
	unsigned char c = *(char *)(&s);
	return (c == 0);
}

uint64_t m5_swap8(uint64_t n, int doit)
{
	if (doit)
		return (((n >> 56) & 0x00000000000000ffULL) |
				((n >> 40) & 0x000000000000ff00ULL) |
				((n >> 24) & 0x0000000000ff0000ULL) |
				((n >>  8) & 0x00000000ff000000ULL) |
				((n <<  8) & 0x000000ff00000000ULL) |
				((n << 24) & 0x0000ff0000000000ULL) |
				((n << 40) & 0x00ff000000000000ULL) |
				((n << 56) & 0xff00000000000000ULL));
	return n;
}

int64_t m5_swap8s(int64_t n, int doit)
{
	if (doit)
	{
		n = ((n <<  8) & 0xff00ff00ff00ff00ULL) |
			((n >>  8) & 0x00ff00ff00ff00ffULL);
		n = ((n << 16) & 0xffff0000ffff0000ULL) |
			((n >> 16) & 0x0000ffff0000ffffULL );
		return (n << 32) | ((n >> 32) & 0xffffffffULL);
	}
	return n;
}

uint32_t m5_swap4(uint32_t n, int doit)
{
	if (doit)
		return (((n & 0x0000ff) << 24) | ((n & 0x0000ff00) <<  8) |
				((n & 0xff0000) >>  8) | ((n & 0xff000000) >> 24));
	return n;
}

int32_t m5_swap4s(int32_t n, int doit)
{
	if (doit)
	{
		n = ((n << 8) & 0xff00ff00) | ((n >> 8) & 0xff00ff);
		return (n << 16) | ((n >> 16) & 0xffff);
	}
	return n;
}

uint16_t m5_swap2(uint16_t n, int doit)
{
	if (doit)
		return (((n & 0x00ff) << 8) | ((n & 0xff00) >> 8));
	return n;
}

void m5_swapstring4(char *foo, int doit)
{
	if (doit)
	{
		char a = foo[0], b = foo[1], c = foo[2], d = foo[3];
		foo[0] = d; foo[1] = c; foo[2] = b; foo[3] = a;
	}
}

void m5_swapstring8(char *foo, int doit)
{
	if (doit)
	{
		char a = foo[0], b = foo[1], c = foo[2], d = foo[3],
		e = foo[4], f = foo[5], g = foo[6], h = foo[7];
		foo[0] = h; foo[1] = g; foo[2] = f; foo[3] = e;
		foo[4] = d; foo[5] = c; foo[6] = b; foo[7] = a;
	}
}

/* ----------------------- soundfile access routines ----------------------- */

	/** This routine opens a file, looks for a supported file format
		header, seeks to end of it, and fills in the soundfile header info
		values. Only 2- and 3-byte fixed-point samples and 4-byte floating point
		samples are supported.  If sf->sf_headersize is nonzero, the caller
		should supply the number of channels, endinanness, and bytes per sample;
		the header is ignored.  If sf->sf_type is non-NULL, the given type
		implementation is used. Otherwise, the routine tries to read the header
		and fill in the properties. Fills sf struct on success, closes fd on
		failure. */
int m5_open_soundfile_via_fd(int fd, t_soundfile *sf, size_t skipframes)
{
	off_t offset;
	errno = 0;
	if (sf->sf_headersize >= 0) /* header detection overridden */
	{
			/* interpret data size from file size */
		ssize_t bytelimit = lseek(fd, 0, SEEK_END);
		if (bytelimit < 0)
			goto badheader;
		if (bytelimit > SFMAXBYTES || bytelimit < 0)
			bytelimit = SFMAXBYTES;
		sf->sf_bytelimit = bytelimit;
		sf->sf_fd = fd;
	}
	else
	{
		char buf[SFHDRBUFSIZE];
		ssize_t bytesread = read(fd, buf, m5_sf_minheadersize);

		if (!sf->sf_type)
		{
				/* check header for type */
			t_soundfile_type **t = m5_soundfile_firsttype();
			while (t)
			{
				if ((*t)->t_isheaderfn(buf, bytesread))
					break;
				t = m5_soundfile_nexttype(t);
			}
			if (!t) /* not recognized */
			{
				errno = SOUNDFILE_ERRUNKNOWN;
				goto badheader;
			}
			sf->sf_type = *t;
		}
		else
		{
				/* check header using given type */
			if (!sf->sf_type->t_isheaderfn(buf, bytesread))
			{
				errno = SOUNDFILE_ERRUNKNOWN;
				goto badheader;
			}
		}
		sf->sf_fd = fd;

			/* rewind and read header */
		if (lseek(sf->sf_fd, 0, SEEK_SET) < 0)
			goto badheader;
		if (!sf->sf_type->t_readheaderfn(sf))
			goto badheader;
	}

		/* seek past header and any sample frames to skip */
	offset = sf->sf_headersize + (skipframes * sf->sf_bytesperframe);
	if (lseek(sf->sf_fd, offset, 0) < offset)
		goto badheader;
	sf->sf_bytelimit -= skipframes * sf->sf_bytesperframe;
	if (sf->sf_bytelimit < 0)
		sf->sf_bytelimit = 0;

		/* copy sample format back to caller */
	return fd;

badheader:
		/* the header wasn't recognized.  We're threadable here so let's not
		print out the error... */
	if (!errno)
		errno = SOUNDFILE_ERRMALFORMED;
	sf->sf_fd = -1;
	if (fd >= 0)
		sys_close(fd);
	return -1;
}

	/* open a soundfile, using supplied path.  Returns a file descriptor
	or -1 on error. */
int m5_open_soundfile_via_namelist(const char *dirname, const char *filename,
	t_namelist *nl, t_soundfile *sf, size_t skipframes)
{
	char buf[MAXPDSTRING], *dummy;
	int fd, sf_fd;
	// int open_via_path(const char *dir, const char *name, const char *ext,
	// char *dirresult, char **nameresult, unsigned int size, int bin)
	fd = open_via_path(dirname, filename, "", buf, &dummy, MAXPDSTRING, 1);
	// fd = do_open_via_path(dirname, filename, "", buf, &dummy, MAXPDSTRING,
		// 1, nl, 0);
	if (fd < 0)
		return -1;
	sf_fd = m5_open_soundfile_via_fd(fd, sf, skipframes);
	return sf_fd;
}

	/* open a soundfile, using open_via_canvas(). Returns a file descriptor
	or -1 on error. */
int m5_open_soundfile_via_canvas(t_canvas *canvas, const char *filename,
	t_soundfile *sf, size_t skipframes)
{
	char buf[MAXPDSTRING], *dummy;
	int fd, sf_fd;
	fd = canvas_open(canvas, filename, "", buf, &dummy, MAXPDSTRING, 1);
	if (fd < 0)
		return -1;
	sf_fd = m5_open_soundfile_via_fd(fd, sf, skipframes);
	return sf_fd;
}

static int m5_find_threshold(int nchannels, int nframes, t_sample **vecs, t_sample threshold)
{
	// MWS: very simple threshold test
	
	int i, j;
	t_sample *fp;
	for (i = 0; i < nchannels; i++)
	{
		for (j = 0, fp = vecs[i]; j < nframes; j++, fp++)
		{
			if (fabs(*fp) >= threshold ) {
				return j;
			}
		}
	}

	return NOT_FOUND;
	
}


static void m5_soundfile_xferin_sample(const t_soundfile *sf, int nvecs,
	t_sample **vecs, size_t framesread, unsigned char *buf, size_t nframes)
{
	int nchannels = (sf->sf_nchannels < nvecs ? sf->sf_nchannels : nvecs), i;
	size_t j;
	unsigned char *sp, *sp2;
	t_sample *fp;
	for (i = 0, sp = buf; i < nchannels; i++, sp += sf->sf_bytespersample)
	{
		if (sf->sf_bytespersample == 2)
		{
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
						*fp = SCALE * ((sp2[0] << 24) | (sp2[1] << 16));
			}
			else
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
						*fp = SCALE * ((sp2[1] << 24) | (sp2[0] << 16));
			}
		}
		else if (sf->sf_bytespersample == 3)
		{
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
						*fp = SCALE * ((sp2[0] << 24) | (sp2[1] << 16) |
									   (sp2[2] << 8));
			}
			else
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
						*fp = SCALE * ((sp2[2] << 24) | (sp2[1] << 16) |
									   (sp2[0] << 8));
			}
		}
		else if (sf->sf_bytespersample == 4)
		{
			t_floatuint alias;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					alias.ui = ((sp2[0] << 24) | (sp2[1] << 16) |
								(sp2[2] << 8)  |  sp2[3]);
					*fp = (t_sample)alias.f;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					alias.ui = ((sp2[3] << 24) | (sp2[2] << 16) |
								(sp2[1] << 8)  |  sp2[0]);
					*fp = (t_sample)alias.f;
				}
			}
		}
		else if (sf->sf_bytespersample == 8)
		{
			t_doubleuint alias;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					alias.ui = (((uint64_t)sp2[0] << 56) | ((uint64_t)sp2[1] << 48) |
								((uint64_t)sp2[2] << 40) | ((uint64_t)sp2[3] << 32) |
								((uint64_t)sp2[4] << 24) | ((uint64_t)sp2[5] << 16) |
								((uint64_t)sp2[6] << 8)  |  (uint64_t)sp2[7]);
					*fp = (t_sample)alias.d;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					alias.ui = (((uint64_t)sp2[7] << 56) | ((uint64_t)sp2[6] << 48) |
								((uint64_t)sp2[5] << 40) | ((uint64_t)sp2[4] << 32) |
								((uint64_t)sp2[3] << 24) | ((uint64_t)sp2[2] << 16) |
								((uint64_t)sp2[1] << 8)  |  (uint64_t)sp2[0]);
					*fp = (t_sample)alias.d;
				}
			}
		}
	}
		/* zero out other outputs */
	for (i = sf->sf_nchannels; i < nvecs; i++)
		for (j = nframes, fp = vecs[i]; j--;)
			*fp++ = 0;
}

static void m5_soundfile_xferin_words(const t_soundfile *sf, int nvecs,
	t_word **vecs, size_t framesread, unsigned char *buf, size_t nframes)
{
	unsigned char *sp, *sp2;
	t_word *wp;
	int nchannels = (sf->sf_nchannels < nvecs ? sf->sf_nchannels : nvecs), i;
	size_t j;
	for (i = 0, sp = buf; i < nchannels; i++, sp += sf->sf_bytespersample)
	{
		if (sf->sf_bytespersample == 2)
		{
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
						wp->w_float = SCALE * ((sp2[0] << 24) | (sp2[1] << 16));
			}
			else
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
						wp->w_float = SCALE * ((sp2[1] << 24) | (sp2[0] << 16));
			}
		}
		else if (sf->sf_bytespersample == 3)
		{
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
						wp->w_float = SCALE * ((sp2[0] << 24) | (sp2[1] << 16) |
											   (sp2[2] << 8));
			}
			else
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
						wp->w_float = SCALE * ((sp2[2] << 24) | (sp2[1] << 16) |
											   (sp2[0] << 8));
			}
		}
		else if (sf->sf_bytespersample == 4)
		{
			t_floatuint alias;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					alias.ui = ((sp2[0] << 24) | (sp2[1] << 16) |
								(sp2[2] << 8)  |  sp2[3]);
					wp->w_float = (t_float)alias.f;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					alias.ui = ((sp2[3] << 24) | (sp2[2] << 16) |
								(sp2[1] << 8)  |  sp2[0]);
					wp->w_float = (t_float)alias.f;
				}
			}
		}
		else if (sf->sf_bytespersample == 8)
		{
			t_doubleuint alias;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					alias.ui = (((uint64_t)sp2[0] << 56) | ((uint64_t)sp2[1] << 48) |
								((uint64_t)sp2[2] << 40) | ((uint64_t)sp2[3] << 32) |
								((uint64_t)sp2[4] << 24) | ((uint64_t)sp2[5] << 16) |
								((uint64_t)sp2[6] << 8)  |  (uint64_t)sp2[7]);
					wp->w_float = (t_float)alias.d;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + framesread;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					alias.ui = (((uint64_t)sp2[7] << 56) | ((uint64_t)sp2[6] << 48) |
								((uint64_t)sp2[5] << 40) | ((uint64_t)sp2[4] << 32) |
								((uint64_t)sp2[3] << 24) | ((uint64_t)sp2[2] << 16) |
								((uint64_t)sp2[1] << 8)  |  (uint64_t)sp2[0]);
					wp->w_float = (t_float)alias.d;
				}
			}
		}
	}
		/* zero out other outputs */
	for (i = sf->sf_nchannels; i < nvecs; i++)
		for (j = nframes, wp = vecs[i]; j--;)
			(wp++)->w_float = 0;
}

	/* soundfiler_write ...

	   usage: write [flags] filename table ...
	   flags:
		 -nframes <frames>
		 -skip <frames>
		 -bytes <bytespersample>
		 -rate / -r <samplerate>
		 -normalize
		 -wave
		 -aiff
		 -caf
		 -next / -nextstep
		 -ascii
		 -big
		 -little
	*/

	/** parsed write arguments */
typedef struct _soundfiler_writeargs
{
	t_symbol *wa_filesym;             /* file path symbol */
	t_soundfile_type *wa_type;        /* type implementation */
	int wa_samplerate;                /* sample rate */
	int wa_bytespersample;            /* number of bytes per sample */
	int wa_bigendian;                 /* is sample data bigendian? */
	size_t wa_nframes;                /* number of sample frames to write */
	size_t wa_onsetframes;            /* sample frame onset when writing */
	int wa_normalize;                 /* normalize samples? */
	int wa_ascii;                     /* write ascii? */
} t_soundfiler_writeargs;


/* the routine which actually does the work should LATER also be called
from garray_write16. */

	/** Parse arguments for writing.  The "obj" argument is only for flagging
		errors.  For streaming to a file the "normalize" and "nframes"
		arguments shouldn't be set but the calling routine flags this. */
static int m5_soundfiler_parsewriteargs(void *obj, int *p_argc, t_atom **p_argv,
	t_soundfiler_writeargs *wa)
{
	int argc = *p_argc;
	t_atom *argv = *p_argv;
	int samplerate = -1, bytespersample = 2, bigendian = 0, endianness = -1;
	size_t nframes = SFMAXFRAMES, onsetframes = 0;
	int normalize = 0, ascii = 0;
	t_symbol *filesym;
	t_soundfile_type *type = NULL;

	while (argc > 0 && argv->a_type == A_SYMBOL &&
		*argv->a_w.w_symbol->s_name == '-')
	{
		const char *flag = argv->a_w.w_symbol->s_name + 1;
		if (!strcmp(flag, "skip"))
		{
			if (argc < 2 || argv[1].a_type != A_FLOAT ||
				(argv[1].a_w.w_float) < 0)
					return -1;
			onsetframes = argv[1].a_w.w_float;
			argc -= 2; argv += 2;
		}
		else if (!strcmp(flag, "nframes"))
		{
			if (argc < 2 || argv[1].a_type != A_FLOAT ||
				argv[1].a_w.w_float < 0)
					return -1;
			nframes = argv[1].a_w.w_float;
			argc -= 2; argv += 2;
		}
		else if (!strcmp(flag, "bytes"))
		{
			if (argc < 2 || argv[1].a_type != A_FLOAT ||
				((bytespersample = argv[1].a_w.w_float) < 2) ||
					!VALID_BYTESPERSAMPLE(bytespersample))
						return -1;
			argc -= 2; argv += 2;
		}
		else if (!strcmp(flag, "normalize"))
		{
			normalize = 1;
			argc -= 1; argv += 1;
		}
		else if (!strcmp(flag, "big"))
		{
			endianness = 1;
			argc -= 1; argv += 1;
		}
		else if (!strcmp(flag, "little"))
		{
			endianness = 0;
			argc -= 1; argv += 1;
		}
		else if (!strcmp(flag, "rate") || !strcmp(flag, "r"))
		{
			if (argc < 2 || argv[1].a_type != A_FLOAT ||
				((samplerate = argv[1].a_w.w_float) <= 0))
					return -1;
			argc -= 2; argv += 2;
		}
		else if (!strcmp(flag, "ascii"))
		{
			ascii = 1;
			argc -= 1; argv += 1;
		}
		else if (!strcmp(flag, "nextstep"))
		{
				/* handle old "-nextstep" alias */
			type = m5_soundfile_findtype("next");
			argc -= 1; argv += 1;
		}
		else
		{
				/* check for type by name */
			if (!(type = m5_soundfile_findtype(flag)))
				return -1; /* unknown flag */
			ascii = 0; /* replaced */
			argc -= 1; argv += 1;
		}
	}
	if (!argc || argv->a_type != A_SYMBOL)
		return -1;
	filesym = argv->a_w.w_symbol;

		/* deduce from filename extension? */
	if (!type)
	{
		t_soundfile_type **t = m5_soundfile_firsttype();
		while (t)
		{
			if ((*t)->t_hasextensionfn(filesym->s_name, MAXPDSTRING))
				break;
			t = m5_soundfile_nexttype(t);
		}
		if (!t)
		{
			if (!ascii)
				ascii = m5_ascii_hasextension(filesym->s_name, MAXPDSTRING);
			t = m5_soundfile_firsttype(); /* default if unknown */
		}
		type = *t;
	}

		/* check requested endianness */
	bigendian = type->t_endiannessfn(endianness, bytespersample);
	if (endianness != -1 && endianness != bigendian)
	{
		post("%s: forced to %s endian", type->t_name,
			(bigendian ? "big" : "little"));
	}

		/* return to caller */
	argc--; argv++;
	*p_argc = argc;
	*p_argv = argv;
	wa->wa_filesym = filesym;
	wa->wa_type = type;
	wa->wa_samplerate = samplerate;
	wa->wa_bytespersample = bytespersample;
	wa->wa_bigendian = bigendian;
	wa->wa_nframes = nframes;
	wa->wa_onsetframes = onsetframes;
	wa->wa_normalize = normalize;
	wa->wa_ascii = ascii;
	return 0;
}

	/** sets sf fd & headerisze on success and returns fd or -1 on failure */
static int m5_create_soundfile(t_canvas *canvas, const char *filename,
	t_soundfile *sf, size_t nframes)
{
	char filenamebuf[MAXPDSTRING], pathbuf[MAXPDSTRING];
	ssize_t headersize = -1;
	int fd;

		/* create file */
	strncpy(filenamebuf, filename, MAXPDSTRING);
	if (!sf->sf_type->t_hasextensionfn(filenamebuf, MAXPDSTRING-10))
		if (!sf->sf_type->t_addextensionfn(filenamebuf, MAXPDSTRING-10))
			return -1;
	filenamebuf[MAXPDSTRING-10] = 0; /* FIXME: what is the 10 for? */
	canvas_makefilename(canvas, filenamebuf, pathbuf, MAXPDSTRING);
	if ((fd = sys_open(pathbuf, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0)
		return -1;
	sf->sf_fd = fd;

		/* write header */
	headersize = sf->sf_type->t_writeheaderfn(sf, nframes);
	if (headersize < 0)
		goto badcreate;
	sf->sf_headersize = headersize;
	return fd;

badcreate:
	sf->sf_fd = -1;
	if (fd >= 0)
		sys_close(fd);
	return -1;
}

static void m5_soundfile_finishwrite(void *obj, const char *filename,
	t_soundfile *sf, size_t nframes, size_t frameswritten)
{
	if (frameswritten >= nframes) return;
	if (nframes < SFMAXFRAMES)
		pd_error(obj, "[soundfiler] write: %ld out of %ld frames written",
			(long)frameswritten, (long)nframes);
	if (sf->sf_type->t_updateheaderfn(sf, frameswritten))
		return;
	m5_object_sferror(obj, "[soundfiler] write", filename, errno, sf);
}

static void m5_soundfile_xferout_sample(const t_soundfile *sf,
	t_sample **vecs, unsigned char *buf, size_t nframes, size_t onsetframes,
	t_sample normalfactor)
{
	int i;
	size_t j;
	unsigned char *sp, *sp2;
	t_sample *fp;
	for (i = 0, sp = buf; i < sf->sf_nchannels; i++,
		sp += sf->sf_bytespersample)
	{
		if (sf->sf_bytespersample == 2)
		{
			t_sample ff = normalfactor * 32768.;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					int xx = 32768. + (*fp * ff);
					xx -= 32768;
					if (xx < -32767)
						xx = -32767;
					if (xx > 32767)
						xx = 32767;
					sp2[0] = (xx >> 8);
					sp2[1] = xx;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					int xx = 32768. + (*fp * ff);
					xx -= 32768;
					if (xx < -32767)
						xx = -32767;
					if (xx > 32767)
						xx = 32767;
					sp2[1] = (xx >> 8);
					sp2[0] = xx;
				}
			}
		}
		else if (sf->sf_bytespersample == 3)
		{
			t_sample ff = normalfactor * 8388608.;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					int xx = 8388608. + (*fp * ff);
					xx -= 8388608;
					if (xx < -8388607)
						xx = -8388607;
					if (xx > 8388607)
						xx = 8388607;
					sp2[0] = (xx >> 16);
					sp2[1] = (xx >> 8);
					sp2[2] = xx;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					int xx = 8388608. + (*fp * ff);
					xx -= 8388608;
					if (xx < -8388607)
						xx = -8388607;
					if (xx > 8388607)
						xx = 8388607;
					sp2[2] = (xx >> 16);
					sp2[1] = (xx >> 8);
					sp2[0] = xx;
				}
			}
		}
		else if (sf->sf_bytespersample == 4)
		{
			t_floatuint f2;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					f2.f = *fp * normalfactor;
					sp2[0] = (f2.ui >> 24); sp2[1] = (f2.ui >> 16);
					sp2[2] = (f2.ui >> 8);  sp2[3] = f2.ui;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					f2.f = *fp * normalfactor;
					sp2[3] = (f2.ui >> 24); sp2[2] = (f2.ui >> 16);
					sp2[1] = (f2.ui >> 8);  sp2[0] = f2.ui;
				}
			}
		}
		else if (sf->sf_bytespersample == 8)
		{
			t_doubleuint f2;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					f2.d = *fp * normalfactor;
					sp2[0] = (f2.ui >> 56); sp2[1] = (f2.ui >> 48);
					sp2[2] = (f2.ui >> 40); sp2[3] = (f2.ui >> 32);
					sp2[4] = (f2.ui >> 24); sp2[5] = (f2.ui >> 16);
					sp2[6] = (f2.ui >> 8);  sp2[7] = f2.ui;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, fp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, fp++)
				{
					f2.d = *fp * normalfactor;
					sp2[7] = (f2.ui >> 56); sp2[6] = (f2.ui >> 48);
					sp2[5] = (f2.ui >> 40); sp2[4] = (f2.ui >> 32);
					sp2[3] = (f2.ui >> 24); sp2[2] = (f2.ui >> 16);
					sp2[1] = (f2.ui >> 8);  sp2[0] = f2.ui;
				}
			}
		}
	}
}

static void m5_soundfile_xferout_words(const t_soundfile *sf, t_word **vecs,
	unsigned char *buf, size_t nframes, size_t onsetframes,
	t_sample normalfactor)
{
	int i;
	size_t j;
	unsigned char *sp, *sp2;
	t_word *wp;
	for (i = 0, sp = buf; i < sf->sf_nchannels;
		 i++, sp += sf->sf_bytespersample)
	{
		if (sf->sf_bytespersample == 2)
		{
			t_sample ff = normalfactor * 32768.;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					int xx = 32768. + (wp->w_float * ff);
					xx -= 32768;
					if (xx < -32767)
						xx = -32767;
					if (xx > 32767)
						xx = 32767;
					sp2[0] = (xx >> 8);
					sp2[1] = xx;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					int xx = 32768. + (wp->w_float * ff);
					xx -= 32768;
					if (xx < -32767)
						xx = -32767;
					if (xx > 32767)
						xx = 32767;
					sp2[1] = (xx >> 8);
					sp2[0] = xx;
				}
			}
		}
		else if (sf->sf_bytespersample == 3)
		{
			t_sample ff = normalfactor * 8388608.;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					int xx = 8388608. + (wp->w_float * ff);
					xx -= 8388608;
					if (xx < -8388607)
						xx = -8388607;
					if (xx > 8388607)
						xx = 8388607;
					sp2[0] = (xx >> 16);
					sp2[1] = (xx >> 8);
					sp2[2] = xx;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					int xx = 8388608. + (wp->w_float * ff);
					xx -= 8388608;
					if (xx < -8388607)
						xx = -8388607;
					if (xx > 8388607)
						xx = 8388607;
					sp2[2] = (xx >> 16);
					sp2[1] = (xx >> 8);
					sp2[0] = xx;
				}
			}
		}
		else if (sf->sf_bytespersample == 4)
		{
			t_floatuint f2;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					f2.f = wp->w_float * normalfactor;
					sp2[0] = (f2.ui >> 24); sp2[1] = (f2.ui >> 16);
					sp2[2] = (f2.ui >> 8);  sp2[3] = f2.ui;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					f2.f = wp->w_float * normalfactor;
					sp2[3] = (f2.ui >> 24); sp2[2] = (f2.ui >> 16);
					sp2[1] = (f2.ui >> 8);  sp2[0] = f2.ui;
				}
			}
		}
		else if (sf->sf_bytespersample == 8)
		{
			t_doubleuint f2;
			if (sf->sf_bigendian)
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					f2.d = wp->w_float * normalfactor;
					sp2[0] = (f2.ui >> 56); sp2[1] = (f2.ui >> 48);
					sp2[2] = (f2.ui >> 40); sp2[3] = (f2.ui >> 32);
					sp2[4] = (f2.ui >> 24); sp2[5] = (f2.ui >> 16);
					sp2[6] = (f2.ui >> 8);  sp2[7] = f2.ui;
				}
			}
			else
			{
				for (j = 0, sp2 = sp, wp = vecs[i] + onsetframes;
					j < nframes; j++, sp2 += sf->sf_bytesperframe, wp++)
				{
					f2.d = wp->w_float * normalfactor;
					sp2[7] = (f2.ui >> 56); sp2[6] = (f2.ui >> 48);
					sp2[5] = (f2.ui >> 40); sp2[4] = (f2.ui >> 32);
					sp2[3] = (f2.ui >> 24); sp2[2] = (f2.ui >> 16);
					sp2[1] = (f2.ui >> 8);  sp2[0] = f2.ui;
				}
			}
		}
	}
}


/* ------------------------- readsf object ------------------------- */

/* READSF uses the Posix threads package; for the moment we're Linux
only although this should be portable to the other platforms.

Each instance of readsf~ owns a "child" thread for doing the Posix file reading.
The parent thread signals the child each time:
	(1) a file wants opening or closing;
	(2) we've eaten another 1/16 of the shared buffer (so that the
		child thread should check if it's time to read some more.)
The child signals the parent whenever a read has completed.  Signaling
is done by setting "conditions" and putting data in mutex-controlled common
areas.
*/

#define MAXVECSIZE 128

#define READSIZE 65536
#define WRITESIZE 65536
#define DEFBUFPERCHAN 262144
#define MINBUFSIZE (4 * READSIZE)
#define MAXBUFSIZE 16777216     /* arbitrary; just don't want to hang malloc */

	/* read/write thread request type */
typedef enum _soundfile_request
{
	REQUEST_NOTHING = 0,
	REQUEST_OPEN    = 1,
	REQUEST_CLOSE   = 2,
	REQUEST_QUIT    = 3,
	REQUEST_BUSY    = 4
} t_soundfile_request;

	/* read/write thread state */
typedef enum _soundfile_state
{
	STATE_IDLE    = 0,
	STATE_STARTUP = 1,
	STATE_STREAM  = 2,
	STATE_STARTUP_2 = 3, // readsf: frames available was reported
	STATE_IDLE_2 = 4,// writesf: waiting for written file to close; haven't reported bytes written
	STATE_STREAM_JUST_STARTING = 5 // writesf: waiting to start, haven't sent 'start on bang' yet
	
} t_soundfile_state;

typedef enum _m5_loop_mode
{
	LOOP_OFF	= 0,
	LOOP_ON		= 1
} t_m5_loop_mode;

#define LOOP_SELF 0
#define START_NOW -1.
#define START_AT_THRESHOLD DBL_MAX
#define END_AT_LOOP -1.
#define END_NEVER DBL_MAX
#define END_NOW 0.

#define FRAMES_NOT_UPDATED SIZE_MAX

typedef enum _m5_sync_mode

{
	SYNC_OFF 	= 0,
	SYNC_GLOBAL = 1
} t_m5_sync_mode;

static t_class *m5_readsf_class;

// 'm5_ prefixed' fields are new additions to readsf
typedef struct _readsf
{
	t_object x_obj;
	t_canvas *x_canvas;
	t_clock *x_clock;
	char *x_buf;                      /**< soundfile buffer */
	int x_bufsize;                    /**< buffer size in bytes */
	int x_noutlets;                   /**< number of audio outlets */
	t_sample *(x_outvec[MAXSFCHANS]); /**< audio vectors */
	int x_vecsize;                    /**< vector size for transfers */
	
	t_outlet *x_m5listOut;			  /** number of frames in file (FTC) */
	
	t_outlet *x_m5startListOut; 	  /** actual start time (FTC *) for m5_writesf */
	
	t_outlet *x_bangout;              /**< bang-on-done outlet */	
	t_soundfile_state x_state;        /**< opened, running, or idle */
	t_float x_insamplerate;           /**< input signal sample rate, if known */
		/* parameters to communicate with subthread */
	t_soundfile_request x_requestcode; /**< pending request to I/O thread */
	const char *x_filename;   /**< file to open (permanently allocated) */
	int x_fileerror;          /**< slot for "errno" return */
	t_soundfile x_sf;         /**< soundfile fd, type, and format info */
	size_t x_onsetframes;     /**< number of sample frames to skip */
	int x_fifosize;           /**< buffer size appropriately rounded down */
	int x_fifohead;           /**< index of next byte to get from file */
	int x_fifotail;           /**< index of next byte the ugen will read */
	int x_eof;                /**< true if fifohead has stopped changing */
	int x_sigcountdown;       /**< counter for signaling child for more data */
	int x_sigperiod;          /**< number of ticks per signal */
	size_t x_frameswritten;   /**< writesf~ only; frames written */
	t_float x_f;              /**< writesf~ only; scalar for signal inlet */
	pthread_mutex_t x_mutex;
	pthread_cond_t x_requestcondition;
	pthread_cond_t x_answercondition;
	pthread_t x_childthread;
	t_namelist *x_namelist;
	
	/* used by 'perform' to signal outlets to send outputs before next block */
	t_clock *x_m5FramesOutClock; 
	t_clock *x_m5StartTimeOutClock;	
	
	/* values to for writing to output */
	size_t x_m5SoundFileFramesAvailableFromOnset;
	size_t x_m5FramesWrittenReport;
	double x_m5WriteStartTimeReport;

	double x_m5HeadTimeRequest;
	double x_m5TailTime;
	
	/* m5_ftc_anchor referenced by ID */
	/* used for common t=0 time */	
	t_symbol *x_m5TimeAnchorName;
	t_m5TimeAnchor *x_m5TimeAnchor;
	
	/* store t=0 if m5_ftc_anchor is not specified */
	double x_m5LocalTimeAnchor;

	
	size_t x_m5LoopLength; /* loop length */
	char x_m5LoopLengthRequest; /* loop start/length change was requested via inlet */
	size_t x_m5LoopStart; /* loop start offset in sample */
	
	double x_m5PlayStartTime; /* frame to start reading / writing */
	double x_m5PlayEndTime; /*frame to stop reading / writing */
	int x_m5PerformedFifoSize; /* store how many frames have been buffered by writesf so far */
	
	t_sample x_m5PlayStartThreshold; /* input signal threshold to detect */
	
#ifdef PDINSTANCE
	t_pdinstance *x_pd_this;  /**< pointer to the owner pd instance */
#endif
} t_readsf;

/* ----- the child thread which performs file I/O ----- */

	/** thread state debug prints to stderr */
//#define DEBUG_SOUNDFILE_THREADS

#if 1
#define sfread_cond_wait pthread_cond_wait
#define sfread_cond_signal pthread_cond_signal
#else
#include <sys/time.h>    /* debugging version... */
#include <sys/types.h>
static void m5_readsf_fakewait(pthread_mutex_t *b)
{
	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 1000000;
	pthread_mutex_unlock(b);
	select(0, 0, 0, 0, &timeout);
	pthread_mutex_lock(b);
}

#define sfread_cond_wait(a,b) readsf_fakewait(b)
#define sfread_cond_signal(a)
#endif

static void *m5_readsf_child_main(void *zz)
{
	t_readsf *x = zz;
	t_soundfile sf = {0};
	ssize_t m5_original_bytelimit = 0;
	size_t m5_seek_max = 0;
	off_t m5_initial_offset = 0;
	
	m5_soundfile_clear(&sf);
#ifdef PDINSTANCE
	pd_this = x->x_pd_this;
#endif
#ifdef DEBUG_SOUNDFILE_THREADS
	fprintf(stderr, "readsf~: 1\n");
#endif
	pthread_mutex_lock(&x->x_mutex);
	while (1)
	{
		int fifohead;
		char *buf;
#ifdef DEBUG_SOUNDFILE_THREADS
		fprintf(stderr, "readsf~: 0\n");
#endif
		if (x->x_requestcode == REQUEST_NOTHING)
		{
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "readsf~: wait 2\n");
#endif
			sfread_cond_signal(&x->x_answercondition);
			sfread_cond_wait(&x->x_requestcondition, &x->x_mutex);
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "readsf~: 3\n");
#endif
		}
		else if (x->x_requestcode == REQUEST_OPEN)
		{
			ssize_t bytesread;
			size_t wantbytes;		
			off_t nextSeek = 0; 	

				/* copy file stuff out of the data structure so we can
				relinquish the mutex while we're in open_soundfile_via_path() */
			size_t onsetframes = x->x_onsetframes;
			// size_t loop_length_bytes = 0;
			const char *filename = x->x_filename;
			const char *dirname = canvas_getdir(x->x_canvas)->s_name;

#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "readsf~: 4\n");
#endif
				/* alter the request code so that an ensuing "open" will get
				noticed. */
			x->x_requestcode = REQUEST_BUSY;
			x->x_fileerror = 0;

				/* if there's already a file open, close it */
			if (sf.sf_fd >= 0)
			{
				pthread_mutex_unlock(&x->x_mutex);
				sys_close(sf.sf_fd);
				sf.sf_fd = -1;
				pthread_mutex_lock(&x->x_mutex);
				x->x_sf.sf_fd = -1;
				if (x->x_requestcode != REQUEST_BUSY)
					goto lost;
			}
				/* cache sf *after* closing as x->sf's type
					may have changed in readsf_open() */
			m5_soundfile_copy(&sf, &x->x_sf);
				/* open the soundfile with the mutex unlocked */
			pthread_mutex_unlock(&x->x_mutex);
			m5_open_soundfile_via_namelist(dirname, filename, x->x_namelist,
				&sf, onsetframes);
			pthread_mutex_lock(&x->x_mutex);
			
			// get maximum size of loop, in bytes, that contains all sound data in file after 
			// 'onset' frames 
			m5_original_bytelimit = sf.sf_bytelimit;
			
			// lowest offset in file, after 'onset' frames
			m5_initial_offset = sf.sf_headersize + (onsetframes * sf.sf_bytesperframe);
			
			// highest offset in file
			m5_seek_max = m5_original_bytelimit + m5_initial_offset;
		

#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "readsf~: 5\n");
#endif
			if (sf.sf_fd < 0)
			{
				x->x_fileerror = errno;
				x->x_eof = 1;
#ifdef DEBUG_SOUNDFILE_THREADS
				fprintf(stderr, "readsf~: open failed %s %s\n",
					filename, dirname);
#endif
				goto lost;
			}
				/* copy back into the instance structure. */
			m5_soundfile_copy(&x->x_sf, &sf);
				/* check if another request has been made; if so, field it */
			if (x->x_requestcode != REQUEST_BUSY)
				goto lost;
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "readsf~: 6\n");
#endif
			x->x_fifohead = 0;
					/* set fifosize from bufsize.  fifosize must be a
					multiple of the number of bytes eaten for each DSP
					tick.  We pessimistically assume MAXVECSIZE samples
					per tick since that could change.  There could be a
					problem here if the vector size increases while a
					soundfile is being played...  */
			x->x_fifosize = x->x_bufsize - (x->x_bufsize %
				(sf.sf_bytesperframe * MAXVECSIZE));
					/* arrange for the "request" condition to be signaled 16
					times per buffer */
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "readsf~: fifosize %d\n", x->x_fifosize);
#endif
			x->x_sigcountdown = x->x_sigperiod = (x->x_fifosize /
				(16 * sf.sf_bytesperframe * x->x_vecsize));
				/* in a loop, wait for the fifo to get hungry and feed it */
			
			// int seekstartflag = 0;
			
			while (x->x_requestcode == REQUEST_BUSY)
			{
				// 
				
				int fifosize = x->x_fifosize;
#ifdef DEBUG_SOUNDFILE_THREADS
				fprintf(stderr, "readsf~: 77\n");
#endif
				// actual loop length, always +ve
				size_t loop_length_bytes = 0;
				
				// determine actual loop length... either use available loop length in file, or pre-defined
				if (x->x_m5LoopLength == LOOP_SELF) {
					loop_length_bytes = m5_original_bytelimit;
				} else {
					loop_length_bytes = sf.sf_bytesperframe * x->x_m5LoopLength;
				}
				
				// cannot have 0 loop length!
				if (loop_length_bytes == 0)
				{
					x->x_eof = 1;
					x->x_fileerror = SOUNDFILE_M5_ERREMPTY;
					goto lost;
				}
				
				// user-defined start time for loop file, in bytes 
				// added to 'onset'
				size_t loop_start_bytes = sf.sf_bytesperframe * x->x_m5LoopStart;

				// Usually 'nextseek' is auto-incremented as we read along the file.
				// When head and tail are equal, there is a request for a fresh buffer, 
				// so synchronize nextseek with newly requested time
				ssize_t byte_time = 0;
				if (x->x_fifohead == 0 && x->x_fifotail == 0) 
				{
					// get the time requested to start playing the loop
					double pst = x->x_m5PlayStartTime;
					if (pst < 0) pst = 0;
					
					// current frame time at 'head', in bytes, relative to time anchor
					byte_time = ((ssize_t)x->x_m5HeadTimeRequest - (ssize_t)pst) * (ssize_t)sf.sf_bytesperframe;
					if (byte_time >= 0)
					{
						// calculate time within current audio loop
						// Note: nextSeek can point past actual end of file. We will insert silence later for that instead.
						nextSeek = (byte_time % loop_length_bytes) + m5_initial_offset + loop_start_bytes;
						
					} else {
						// the current time is 'before' play start time
						// Work backward to find time within current audio loop.
						// This will allow us to start playing samples if we cross the 'start time' boundary during 
						// this read iteration.
						nextSeek = loop_length_bytes - ((-1 * byte_time) % loop_length_bytes) + m5_initial_offset + loop_start_bytes;

					}
					
				}				

				// nudge it around if on exactly the end of the loop
				
				if (nextSeek == loop_length_bytes + m5_initial_offset + loop_start_bytes) {
					nextSeek = m5_initial_offset + loop_start_bytes;
				}
				
				// max number of bytes that can be copied into FIFO before end of current audio loop
				// We will go back to the beginning of the audio loop in the next iteration of this While loop
				size_t loop_byte_limit = loop_length_bytes + m5_initial_offset + loop_start_bytes - nextSeek ;
				
				if (x->x_fifohead >= x->x_fifotail)
				{
						/* if the head is >= the tail, we can immediately read
						to the end of the fifo.  Unless, that is, we would
						read all the way to the end of the buffer and the
						"tail" is zero; this would fill the buffer completely
						which isn't allowed because you can't tell a completely
						full buffer from an empty one. */
					if (x->x_fifotail || (fifosize - x->x_fifohead > READSIZE))
					{
						wantbytes = fifosize - x->x_fifohead;
						// only read up to READSIZE
						if (wantbytes > READSIZE)
							wantbytes = READSIZE;
						
						// only read up to end of audio loop
						if (wantbytes > loop_byte_limit)
						{
							wantbytes = loop_byte_limit;
						}
#ifdef DEBUG_SOUNDFILE_THREADS
						fprintf(stderr, "readsf~: head %d, tail %d, size %ld\n",
							x->x_fifohead, x->x_fifotail, wantbytes);
#endif
					}
					else
					{
#ifdef DEBUG_SOUNDFILE_THREADS
						fprintf(stderr, "readsf~: wait 7a...\n");
#endif
						sfread_cond_signal(&x->x_answercondition);
#ifdef DEBUG_SOUNDFILE_THREADS
						fprintf(stderr, "readsf~: signaled...\n");
#endif
						sfread_cond_wait(&x->x_requestcondition, &x->x_mutex);
#ifdef DEBUG_SOUNDFILE_THREADS
						fprintf(stderr, "readsf~: 7a ... done\n");
#endif
						continue;
					}
				}
				else
				{
						/* otherwise check if there are at least READSIZE
						bytes to read.  If not, wait and loop back. */
					wantbytes =  x->x_fifotail - x->x_fifohead - 1;
					if (wantbytes < READSIZE)
					{					
#ifdef DEBUG_SOUNDFILE_THREADS
						fprintf(stderr, "readsf~: wait 7...\n");
#endif
						sfread_cond_signal(&x->x_answercondition);
						sfread_cond_wait(&x->x_requestcondition, &x->x_mutex);
#ifdef DEBUG_SOUNDFILE_THREADS
						fprintf(stderr, "readsf~: 7 ... done\n");
#endif
						continue;
					}
					else wantbytes = READSIZE;
					if (wantbytes > loop_byte_limit)
					{
						wantbytes = loop_byte_limit;
					}
				}
#ifdef DEBUG_SOUNDFILE_THREADS
				fprintf(stderr, "readsf~: 8\n");
#endif
				buf = x->x_buf;
				fifohead = x->x_fifohead;
				
				
				off_t bytesSought = 0;
				int last_fifohead = x->x_fifohead;
				double last_headTimeRequest = x->x_m5HeadTimeRequest;
				pthread_mutex_unlock(&x->x_mutex);
				
				// if nextSeek is within actual file
				if (nextSeek < (off_t)m5_seek_max) 
				{
					bytesSought = lseek(sf.sf_fd, nextSeek, SEEK_SET);
				}
				else 
				{
					bytesSought = nextSeek;
				}
				
				// don't read past end of the file
				ssize_t actual_bytes_to_want =  ((ssize_t)m5_seek_max - (ssize_t)nextSeek);
				
				if (actual_bytes_to_want > (ssize_t)wantbytes) 
				{ 
					actual_bytes_to_want = wantbytes;
				} else if (actual_bytes_to_want < 0) 
				{
					actual_bytes_to_want = 0;
				}	

				// zeroes to fill out FIFO if our audio loop extends past end of file
				ssize_t wantzeroes = wantbytes - actual_bytes_to_want;
#ifdef DEBUG_READ_LOOP
				fprintf(stderr, "loop: %ld, %ld %ld %ld %ld %ld %ld %ld %ld\n", byte_time, loop_length_bytes, nextSeek, wantbytes, actual_bytes_to_want, wantzeroes, m5_seek_max, loop_byte_limit, m5_initial_offset);
#endif

				
				bytesread = read(sf.sf_fd, buf + fifohead, actual_bytes_to_want);
				
				ssize_t i = 0;
				
				// add silence to the rest 
				char * b = buf + fifohead + actual_bytes_to_want;
				for (; i < wantzeroes; i++)
					*b++ = 0;
				
				
				pthread_mutex_lock(&x->x_mutex);
				if (x->x_requestcode != REQUEST_BUSY)
					break;
				if (bytesread < 0 || bytesSought != nextSeek)
				{
#ifdef DEBUG_SOUNDFILE_THREADS
					fprintf(stderr, "readsf~: fileerror %d\n", errno);
#endif
					x->x_fileerror = errno;
					break;
				}
				else if (bytesread == 0 && actual_bytes_to_want > 0)
				{
					// couldn't read from file for some reason
					goto lost;
				}
				else
				{
					// Make sure fifohead wasn't reset by parent process during read, then auto-increment
					// otherwise nextSeek will be updated above based on playStartTime and current time
					if (x->x_fifohead == last_fifohead && x->x_m5HeadTimeRequest == last_headTimeRequest) {
						x->x_fifohead += bytesread + wantzeroes;
						if (x->x_fifohead == fifosize)
							x->x_fifohead = 0;
						nextSeek += bytesread + wantzeroes;
						// If the math works out, we should always end up at exactly the end of the loop when we get to the end
						if (nextSeek == m5_initial_offset + (off_t)loop_length_bytes + (off_t)loop_start_bytes)		
						{
							nextSeek = m5_initial_offset + (off_t)loop_start_bytes;
						}
					}
				}
#ifdef DEBUG_SOUNDFILE_THREADS
				fprintf(stderr, "readsf~: after, head %d tail %d\n",
					x->x_fifohead, x->x_fifotail);
#endif
					/* signal parent in case it's waiting for data */
				sfread_cond_signal(&x->x_answercondition);
			}

		lost:
			if (x->x_requestcode == REQUEST_BUSY)
				x->x_requestcode = REQUEST_NOTHING;
				/* fell out of read loop: close file if necessary,
				set EOF and signal once more */
			if (sf.sf_fd >= 0)
			{
					/* only set EOF if there is no pending "open" request!
					Otherwise, we might accidentally set EOF after it has been
					unset in readsf_open() and the stream would fail silently. */
				if (x->x_requestcode != REQUEST_OPEN)
					x->x_eof = 1;
				x->x_sf.sf_fd = -1;
					/* use cached sf */
				pthread_mutex_unlock(&x->x_mutex);
				sys_close(sf.sf_fd);
				sf.sf_fd = -1;
				pthread_mutex_lock(&x->x_mutex);
			}
			sfread_cond_signal(&x->x_answercondition);
		}
		else if (x->x_requestcode == REQUEST_CLOSE)
		{
			if (sf.sf_fd >= 0)
			{
				x->x_sf.sf_fd = -1;
					/* use cached sf */
				pthread_mutex_unlock(&x->x_mutex);
				sys_close(sf.sf_fd);
				sf.sf_fd = -1;
				pthread_mutex_lock(&x->x_mutex);
			}
			if (x->x_requestcode == REQUEST_CLOSE)
				x->x_requestcode = REQUEST_NOTHING;
			sfread_cond_signal(&x->x_answercondition);
		}
		else if (x->x_requestcode == REQUEST_QUIT)
		{
			if (sf.sf_fd >= 0)
			{
				x->x_sf.sf_fd = -1;
					/* use cached sf */
				pthread_mutex_unlock(&x->x_mutex);
				sys_close(sf.sf_fd);
				sf.sf_fd = -1;
				pthread_mutex_lock(&x->x_mutex);
			}
			x->x_requestcode = REQUEST_NOTHING;
			sfread_cond_signal(&x->x_answercondition);
			break;
		}
		else
		{
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "readsf~: 13\n");
#endif
		}
	}
#ifdef DEBUG_SOUNDFILE_THREADS
	fprintf(stderr, "readsf~: thread exit\n");
#endif
	pthread_mutex_unlock(&x->x_mutex);
	return 0;
}

/* ----- the object proper runs in the calling (parent) thread ----- */

static void m5_readsf_tick(t_readsf *x);
static void m5_readsf_frame_out_tick(t_readsf *x);

static void *m5_readsf_new(t_floatarg fnchannels, t_floatarg fbufsize)
{
	t_readsf *x;
	int nchannels = fnchannels, bufsize = fbufsize, i;
	char *buf;

	if (nchannels < 1)
		nchannels = 1;
	else if (nchannels > MAXSFCHANS)
		nchannels = MAXSFCHANS;
	if (bufsize <= 0) bufsize = DEFBUFPERCHAN * nchannels;
	else if (bufsize < MINBUFSIZE)
		bufsize = MINBUFSIZE;
	else if (bufsize > MAXBUFSIZE)
		bufsize = MAXBUFSIZE;
	buf = getbytes(bufsize);
	if (!buf) return 0;

	x = (t_readsf *)pd_new(m5_readsf_class);

	for (i = 0; i < nchannels; i++)
		outlet_new(&x->x_obj, gensym("signal"));
	x->x_noutlets = nchannels;
	
	x->x_bangout = outlet_new(&x->x_obj, &s_bang);
	x->x_m5listOut = outlet_new(&x->x_obj, &s_anything);
	
	pthread_mutex_init(&x->x_mutex, 0);
	pthread_cond_init(&x->x_requestcondition, 0);
	pthread_cond_init(&x->x_answercondition, 0);
	x->x_vecsize = MAXVECSIZE;
	x->x_state = STATE_IDLE;
	x->x_clock = clock_new(x, (t_method)m5_readsf_tick);
	x->x_m5FramesOutClock = clock_new(x, (t_method)m5_readsf_frame_out_tick);
	x->x_canvas = canvas_getcurrent();
	m5_soundfile_clear(&x->x_sf);
	x->x_sf.sf_bytespersample = 2;
	x->x_sf.sf_nchannels = 1;
	x->x_sf.sf_bytesperframe = 2;
	x->x_buf = buf;
	x->x_bufsize = bufsize;
	x->x_fifosize = x->x_fifohead = x->x_fifotail = 0; 
	x->x_requestcode = 0;
	// x->x_m5FramesPlayed = 0;
	x->x_m5SoundFileFramesAvailableFromOnset = 0;
	x->x_m5LoopLength = LOOP_SELF;
	x->x_m5LoopLengthRequest = 0;
	x->x_m5LoopStart = 0;
	x->x_namelist = 0;
	
	x->x_m5HeadTimeRequest = 0;
	x->x_m5TailTime = 0;
	x->x_m5TimeAnchorName = 0;
	x->x_m5TimeAnchor = 0;
	x->x_m5LocalTimeAnchor = 0;
	
	x->x_m5PlayStartTime = 0;
	x->x_m5PlayEndTime = END_AT_LOOP;
	x->x_m5PlayStartThreshold = 0;
	
	
#ifdef PDINSTANCE
	x->x_pd_this = pd_this;
#endif
	pthread_create(&x->x_childthread, 0, m5_readsf_child_main, x);
	return x;
}

static void m5_readsf_tick(t_readsf *x)
{
	outlet_bang(x->x_bangout);
}

static void m5_readsf_frame_out_tick(t_readsf *x)
{

	t_m5FrameTimeCode ftc;
	m5_frame_time_code_from_frames(x->x_m5SoundFileFramesAvailableFromOnset, &ftc);
	m5_frame_time_code_out(&ftc, x->x_m5listOut);
}

static t_int *m5_readsf_perform(t_int *w)
{
	t_readsf *x = (t_readsf *)(w[1]);
	int vecsize = x->x_vecsize, noutlets = x->x_noutlets, i;
	size_t j;
	t_sample *fp;
		
	if (x->x_state == STATE_STREAM)
	{
		int wantbytes;
		t_soundfile sf = {0};
		pthread_mutex_lock(&x->x_mutex);
		
		// Don't play anything until file has been opened and number of frames in file reported
		if (x->x_m5SoundFileFramesAvailableFromOnset == 0)  {
					// get file length and send it to the outlet once if ready
			
			// sf_bytelimit reports the bytes from the child thread		
			if (x->x_sf.sf_bytesperframe > 0 && x->x_sf.sf_bytelimit != SFMAXBYTES) {
				x->x_m5SoundFileFramesAvailableFromOnset = x->x_sf.sf_bytelimit / x->x_sf.sf_bytesperframe;
			}
			// if found sf_bytelimit != SFMAXBYTES
			if (x->x_m5SoundFileFramesAvailableFromOnset > 0) {				
				clock_delay(x->x_m5FramesOutClock, 0);
			} else {
				
				// either error, or still waiting (play silence for now and return)
				if (x->x_fileerror) {
					m5_object_sferror(x, "[readsf~]", x->x_filename,x->x_fileerror, &x->x_sf);
					x->x_state = STATE_IDLE;
					clock_delay(x->x_m5FramesOutClock, 0);
					clock_delay(x->x_clock, 0);	
				}
				pthread_mutex_unlock(&x->x_mutex);
				for (i = 0; i < noutlets; i++){
					for (j = vecsize, fp = x->x_outvec[i]; j--;){
						*fp++ = 0;
					}
				}

				
				
				return w+2;
			}	

		}
		
		/* copy with mutex locked! */
		
		m5_soundfile_copy(&sf, &x->x_sf);
		
		size_t blockStartTime = 0; // frame count since time anchor
		if (x->x_m5TimeAnchor) 
		{
			// shared time anchor
			blockStartTime = m5_time_anchor_get_time_since_start(x->x_m5TimeAnchor);
		} 
		else 
		{
			// local clock for this object
			double d = ceil(clock_gettimesincewithunits(x->x_m5LocalTimeAnchor, 1, 1));
			if (d < 0.) { d = 0.;}
			blockStartTime = (size_t)d;
		}
				
		// request to start relative to next immediate block
		if (x->x_m5PlayStartTime == START_NOW)  
		{
			x->x_m5PlayStartTime = (double)blockStartTime;
		}
		
		
		
		// reset the fifo so that it starts filling from the current block time
		// Do this if parameters (start/length) for audio loop changed
		// if (x->x_m5LoopLengthRequest || ((size_t)x->x_m5TailTime != (size_t)blockStartTime)) {		
		if (x->x_m5LoopLengthRequest) {		
			x->x_m5LoopLengthRequest = 0;
			x->x_fifohead = x->x_fifotail = x->x_eof = 0;
		}
		
		// if the tail
		// somehow is not lined up with the current needed frame clock, check to see if we can fast-forward
		// otherwise reset the fifo like with x_m5LoopLengthRequest
		if ((size_t)x->x_m5TailTime != (size_t)blockStartTime) {
			ssize_t time_out = (ssize_t)blockStartTime - (ssize_t)x->x_m5TailTime;
			if (time_out < 0) {
				x->x_fifohead = x->x_fifotail = x->x_eof = 0;
			}
			size_t forward_limit = 0;
			if (x->x_fifohead < x->x_fifotail) {
				forward_limit = x->x_fifosize;
			} else {
				forward_limit = x->x_fifohead;
			}
			if ((time_out + x->x_fifotail < forward_limit)) {
				x->x_fifotail += time_out;
				x->x_m5TailTime = blockStartTime;
			} else {
				x->x_fifohead = x->x_fifotail = x->x_eof = 0;
			}

		}
		
		// tail and head wound up in the same place, so update start time
		if (x->x_fifohead == x->x_fifotail)
		{
			// tell the child where we need to start reading based on frame clock
			x->x_m5HeadTimeRequest = blockStartTime;
			x->x_m5TailTime = blockStartTime;
		}
		
		wantbytes = vecsize * sf.sf_bytesperframe;
		
		
		// if fifo is not ready, play silence and return
		if (!x->x_eof && x->x_fifohead >= x->x_fifotail &&
		x->x_fifohead < x->x_fifotail + wantbytes-1) 
		{
			sfread_cond_signal(&x->x_requestcondition);
			pthread_mutex_unlock(&x->x_mutex);
			for (i = 0; i < noutlets; i++){
				for (j = vecsize, fp = x->x_outvec[i]; j--;){
					*fp++ = 0;
				}
			}
			
			
			
			return w+2;
		}
		
// 		// fill fifo (and wait for file to finish opening, if needed)	
// 		while (!x->x_eof && x->x_fifohead >= x->x_fifotail &&
// 				x->x_fifohead < x->x_fifotail + wantbytes-1)
// 		{
// 			
// #ifdef DEBUG_SOUNDFILE_THREADS
// 			fprintf(stderr, "readsf~: wait...\n");
// #endif
// 			sfread_cond_signal(&x->x_requestcondition);
// 			sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
// 				/* resync local variables -- bug fix thanks to Shahrokh */
// 			vecsize = x->x_vecsize;
// 			m5_soundfile_copy(&sf, &x->x_sf);
// 			wantbytes = vecsize * sf.sf_bytesperframe;
// 		
// #ifdef DEBUG_SOUNDFILE_THREADS
// 			fprintf(stderr, "readsf~: ... done\n");
// #endif
// 		}

		if (x->x_fileerror) {
			m5_object_sferror(x, "[readsf~]", x->x_filename,x->x_fileerror, &x->x_sf);
			x->x_state = STATE_IDLE;
			clock_delay(x->x_clock, 0);	
			/* send bang and zero out the (rest of the) output */
			pthread_mutex_unlock(&x->x_mutex);
				
					
			for (i = 0; i < noutlets; i++)
				for (j = vecsize, fp = x->x_outvec[i]; j--;)
					*fp++ = 0;
			return w + 2;
		}
		
		// There was a request to set the end time to the end of the current loop.
		// We need to calculate x_m5PlayEndTime here in case loop length depends on # of frames in opened soundfile
		if (x->x_m5PlayEndTime == END_AT_LOOP) 
		{
			int loop_count = 1;
			double loop_length = (double)x->x_m5LoopLength;
			if (x->x_m5LoopLength == LOOP_SELF) 
			{
				loop_length =  (double)x->x_m5SoundFileFramesAvailableFromOnset;
			}
			if (loop_length <= 0.) 
			{
				// end time cannot be before start time
				x->x_m5PlayEndTime = x->x_m5PlayStartTime;
			} else {
				// some loops have already played, so find start time of current one
				if (x->x_m5PlayStartTime <= blockStartTime) {
					loop_count = floor((blockStartTime - x->x_m5PlayStartTime) / loop_length) + 1;
				}
				x->x_m5PlayEndTime = x->x_m5PlayStartTime + loop_length * loop_count;
			}
		}
		
		x->x_state = STATE_STREAM;
		
		if (blockStartTime + vecsize >(size_t) x->x_m5PlayEndTime)
		{
			// the current block passes by the requested end time
			// finish the partial buffer and set the rest to silence

			size_t xfersize;
		
			if (blockStartTime >= (size_t)x->x_m5PlayEndTime)
			{
				xfersize = 0;
			} else {
				xfersize =(size_t) x->x_m5PlayEndTime - blockStartTime;
				if (xfersize > (size_t)vecsize) xfersize = vecsize;
			}
			
			if (xfersize)
			{
				m5_soundfile_xferin_sample(&sf, noutlets, x->x_outvec, 0,
					(unsigned char *)(x->x_buf + x->x_fifotail), xfersize);
				vecsize -= xfersize;
			}
			
			x->x_state = STATE_IDLE;
			x->x_requestcode = REQUEST_CLOSE;
			clock_delay(x->x_clock, 0);	
			/* send bang and zero out the (rest of the) output */
			pthread_mutex_unlock(&x->x_mutex);
				
					
			for (i = 0; i < noutlets; i++)
				for (j = vecsize, fp = x->x_outvec[i] + xfersize; j--;)
					*fp++ = 0;
			return w + 2;
		}
		else if (blockStartTime < (size_t)x->x_m5PlayStartTime) 
		{
			// start time may occur within this block (or later). 
			// fill with partial silence in the meantime before the start time
			// we keep updating the tail even though we are not reading (all) the data, to keep things in sync
			size_t zerosize;
			zerosize = (size_t)x->x_m5PlayStartTime - blockStartTime;
			
			if (zerosize > (size_t)vecsize) {
				zerosize = vecsize;
			}
			pthread_mutex_unlock(&x->x_mutex);
			for (i = 0; i < noutlets; i++)
				for (j = zerosize, fp = x->x_outvec[i]; j--;)
					*fp++ = 0;
			pthread_mutex_lock(&x->x_mutex);
			/* resync local variables */
			vecsize = x->x_vecsize;
			m5_soundfile_copy(&sf, &x->x_sf);
			
			
			int xfersize = vecsize - zerosize;
			
			if (xfersize)
			{
				m5_soundfile_xferin_sample(&sf, noutlets, x->x_outvec, zerosize,
				(unsigned char *)(x->x_buf + x->x_fifotail), xfersize);
			}
			x->x_fifotail += vecsize * sf.sf_bytesperframe;

			x->x_m5TailTime += vecsize;
		} else {
			// Regular playback, stream entire buffer.
			// Note if audio loop extends past end of actual soundfile, the
			// child process handles inserting silence into the buffer
			m5_soundfile_xferin_sample(&sf, noutlets, x->x_outvec, 0,
				(unsigned char *)(x->x_buf + x->x_fifotail), vecsize);
			
			x->x_fifotail += vecsize * sf.sf_bytesperframe;
			x->x_m5TailTime += vecsize;
		}


		
		if (x->x_fifotail >= x->x_fifosize) {
			x->x_fifotail = 0;			
		}
			
		if ((--x->x_sigcountdown) <= 0)
		{
			sfread_cond_signal(&x->x_requestcondition);
			x->x_sigcountdown = x->x_sigperiod;
		}
		pthread_mutex_unlock(&x->x_mutex);
	}
	else
	{
		if (x->x_state == STATE_STARTUP) {
			pthread_mutex_lock(&x->x_mutex);
			// get file length and send it to the outlet once if ready
			if (x->x_m5SoundFileFramesAvailableFromOnset == 0) {
				if (x->x_sf.sf_bytesperframe > 0 && x->x_sf.sf_bytelimit != SFMAXBYTES) {
					x->x_m5SoundFileFramesAvailableFromOnset = x->x_sf.sf_bytelimit / x->x_sf.sf_bytesperframe;
				}
				if (x->x_m5SoundFileFramesAvailableFromOnset > 0) {
					x->x_state = STATE_STARTUP_2;
					clock_delay(x->x_m5FramesOutClock, 0);
				}
#ifdef DEBUG_SOUNDFILE_THREADS				
				fprintf(stderr, "readsf~ perform: sf_bytelimit, x_m5SoundFileFramesAvailableFromOnset, sf_bytesperframe %ld, %ld, %d\n", x->x_sf.sf_bytelimit, x->x_m5SoundFileFramesAvailableFromOnset, x->x_sf.sf_bytesperframe);
#endif				
			}
			
			if (x->x_fileerror) {
				m5_object_sferror(x, "[readsf~]", x->x_filename,x->x_fileerror, &x->x_sf);
				x->x_state = STATE_IDLE;
				clock_delay(x->x_m5FramesOutClock, 0);
				clock_delay(x->x_clock, 0);	
			}
			
			pthread_mutex_unlock(&x->x_mutex);
		}

		for (i = 0; i < noutlets; i++)
			for (j = vecsize, fp = x->x_outvec[i]; j--;)
				*fp++ = 0;
	}
	return w + 2;
}

	/** start making output.  If we're in the "startup" state change
		to the "running" state. */
		
// play from beginning immediately
static void m5_readsf_start(t_readsf *x, t_symbol *s, int argc, t_atom *argv)
{
	t_m5FrameTimeCode ftc;
	if (x->x_state != STATE_STARTUP && x->x_state!= STATE_STARTUP_2) 
	{
		pd_error(x, "[readsf~]: start requested with no prior 'open'");
		return;
	}
	// no args - start now or as soon as file opened
	if (argc == 0) 
	{
		pthread_mutex_lock(&x->x_mutex);
		x->x_m5LoopLengthRequest = 1;
		x->x_state = STATE_STREAM;
		x->x_m5PlayStartTime = START_NOW;
		
		// get a new t=0 reference time for case when a shared FTC anchor is not used
		x->x_m5LocalTimeAnchor = clock_getlogicaltime();
		
		sfread_cond_signal(&x->x_requestcondition);
		pthread_mutex_unlock(&x->x_mutex);
		return;
	}
	// read FTC from parameters and make that the start time
	if (m5_frame_time_code_from_atoms(argc, argv, &ftc)) {
		pd_error (x,"m5_readsf~: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	}
	long ll = m5_frames_from_time_code(&ftc);
	if (ll < 0) {
		pd_error (x,"m5_readsf~: start time must be >= 0 frames.");
		return;
	}
	pthread_mutex_lock(&x->x_mutex);
	x->x_m5LoopLengthRequest = 1;
	x->x_state = STATE_STREAM;
	x->x_m5PlayStartTime = (double)ll;
	
	x->x_m5LocalTimeAnchor = clock_getlogicaltime();
	
	sfread_cond_signal(&x->x_requestcondition);
	pthread_mutex_unlock(&x->x_mutex);
}


static void m5_readsf_stop(t_readsf *x, t_symbol *s, int argc, t_atom *argv)
{
	t_m5FrameTimeCode ftc;
	if (x->x_state != STATE_STREAM && x->x_state != STATE_STARTUP && x->x_state!= STATE_STARTUP_2) 
	{
		pd_error(x, "[readsf~]: stop requested with no prior 'open'");
		return;
	}
	
	// stop on next block, as usual for readsf
	if (argc == 0) {
		pthread_mutex_lock(&x->x_mutex);
		x->x_state = STATE_IDLE;
		x->x_requestcode = REQUEST_CLOSE;
		sfread_cond_signal(&x->x_requestcondition);
		pthread_mutex_unlock(&x->x_mutex);
		return;
	}
	
	// stop asap
	if (atom_getsymbolarg(0, argc, argv) == gensym("now")) {
		pthread_mutex_lock(&x->x_mutex);
		x->x_m5PlayEndTime = END_NOW;
		sfread_cond_signal(&x->x_requestcondition);
		pthread_mutex_unlock(&x->x_mutex);
		return;
	// stop at 'end' of current audio loop	
	} else if (atom_getsymbolarg(0, argc, argv) == gensym("end")) {
		pthread_mutex_lock(&x->x_mutex);
		x->x_m5PlayEndTime = END_AT_LOOP;
		sfread_cond_signal(&x->x_requestcondition);
		pthread_mutex_unlock(&x->x_mutex);
		return;
	// keep looping forever (can follow-up with a another 'stop' later to actually stop)
	} else if (atom_getsymbolarg(0, argc, argv) == gensym("never")) {
		pthread_mutex_lock(&x->x_mutex);
		x->x_m5PlayEndTime = END_NEVER;
		sfread_cond_signal(&x->x_requestcondition);
		pthread_mutex_unlock(&x->x_mutex);
		return;
	}
	// stop at specific FTC
	if (m5_frame_time_code_from_atoms(argc, argv, &ftc)) {
		pd_error (x,"m5_readsf~: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	}
	long ll = m5_frames_from_time_code(&ftc);
	if (ll < 0) {
		pd_error (x,"m5_readsf~: end time must be >= 0 frames.");
		return;
	}
	pthread_mutex_lock(&x->x_mutex);
	x->x_m5PlayEndTime = (double)ll;
	sfread_cond_signal(&x->x_requestcondition);
	pthread_mutex_unlock(&x->x_mutex);

}

// set loop start file offset (FTC) (i.e. # of frames into sound file after static 'onset' to loop from)
static void m5_readsf_loop_start (t_readsf *x, t_symbol *s, int argc, t_atom *argv) 
{
	t_m5FrameTimeCode ftc;
	if (m5_frame_time_code_from_atoms(argc, argv, &ftc)) {
		pd_error (x,"m5_readsf~: A frame time code must be three floats... 1|-1, epoch, frames.");
		pd_error (x,"m5_readsf~: Use loopstart 1 0 0 to start at 0 frames.");
		return;
	}
	
	long ll = m5_frames_from_time_code(&ftc);
	if (ll < 0) {
		pd_error (x,"m5_readsf~: Loop start must be >= 0 frames.");
		return;
	}
	pthread_mutex_lock(&x->x_mutex);
	x->x_m5LoopLengthRequest = 1;
	x->x_m5LoopStart = ll;	
	sfread_cond_signal(&x->x_requestcondition);
	pthread_mutex_unlock(&x->x_mutex);
	
	
}

// set loop length (FTC)
static void m5_readsf_loop_length(t_readsf *x, t_symbol *s, int argc, t_atom *argv)
{
	t_m5FrameTimeCode ftc;
	if (atom_getsymbolarg(0, argc, argv) == gensym("self")) {
		pthread_mutex_lock(&x->x_mutex);
		x->x_m5LoopLengthRequest = 1;
		x->x_m5LoopLength = LOOP_SELF;
		sfread_cond_signal(&x->x_requestcondition);
		pthread_mutex_unlock(&x->x_mutex);
		return;
	}
	if (m5_frame_time_code_from_atoms(argc, argv, &ftc)) {
		pd_error (x,"m5_readsf~: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	}
	long ll = m5_frames_from_time_code(&ftc);
	if (ll <= 0) {
		pd_error (x,"m5_readsf~: Loop length must be > 0 frames.");
		return;
	}
	pthread_mutex_lock(&x->x_mutex);
	x->x_m5LoopLengthRequest = 1;
	x->x_m5LoopLength = ll;	
	sfread_cond_signal(&x->x_requestcondition);
	pthread_mutex_unlock(&x->x_mutex);
}

// set ID for FTC anchor (shared time reference for t=0)
static void m5_readsf_time_set(t_readsf *x, t_symbol *s)
{
	t_m5TimeAnchor *a;
	
	x->x_m5TimeAnchorName = s;
	if (!s) {
		x->x_m5TimeAnchor = 0;		
		return;
	}
	if (s == gensym("self")) 
	{
		x->x_m5TimeAnchor = 0;		
		return;
	}
	if (!(a = m5_time_anchor_find(s)))
	{
		if (*s->s_name) pd_error(x, "m5_readsf~: %s: no such time anchor",
			x->x_m5TimeAnchorName->s_name);
		x->x_m5TimeAnchor = 0;
	}
	else m5_time_anchor_usedindsp(a);
	
	x->x_m5TimeAnchor = a;
}

static void m5_readsf_time(t_readsf *x, t_symbol *name) 
{
	m5_readsf_time_set(x, name);
	x->x_m5LoopLengthRequest = 1;	
}

// legacy - 1 = start, 0 = stop
static void m5_readsf_float(t_readsf *x, t_floatarg f)
{
	if (f != 0)
		m5_readsf_start(x, 0, 0, 0);
	else m5_readsf_stop(x, 0, 0, 0);
}

static int m5_readsf_one_iter(const char *path, t_readsf *x)
{
	x->x_namelist = namelist_append(x->x_namelist, path, 0);
	return 1;
}

	/** open method.  Called as:
		open [flags] filename [onsetframes headersize channels bytes endianness]
		(if headersize is zero, header is taken to be automatically detected;
		thus, use the special "-1" to mean a truly headerless file.)
		if type implementation is set, pass this to open unless headersize is -1 */
static void m5_readsf_open(t_readsf *x, t_symbol *s, int argc, t_atom *argv)
{
	t_symbol *filesym, *endian;
	t_float onsetframes, headersize, nchannels, bytespersample;
	t_soundfile_type *type = NULL;

	while (argc > 0 && argv->a_type == A_SYMBOL &&
		*argv->a_w.w_symbol->s_name == '-')
	{
			/* check for type by name */
		const char *flag = argv->a_w.w_symbol->s_name + 1;
		if (!(type = m5_soundfile_findtype(flag)))
			goto usage; /* unknown flag */
		argc -= 1; argv += 1;
	}
	filesym = atom_getsymbolarg(0, argc, argv);
	onsetframes = atom_getfloatarg(1, argc, argv);
	headersize = atom_getfloatarg(2, argc, argv);
	nchannels = atom_getfloatarg(3, argc, argv);
	bytespersample = atom_getfloatarg(4, argc, argv);
	endian = atom_getsymbolarg(5, argc, argv);
	if (!*filesym->s_name)
		return; /* no filename */

	pthread_mutex_lock(&x->x_mutex);
	if (x->x_namelist)
		namelist_free(x->x_namelist), x->x_namelist = 0;
		/* see open_soundfile_via_namelist() */
	if (!sys_isabsolutepath(filesym->s_name))
		canvas_path_iterate(x->x_canvas,
			(t_canvas_path_iterator)m5_readsf_one_iter, x);
	if (sys_verbose)    /* do a fake open just for the verbose printout */
	{
		char buf[MAXPDSTRING], *dummy;
		int fd;
		fd = open_via_path(canvas_getdir(x->x_canvas)->s_name, filesym->s_name, "", buf, &dummy, MAXPDSTRING, 1);
		// fd = do_open_via_path(canvas_getdir(x->x_canvas)->s_name,
			// filesym->s_name, "", buf, &dummy, MAXPDSTRING, 1, x->x_namelist, 1);
		if (fd >= 0)
			close(fd);
	}
	m5_soundfile_clear(&x->x_sf);
	x->x_requestcode = REQUEST_OPEN;
	x->x_filename = filesym->s_name;
	x->x_fifotail = 0;
	x->x_fifohead = 0;
	// x->x_m5FramesPlayed = 0;
	if (*endian->s_name == 'b')
		 x->x_sf.sf_bigendian = 1;
	else if (*endian->s_name == 'l')
		 x->x_sf.sf_bigendian = 0;
	else if (*endian->s_name)
		pd_error(x, "[readsf~] open: endianness neither 'b' nor 'l'");
	else x->x_sf.sf_bigendian = m5_sys_isbigendian();
	x->x_onsetframes = (onsetframes > 0 ? onsetframes : 0);
	x->x_sf.sf_headersize = (headersize > 0 ? headersize :
		(headersize == 0 ? -1 : 0));
	x->x_sf.sf_nchannels = (nchannels >= 1 ? nchannels : 1);
	x->x_sf.sf_bytespersample = (bytespersample > 2 ? bytespersample : 2);
	x->x_sf.sf_bytesperframe = x->x_sf.sf_nchannels * x->x_sf.sf_bytespersample;
	x->x_sf.sf_bytelimit = 0;
	if (type && x->x_sf.sf_headersize >= 0)
	{
		post("'-%s' overridden by headersize", type->t_name);
		x->x_sf.sf_type = NULL;
	}
	else
		x->x_sf.sf_type = type;
	x->x_eof = 0;
	x->x_m5SoundFileFramesAvailableFromOnset = 0;
	x->x_fileerror = 0;
	x->x_m5HeadTimeRequest = x->x_m5TailTime = 0;
	x->x_m5PlayStartTime = START_NOW;
	x->x_m5PlayEndTime = END_AT_LOOP;
	x->x_state = STATE_STARTUP;
	
	sfread_cond_signal(&x->x_requestcondition);
	pthread_mutex_unlock(&x->x_mutex);
	return;
usage:
	pd_error(x, "[readsf~]: usage; open [flags] filename [onset] [headersize]...");
	pd_error(0, "[nchannels] [bytespersample] [endian (b or l)]");
	post("flags: %s",m5_sf_typeargs);
}

static void m5_readsf_dsp(t_readsf *x, t_signal **sp)
{
	m5_readsf_time_set(x, x->x_m5TimeAnchorName);
	int i, noutlets = x->x_noutlets;
	pthread_mutex_lock(&x->x_mutex);
	x->x_vecsize = sp[0]->s_n;
	x->x_sigperiod = x->x_fifosize / (x->x_sf.sf_bytesperframe * x->x_vecsize);
	for (i = 0; i < noutlets; i++)
		x->x_outvec[i] = sp[i]->s_vec;
	pthread_mutex_unlock(&x->x_mutex);
	dsp_add(m5_readsf_perform, 1, x);	
}

static void m5_readsf_print(t_readsf *x)
{
	post("state %d", x->x_state);
	post("fifo head %d", x->x_fifohead);
	post("fifo tail %d", x->x_fifotail);
	post("fifo size %d", x->x_fifosize);
	post("fd %d", x->x_sf.sf_fd);
	post("eof %d", x->x_eof);
	post("total frames %d", x->x_m5SoundFileFramesAvailableFromOnset);
	
}

	/** request QUIT and wait for acknowledge */
static void m5_readsf_free(t_readsf *x)
{
	void *threadrtn;
	pthread_mutex_lock(&x->x_mutex);
	x->x_requestcode = REQUEST_QUIT;
	sfread_cond_signal(&x->x_requestcondition);
	while (x->x_requestcode != REQUEST_NOTHING)
	{
		sfread_cond_signal(&x->x_requestcondition);
		sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
	}
	pthread_mutex_unlock(&x->x_mutex);
	if (pthread_join(x->x_childthread, &threadrtn))
		pd_error(x, "[readsf~] free: join failed");

	pthread_cond_destroy(&x->x_requestcondition);
	pthread_cond_destroy(&x->x_answercondition);
	pthread_mutex_destroy(&x->x_mutex);
	freebytes(x->x_buf, x->x_bufsize);
	clock_free(x->x_clock);
	clock_free(x->x_m5FramesOutClock);
}

static void m5_readsf_setup(void)
{
	m5_readsf_class = class_new(gensym("m5_readsf~"),
		(t_newmethod)m5_readsf_new, (t_method)m5_readsf_free,
		sizeof(t_readsf), 0, A_DEFFLOAT, A_DEFFLOAT, 0);
	class_addfloat(m5_readsf_class, (t_method)m5_readsf_float);
	class_addmethod(m5_readsf_class, (t_method)m5_readsf_start, gensym("start"), A_GIMME, 0);
	// class_addmethod(m5_readsf_class, (t_method)m5_readsf_start_arm, gensym("start_arm"), 0);
	// class_addmethod(m5_readsf_class, (t_method)m5_readsf_start_sync_now, gensym("start_sync_now"), 0);
	class_addmethod(m5_readsf_class, (t_method)m5_readsf_stop, gensym("stop"), A_GIMME, 0);
	class_addmethod(m5_readsf_class, (t_method)m5_readsf_dsp,
		gensym("dsp"), A_CANT, 0);
	class_addmethod(m5_readsf_class, (t_method)m5_readsf_open,
		gensym("open"), A_GIMME, 0);
	class_addmethod(m5_readsf_class, (t_method)m5_readsf_print, gensym("print"), 0);
	
	// class_addmethod(m5_readsf_class, (t_method)m5_readsf_loop_off, gensym("loopoff"), 0);
	// class_addmethod(m5_readsf_class, (t_method)m5_readsf_loop_on, gensym("loopon"), 0);
	class_addmethod(m5_readsf_class, (t_method)m5_readsf_time, gensym("time"), A_SYMBOL, 0);
	class_addmethod(m5_readsf_class, (t_method)m5_readsf_loop_length, gensym("looplength"), A_GIMME, 0);
	class_addmethod(m5_readsf_class, (t_method)m5_readsf_loop_start, gensym("loopstart"), A_GIMME, 0);
		
}

/* ------------------------- writesf ------------------------- */

static t_class *m5_writesf_class;

typedef t_readsf t_writesf; /* just re-use the structure */

/* ----- the child thread which performs file I/O ----- */

static void *m5_writesf_child_main(void *zz)
{
	t_writesf *x = zz;
	t_soundfile sf = {0};
	m5_soundfile_clear(&sf);
#ifdef PDINSTANCE
	pd_this = x->x_pd_this;
#endif
#ifdef DEBUG_SOUNDFILE_THREADS
	fprintf(stderr, "writesf~: 1\n");
#endif
	pthread_mutex_lock(&x->x_mutex);
	while (1)
	{
#ifdef DEBUG_SOUNDFILE_THREADS
		fprintf(stderr, "writesf~: 0\n");
#endif
		if (x->x_requestcode == REQUEST_NOTHING)
		{
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "writesf~: wait 2\n");
#endif
			sfread_cond_signal(&x->x_answercondition);
			sfread_cond_wait(&x->x_requestcondition, &x->x_mutex);
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "writesf~: 3\n");
#endif
		}
		else if (x->x_requestcode == REQUEST_OPEN)
		{
			ssize_t byteswritten;
			size_t writebytes;

				/* copy file stuff out of the data structure so we can
				relinquish the mutex while we're in open_soundfile_via_path() */
			const char *filename = x->x_filename;
			t_canvas *canvas = x->x_canvas;
			m5_soundfile_copy(&sf, &x->x_sf);

				/* alter the request code so that an ensuing "open" will get
				noticed. */
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "writesf~: 4\n");
#endif
			x->x_requestcode = REQUEST_BUSY;
			x->x_fileerror = 0;

				/* if there's already a file open, close it.  This
				should never happen since writesf_open() calls stop if
				needed and then waits until we're idle. */
			if (sf.sf_fd >= 0)
			{
				size_t frameswritten = x->x_frameswritten;

				pthread_mutex_unlock(&x->x_mutex);
				m5_soundfile_finishwrite(x, filename, &sf,
					SFMAXFRAMES, frameswritten);
				sys_close(sf.sf_fd);
				sf.sf_fd = -1;
				pthread_mutex_lock(&x->x_mutex);
				x->x_sf.sf_fd = -1;
#ifdef DEBUG_SOUNDFILE_THREADS
				fprintf(stderr, "writesf~: bug? ditched %ld\n", frameswritten);
#endif
				if (x->x_requestcode != REQUEST_BUSY)
					continue;
			}
				/* cache sf *after* closing as x->sf's type
					may have changed in writesf_open() */
			m5_soundfile_copy(&sf, &x->x_sf);

				/* open the soundfile with the mutex unlocked */
			pthread_mutex_unlock(&x->x_mutex);
			m5_create_soundfile(canvas, filename, &sf, 0);
			pthread_mutex_lock(&x->x_mutex);

#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "writesf~: 5\n");
#endif

			if (sf.sf_fd < 0)
			{
				x->x_sf.sf_fd = -1;
				x->x_eof = 1;
				x->x_fileerror = errno;
#ifdef DEBUG_SOUNDFILE_THREADS
				fprintf(stderr, "writesf~: open failed %s\n", filename);
#endif
				goto bail;
			}
				/* check if another request has been made; if so, field it */
			if (x->x_requestcode != REQUEST_BUSY)
				continue;
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "writesf~: 6\n");
#endif
				/* copy back into the instance structure. */
			m5_soundfile_copy(&x->x_sf, &sf);
			x->x_fifotail = 0;
			x->x_frameswritten = 0;
				/* in a loop, wait for the fifo to have data and write it
					to disk */
			while (x->x_requestcode == REQUEST_BUSY ||
				(x->x_requestcode == REQUEST_CLOSE &&
					x->x_fifohead != x->x_fifotail))
			{
				int fifosize = x->x_fifosize, fifotail;
				char *buf = x->x_buf;
#ifdef DEBUG_SOUNDFILE_THREADS
				fprintf(stderr, "writesf~: 77\n");
#endif
					/* if the head is < the tail, we can immediately write
					from tail to end of fifo to disk; otherwise we hold off
					writing until there are at least WRITESIZE bytes in the
					buffer */
				if (x->x_fifohead < x->x_fifotail ||
					x->x_fifohead >= x->x_fifotail + WRITESIZE
					|| (x->x_requestcode == REQUEST_CLOSE &&
						x->x_fifohead != x->x_fifotail))
				{
					writebytes = (x->x_fifohead < x->x_fifotail ?
						fifosize : x->x_fifohead) - x->x_fifotail;
					if (writebytes > READSIZE)
						writebytes = READSIZE;
				}
				else
				{
#ifdef DEBUG_SOUNDFILE_THREADS
					fprintf(stderr, "writesf~: wait 7a...\n");
#endif
					sfread_cond_signal(&x->x_answercondition);
#ifdef DEBUG_SOUNDFILE_THREADS
					fprintf(stderr, "writesf~: signaled...\n");
#endif
					sfread_cond_wait(&x->x_requestcondition,
						&x->x_mutex);
#ifdef DEBUG_SOUNDFILE_THREADS
					fprintf(stderr, "writesf~: 7a ... done\n");
#endif
					continue;
				}
#ifdef DEBUG_SOUNDFILE_THREADS
				fprintf(stderr, "writesf~: 8\n");
#endif
				fifotail = x->x_fifotail;
				m5_soundfile_copy(&sf, &x->x_sf);
				pthread_mutex_unlock(&x->x_mutex);
				byteswritten = write(sf.sf_fd, buf + fifotail, writebytes);
				pthread_mutex_lock(&x->x_mutex);
				if (x->x_requestcode != REQUEST_BUSY &&
					x->x_requestcode != REQUEST_CLOSE)
						break;
				if (byteswritten < 0 || (size_t)byteswritten < writebytes)
				{
#ifdef DEBUG_SOUNDFILE_THREADS
					fprintf(stderr, "writesf~: fileerror %d\n", errno);
#endif
					x->x_fileerror = errno;
					goto bail;
				}
				else
				{
					if (fifotail != x->x_fifotail)
					{
						//something changed 
						//we shouldn't be reading from the buffer unless fifohead is ahead of fifotail, and the
						//main thread only alters fifotail once per file, when they are equal, so this shouldn't happen
						goto bail;
					}
					x->x_fifotail += byteswritten;
					if (x->x_fifotail == fifosize)
						x->x_fifotail = 0;
				}
				x->x_frameswritten += byteswritten / sf.sf_bytesperframe;
#ifdef DEBUG_SOUNDFILE_THREADS
				fprintf(stderr, "writesf~: after head %d tail %d written %ld\n",
					x->x_fifohead, x->x_fifotail, x->x_frameswritten);
#endif
					/* signal parent in case it's waiting for data */
				sfread_cond_signal(&x->x_answercondition);
				continue;

			 bail:
				 if (x->x_requestcode == REQUEST_BUSY)
					 x->x_requestcode = REQUEST_NOTHING;
					 /* hit an error; close file if necessary,
					 set EOF and signal once more */
				 if (sf.sf_fd >= 0)
				 {
					 pthread_mutex_unlock(&x->x_mutex);
					 sys_close(sf.sf_fd);
					 sf.sf_fd = -1;
					 pthread_mutex_lock(&x->x_mutex);
					 x->x_eof = 1;
					 x->x_sf.sf_fd = -1;
				 }
				 sfread_cond_signal(&x->x_answercondition);
			}
		}
		else if (x->x_requestcode == REQUEST_CLOSE ||
			x->x_requestcode == REQUEST_QUIT)
		{
			int quit = (x->x_requestcode == REQUEST_QUIT);
			if (sf.sf_fd >= 0)
			{
				const char *filename = x->x_filename;
				size_t frameswritten = x->x_frameswritten;
				m5_soundfile_copy(&sf, &x->x_sf);
				pthread_mutex_unlock(&x->x_mutex);
				m5_soundfile_finishwrite(x, filename, &sf,
					SFMAXFRAMES, frameswritten);
				sys_close(sf.sf_fd);
				sf.sf_fd = -1;
				pthread_mutex_lock(&x->x_mutex);
				x->x_sf.sf_fd = -1;
			}
			x->x_requestcode = REQUEST_NOTHING;
			x->x_m5FramesWrittenReport = x->x_frameswritten;
			sfread_cond_signal(&x->x_answercondition);
			if (quit)
				break;
		}
		else
		{
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "writesf~: 13\n");
#endif
		}
	}
#ifdef DEBUG_SOUNDFILE_THREADS
	fprintf(stderr, "writesf~: thread exit\n");
#endif
	pthread_mutex_unlock(&x->x_mutex);
	return 0;
}

/* ----- the object proper runs in the calling (parent) thread ----- */

static void m5_writesf_frame_out_tick(t_writesf *x)
{
	
	t_m5FrameTimeCode ftc;
	m5_frame_time_code_from_frames(x->x_m5SoundFileFramesAvailableFromOnset, &ftc);	
	m5_frame_time_code_out(&ftc, x->x_m5listOut);
}

static void m5_writesf_start_time_tick(t_writesf *x)
{
	t_m5FrameTimeCode ftc;
	m5_frame_time_code_from_frames(x->x_m5WriteStartTimeReport, &ftc);
	m5_frame_time_code_out(&ftc, x->x_m5startListOut);
}

static void *m5_writesf_new(t_floatarg fnchannels, t_floatarg fbufsize)
{
	t_writesf *x;
	int nchannels = fnchannels, bufsize = fbufsize, i;
	char *buf;

	if (nchannels < 1)
		nchannels = 1;
	else if (nchannels > MAXSFCHANS)
		nchannels = MAXSFCHANS;
	if (bufsize <= 0) bufsize = DEFBUFPERCHAN * nchannels;
	else if (bufsize < MINBUFSIZE)
		bufsize = MINBUFSIZE;
	else if (bufsize > MAXBUFSIZE)
		bufsize = MAXBUFSIZE;
	buf = getbytes(bufsize);
	if (!buf) return 0;

	x = (t_writesf *)pd_new(m5_writesf_class);

	for (i = 1; i < nchannels; i++)
		inlet_new(&x->x_obj,  &x->x_obj.ob_pd, &s_signal, &s_signal);

	x->x_f = 0;
	pthread_mutex_init(&x->x_mutex, 0);
	pthread_cond_init(&x->x_requestcondition, 0);
	pthread_cond_init(&x->x_answercondition, 0);
	x->x_vecsize = MAXVECSIZE;
	x->x_insamplerate = 0;
	x->x_state = STATE_IDLE;
	x->x_canvas = canvas_getcurrent();
	m5_soundfile_clear(&x->x_sf);
	x->x_sf.sf_nchannels = nchannels;
	x->x_sf.sf_bytespersample = 2;
	x->x_sf.sf_bytesperframe = nchannels * 2;
	x->x_buf = buf;
	x->x_bufsize = bufsize;
	x->x_fifosize = x->x_fifohead = x->x_fifotail = x->x_requestcode = 0;
	
	x->x_m5SoundFileFramesAvailableFromOnset = 0;
	x->x_m5FramesWrittenReport = FRAMES_NOT_UPDATED;
	x->x_m5LoopLength = 0;
	x->x_m5LoopLengthRequest = 0;
	
	x->x_m5HeadTimeRequest = 0;
	x->x_m5TailTime = 0;
	x->x_m5TimeAnchorName = 0;
	x->x_m5TimeAnchor = 0;
	x->x_m5LocalTimeAnchor = 0;
	
	x->x_m5PlayStartTime = START_NOW;
	x->x_m5PlayEndTime = END_NEVER;
	x->x_m5PlayStartThreshold = 0.5;
	
	x->x_m5FramesOutClock = clock_new(x, (t_method)m5_writesf_frame_out_tick);
	x->x_m5StartTimeOutClock = clock_new(x, (t_method)m5_writesf_start_time_tick);
	
	x->x_m5startListOut = outlet_new(&x->x_obj, &s_anything);
	x->x_m5listOut = outlet_new(&x->x_obj, &s_anything);

#ifdef PDINSTANCE
	x->x_pd_this = pd_this;
#endif
	pthread_create(&x->x_childthread, 0, m5_writesf_child_main, x);
	return x;
}

static t_int *m5_writesf_perform(t_int *w)
{
	t_writesf *x = (t_writesf *)(w[1]);
	if (x->x_state == STATE_STREAM || x->x_state == STATE_STREAM_JUST_STARTING)
	{
		size_t roominfifo;
		size_t wantbytes;
		int vecsize = x->x_vecsize;
		
		// tail push is used to copy incoming bytes to FIFO , but prevent
		// child process from writing them, in case we want to retroactively
		// start recording 'in the past'
		int tailpush = 0;

		t_soundfile sf = {0};
		pthread_mutex_lock(&x->x_mutex);
			/* copy with mutex locked! */
		m5_soundfile_copy(&sf, &x->x_sf);
		
		size_t blockStartTime = 0; // frame count since time anchor
		if (x->x_m5TimeAnchor) 
		{
			// shared time anchor
			blockStartTime = m5_time_anchor_get_time_since_start(x->x_m5TimeAnchor);
		} 
		else 
		{
			// local clock for this object
			double d =  ceil(clock_gettimesincewithunits(x->x_m5LocalTimeAnchor, 1, 1));
			if (d < 0.){d = 0.;}
			blockStartTime = (size_t)d;
		}
		if (x->x_m5PlayStartTime == START_NOW)  
		{
			x->x_m5PlayStartTime = blockStartTime;
			x->x_m5WriteStartTimeReport = blockStartTime;
		}
		
		if (x->x_m5PlayStartTime == START_AT_THRESHOLD)
		{
			// do threshold detect if necessary
			t_sample the_threshold = x->x_m5PlayStartThreshold;
			
			pthread_mutex_unlock(&x->x_mutex);
			int started = NOT_FOUND;
			started = m5_find_threshold(sf.sf_nchannels, vecsize,  x->x_outvec, the_threshold);
			pthread_mutex_lock(&x->x_mutex);
			
			m5_soundfile_copy(&sf, &x->x_sf);			
			if (started != NOT_FOUND) 
			{
				// get the start time, can subtract some extra frames here if needed to record the threshold onset
				x->x_m5PlayStartTime = blockStartTime + started - 0;
			}
		}
		
		
		char is_finished = 0;
		int vecstart = 0;
		int overdue = 0;
		if (blockStartTime + (size_t)vecsize > (size_t)x->x_m5PlayEndTime)
		{
			is_finished = 1;
			ssize_t xfersize = (ssize_t)x->x_m5PlayEndTime - (ssize_t)blockStartTime;
			if (xfersize > 0) {
				vecsize = xfersize;
			} else {
				vecsize = 0;
			}
			
		} 
		// note: always true if x_m5PlayStartTime = START_AT_THRESHOLD
		else if (blockStartTime <= (size_t)x->x_m5PlayStartTime)
		{
			if (blockStartTime + (size_t)vecsize > x->x_m5PlayStartTime)
			{	
				// partial vector, scheduled to start recording during this block			
				vecstart = (size_t)x->x_m5PlayStartTime - blockStartTime;
				// realign the tail and head so that the head ends up one full vecsize from the beginning
				x->x_fifotail = x->x_fifohead = vecstart * sf.sf_bytesperframe;
				vecsize -= vecstart;
				
				x->x_m5WriteStartTimeReport = x->x_m5PlayStartTime;
				
			} else {
				// not starting yet, but use tailpush to allow us to
				// save incoming samples to the buffer without writing to disk
				tailpush = vecsize;
				
			}
		} else if (x->x_state == STATE_STREAM_JUST_STARTING && (overdue = blockStartTime - (size_t)x->x_m5PlayStartTime) > 0) 
		{
			// write start time is in the past but we haven't started recording yet
			
			// get how many bytes before now that we need to actually keep
			int overdueBytes = overdue * sf.sf_bytesperframe;
			
			// can't go back further than the buffer can store
			if (overdueBytes >= x->x_fifosize) {
				// one frame less than fifosize so head and tail don't exactly line up again (which would prevent child process from writing)
				overdueBytes = x->x_fifosize - (1 * sf.sf_bytesperframe); 				
			}
			
			// can't go back further than we've actually received
			// Note- x_m5PerformedFifoSize will never actually be larger than x_fifosize anyway, but
			// we do the extra check above since we want leave a 1-frame gap in that case
			if (overdueBytes > x->x_m5PerformedFifoSize) {
				overdueBytes = x->x_m5PerformedFifoSize;
			}
			
			// move the tail back to a position away from head,
			// child process will start moving bytes from FIFO to disk
			x->x_fifotail -= overdueBytes;
			if (x->x_fifotail < 0)
			{
				x->x_fifotail = x->x_fifosize + x->x_fifotail;
			}
			int actualFrames = overdueBytes /  sf.sf_bytesperframe;
			int difff = overdue - actualFrames;
			// will output the time we actually started saving frames
			x->x_m5WriteStartTimeReport = x->x_m5PlayStartTime + difff;
			
		}
		wantbytes = vecsize * sf.sf_bytesperframe;
		roominfifo = x->x_fifotail - x->x_fifohead;
		if (roominfifo <= 0)
			roominfifo += x->x_fifosize;
		while (!x->x_eof && roominfifo < wantbytes + 1)
		{
			fprintf(stderr, "writesf waiting for disk write..\n");
			fprintf(stderr, "(head %d, tail %d, room %d, want %ld)\n",
				(int)x->x_fifohead, (int)x->x_fifotail,
				(int)roominfifo, (long)wantbytes);
			sfread_cond_signal(&x->x_requestcondition);
			sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
			fprintf(stderr, "... done waiting.\n");
			roominfifo = x->x_fifotail - x->x_fifohead;
			if (roominfifo <= 0)
				roominfifo += x->x_fifosize;
		}
		if (x->x_eof)
		{
			if (x->x_fileerror)
				m5_object_sferror(x, "[writesf~]", x->x_filename,
					x->x_fileerror, &x->x_sf);
			x->x_state = STATE_IDLE;
			sfread_cond_signal(&x->x_requestcondition);
			pthread_mutex_unlock(&x->x_mutex);
			return w + 2;
		}

		
		m5_soundfile_xferout_sample(&sf, x->x_outvec,
			(unsigned char *)(x->x_buf + x->x_fifohead), vecsize, 0, 1.);
		
		// there are bytes in fifo that actually came from the inlet	
		x->x_m5PerformedFifoSize += wantbytes;
		if (x->x_m5PerformedFifoSize > x->x_fifosize)
			x->x_m5PerformedFifoSize = x->x_fifosize;
		
		x->x_fifohead += wantbytes;

		if (x->x_fifohead >= x->x_fifosize)
			x->x_fifohead = 0;
		
		if (tailpush)
			x->x_fifotail = x->x_fifohead;
		
		else if (x->x_state == STATE_STREAM_JUST_STARTING && vecsize > 0) {
			x->x_state = STATE_STREAM;
			clock_delay(x->x_m5StartTimeOutClock, 0);
		}
		if (is_finished)
		{
			x->x_state = STATE_IDLE_2;
			x->x_requestcode = REQUEST_CLOSE;
			sfread_cond_signal(&x->x_requestcondition);	
		}
		else if ((--x->x_sigcountdown) <= 0)
		{
#ifdef DEBUG_SOUNDFILE_THREADS
			fprintf(stderr, "writesf~: signal 1\n");
#endif
			sfread_cond_signal(&x->x_requestcondition);
			x->x_sigcountdown = x->x_sigperiod;
		}
		pthread_mutex_unlock(&x->x_mutex);
	}
	else if (x->x_state == STATE_IDLE_2)
	{
		pthread_mutex_lock(&x->x_mutex);
		if (x->x_m5FramesWrittenReport != FRAMES_NOT_UPDATED) 
		{
			x->x_m5SoundFileFramesAvailableFromOnset = x->x_m5FramesWrittenReport;
			x->x_m5FramesWrittenReport = 0;
			clock_delay(x->x_m5FramesOutClock, 0);
			x->x_state = STATE_IDLE;
		}
		pthread_mutex_unlock(&x->x_mutex);
	}
	return w + 2;
}

	/** start making output.  If we're in the "startup" state change
		to the "running" state. */
static void m5_writesf_start(t_writesf *x, t_symbol *s, int argc, t_atom *argv)
{
	t_m5FrameTimeCode ftc;
	if (x->x_state != STATE_STARTUP)
	{
		pd_error(x, "[writesf~]: start requested with no prior 'open'");
		return;
	}
	if (argc == 0)
	{
		pthread_mutex_lock(&x->x_mutex);		
		x->x_state = STATE_STREAM_JUST_STARTING;
		x->x_m5PlayStartTime = START_NOW;
		x->x_m5LocalTimeAnchor = clock_getlogicaltime();
		sfread_cond_signal(&x->x_requestcondition);
		pthread_mutex_unlock(&x->x_mutex);
		return;
	} else if (argc == 1)
	{
		x->x_m5PlayStartThreshold = atom_getfloatarg(0, argc, argv);
		pthread_mutex_lock(&x->x_mutex);	
		x->x_state = STATE_STREAM_JUST_STARTING;
		x->x_m5PlayStartTime = START_AT_THRESHOLD;
		x->x_m5LocalTimeAnchor = clock_getlogicaltime();
		sfread_cond_signal(&x->x_requestcondition);
		pthread_mutex_unlock(&x->x_mutex);
		return;
	}
	if (m5_frame_time_code_from_atoms(argc, argv, &ftc)) {
		pd_error (x,"m5_writesf~: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	}
	long ll = m5_frames_from_time_code(&ftc);
	if (ll < 0) {
		pd_error (x,"m5_writesf~: start time must be >= 0 frames.");
		return;
	}
	pthread_mutex_lock(&x->x_mutex);	
	x->x_state = STATE_STREAM_JUST_STARTING;
	x->x_m5PlayStartTime = (double)ll;
	x->x_m5LocalTimeAnchor = clock_getlogicaltime();
	sfread_cond_signal(&x->x_requestcondition);
	pthread_mutex_unlock(&x->x_mutex);
	

}

	/** LATER rethink whether you need the mutex just to set a variable? */
static void m5_writesf_stop(t_writesf *x, t_symbol *s, int argc, t_atom *argv)
{
	t_m5FrameTimeCode ftc;
	if (x->x_state != STATE_STREAM && x->x_state != STATE_STREAM_JUST_STARTING && x->x_state != STATE_STARTUP) 
	{
		pd_error(x, "[m5_writesf~]: stop requested with no prior 'open'");
		return;
	}
	if (argc == 0) {
		pthread_mutex_lock(&x->x_mutex);
		x->x_state = STATE_IDLE_2;
		x->x_requestcode = REQUEST_CLOSE;
		sfread_cond_signal(&x->x_requestcondition);
		pthread_mutex_unlock(&x->x_mutex);
		return;
	}
	if (m5_frame_time_code_from_atoms(argc, argv, &ftc)) {
		pd_error (x,"m5_writesf~: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	}
	long ll = m5_frames_from_time_code(&ftc);
	if (ll < 0) {
		pd_error (x,"m5_writesf~: end time must be >= 0 frames.");
		return;
	}
	pthread_mutex_lock(&x->x_mutex);
	x->x_m5PlayEndTime = (double)ll;
	sfread_cond_signal(&x->x_requestcondition);
	pthread_mutex_unlock(&x->x_mutex);
}

static void m5_writesf_time_set(t_writesf *x, t_symbol *s)
{
	t_m5TimeAnchor *a;
	
	x->x_m5TimeAnchorName = s;
	if (!s) {
		x->x_m5TimeAnchor = 0;		
		return;
	}
	if (s == gensym("self")) 
	{
		x->x_m5TimeAnchor = 0;		
		return;
	}
	if (!(a = m5_time_anchor_find(s)))
	{
		if (*s->s_name) pd_error(x, "m5_writesf~: %s: no such time anchor",
			x->x_m5TimeAnchorName->s_name);
		x->x_m5TimeAnchor = 0;
	}
	else m5_time_anchor_usedindsp(a);
	
	x->x_m5TimeAnchor = a;
}

static void m5_writesf_time(t_writesf *x, t_symbol *name) 
{
	m5_readsf_time_set(x, name);
}

	/** open method.  Called as: open [flags] filename with args as in
		soundfiler_parsewriteargs(). */
static void m5_writesf_open(t_writesf *x, t_symbol *s, int argc, t_atom *argv)
{
	t_soundfiler_writeargs wa = {0};
	if (x->x_state != STATE_IDLE)
		m5_writesf_stop(x, 0, 0, 0);
	if (m5_soundfiler_parsewriteargs(x, &argc, &argv, &wa) || wa.wa_ascii)
	{
		pd_error(x, "[writesf~]: usage; open [flags] filename...");
		post("flags: -bytes <n> %s -big -little -rate <n>", m5_sf_typeargs);
		return;
	}
	if (wa.wa_normalize || wa.wa_onsetframes || (wa.wa_nframes != SFMAXFRAMES))
		pd_error(x, "[writesf~] open: normalize/onset/nframes argument ignored");
	if (argc)
		pd_error(x, "[writesf~] open: extra argument(s) ignored");
	pthread_mutex_lock(&x->x_mutex);
		/* make sure that the child thread has finished writing */
	while (x->x_requestcode != REQUEST_NOTHING)
	{
		sfread_cond_signal(&x->x_requestcondition);
		sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
	}
	x->x_filename = wa.wa_filesym->s_name;
	x->x_sf.sf_type = wa.wa_type;
	if (wa.wa_samplerate > 0)
		x->x_sf.sf_samplerate = wa.wa_samplerate;
	else if (x->x_insamplerate > 0)
		x->x_sf.sf_samplerate = x->x_insamplerate;
	else x->x_sf.sf_samplerate = sys_getsr();
	x->x_sf.sf_bytespersample =
		(wa.wa_bytespersample > 2 ? wa.wa_bytespersample : 2);
	x->x_sf.sf_bigendian = wa.wa_bigendian;
	x->x_sf.sf_bytesperframe = x->x_sf.sf_nchannels * x->x_sf.sf_bytespersample;
	x->x_frameswritten = 0;
	x->x_requestcode = REQUEST_OPEN;
	x->x_fifotail = 0;
	x->x_fifohead = 0;
	x->x_eof = 0;
	x->x_fileerror = 0;
	x->x_state = STATE_STARTUP;
	
	x->x_m5FramesWrittenReport = FRAMES_NOT_UPDATED;
	
	x->x_m5PlayStartTime = START_NOW;
	x->x_m5PlayEndTime = END_NEVER;
	x->x_m5PerformedFifoSize = 0;
	
		/* set fifosize from bufsize.  fifosize must be a
		multiple of the number of bytes eaten for each DSP
		tick.  */
	x->x_fifosize = x->x_bufsize - (x->x_bufsize %
		(x->x_sf.sf_bytesperframe * MAXVECSIZE));
		/* arrange for the "request" condition to be signaled 16
			times per buffer */
	x->x_sigcountdown = x->x_sigperiod = (x->x_fifosize /
			(16 * (x->x_sf.sf_bytesperframe * x->x_vecsize)));
	sfread_cond_signal(&x->x_requestcondition);
	pthread_mutex_unlock(&x->x_mutex);
}

static void m5_writesf_dsp(t_writesf *x, t_signal **sp)
{
	m5_writesf_time_set(x, x->x_m5TimeAnchorName);
	int i, ninlets = x->x_sf.sf_nchannels;
	pthread_mutex_lock(&x->x_mutex);
	x->x_vecsize = sp[0]->s_n;
	x->x_sigperiod = (x->x_fifosize /
			(16 * x->x_sf.sf_bytesperframe * x->x_vecsize));
	for (i = 0; i < ninlets; i++)
		x->x_outvec[i] = sp[i]->s_vec;
	x->x_insamplerate = sp[0]->s_sr;
	pthread_mutex_unlock(&x->x_mutex);
	dsp_add(m5_writesf_perform, 1, x);
}

static void m5_writesf_print(t_writesf *x)
{
	post("state %d", x->x_state);
	post("fifo head %d", x->x_fifohead);
	post("fifo tail %d", x->x_fifotail);
	post("fifo size %d", x->x_fifosize);
	post("fd %d", x->x_sf.sf_fd);
	post("eof %d", x->x_eof);
	post ("start time %d", (size_t)x->x_m5PlayStartTime);
	post ("end time %d", (size_t)x->x_m5PlayEndTime);
	post ("length %d", (size_t)x->x_m5PlayEndTime - (size_t)x->x_m5PlayStartTime);
}

	/** request QUIT and wait for acknowledge */
static void m5_writesf_free(t_writesf *x)
{
	void *threadrtn;
	pthread_mutex_lock(&x->x_mutex);
	x->x_requestcode = REQUEST_QUIT;
#ifdef DEBUG_SOUNDFILE_THREADS
	fprintf(stderr, "writesf~: stopping thread...\n");
#endif
	sfread_cond_signal(&x->x_requestcondition);
	while (x->x_requestcode != REQUEST_NOTHING)
	{
#ifdef DEBUG_SOUNDFILE_THREADS
		fprintf(stderr, "writesf~: signaling...\n");
#endif
		sfread_cond_signal(&x->x_requestcondition);
		sfread_cond_wait(&x->x_answercondition, &x->x_mutex);
	}
	pthread_mutex_unlock(&x->x_mutex);
	if (pthread_join(x->x_childthread, &threadrtn))
		pd_error(x, "[writesf~] free: join failed");
#ifdef DEBUG_SOUNDFILE_THREADS
	fprintf(stderr, "writesf~: ... done\n");
#endif

	pthread_cond_destroy(&x->x_requestcondition);
	pthread_cond_destroy(&x->x_answercondition);
	pthread_mutex_destroy(&x->x_mutex);
	freebytes(x->x_buf, x->x_bufsize);
	// clock_free(x->x_clock);
	clock_free(x->x_m5FramesOutClock);
	clock_free(x->x_m5StartTimeOutClock);
}

static void m5_writesf_setup(void)
{
	m5_writesf_class = class_new(gensym("m5_writesf~"),
		(t_newmethod)m5_writesf_new, (t_method)m5_writesf_free,
		sizeof(t_writesf), 0, A_DEFFLOAT, A_DEFFLOAT, 0);
	class_addmethod(m5_writesf_class, (t_method)m5_writesf_start, gensym("start"), A_GIMME, 0);
	class_addmethod(m5_writesf_class, (t_method)m5_writesf_stop, gensym("stop"), A_GIMME, 0);
	class_addmethod(m5_writesf_class, (t_method)m5_writesf_dsp,
		gensym("dsp"), A_CANT, 0);
	class_addmethod(m5_writesf_class, (t_method)m5_writesf_open,
		gensym("open"), A_GIMME, 0);
	class_addmethod(m5_writesf_class, (t_method)m5_writesf_print, gensym("print"), 0);
	class_addmethod(m5_writesf_class, (t_method)m5_writesf_time, gensym("time"), A_SYMBOL, 0);
	CLASS_MAINSIGNALIN(m5_writesf_class, t_writesf, x_f);
}

/* ------------------------- global setup routine ------------------------ */

void m5_soundfile_setup(void)
{
	m5_soundfile_type_setup();
	// soundfiler_setup();
	m5_readsf_setup();
	m5_writesf_setup();
	
	m5_time_anchor_setup();
	m5_ftc_add_setup();
	m5_ftc_mult_setup();
	m5_ftc_cycles_setup();
	m5_ftc_compare_setup();
}
