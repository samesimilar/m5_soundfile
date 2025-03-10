/* Copyright (c) 2025 Michael Spears. 
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#include <m_pd.h>
#include <math.h>
#include <stdlib.h>
#include "m5_timeanchor.h"

// max sequential integer representable in float
#define FRAME_FLOAT_EPOCH 16777216

// indicates that TIME ANCHOR hasn't been read yet.
// T=0 will be defined on first request for time.
#define MARK_TIME_ANCHOR -1.

static t_class *m5_time_anchor_class;
static t_class *m5_ftc_add_class;
static t_class *m5_ftc_mult_class;
static t_class *m5_ftc_cycles_class;
static t_class *m5_ftc_compare_class;

/*
	
	m5_ftc_anchor, and utility functions
	
*/
static void *m5_time_anchor_new(t_symbol *s)
{
	t_m5TimeAnchor *x = (t_m5TimeAnchor *)pd_new(m5_time_anchor_class);
	x->x_usedindsp = 0;
	pd_bind(&x->x_obj.ob_pd, s);
	x->x_sym = s;

	x->x_starttime = MARK_TIME_ANCHOR;
	x->x_timeOut = outlet_new(&x->x_obj, &s_list);
	
	canvas_update_dsp();
	return (x);
}

t_m5TimeAnchor* m5_time_anchor_find(t_symbol *s) 
{
	return (t_m5TimeAnchor *)pd_findbyclass(s, m5_time_anchor_class);
}

static void m5_time_anchor_free(t_m5TimeAnchor *x)
{
	pd_unbind(&x->x_obj.ob_pd, x->x_sym);
	if (x->x_usedindsp == 1) {
		canvas_update_dsp();
	}
}

static void m5_time_anchor_mark(t_m5TimeAnchor *x) {
	x->x_starttime = clock_getlogicaltime();
}

static void m5_time_anchor_bang(t_m5TimeAnchor *x) 
{
	double now = m5_time_anchor_get_time_since_start(x);
	t_m5FrameTimeCode ftc;
	m5_frame_time_code_from_frames(now, &ftc);	
	m5_frame_time_code_out(&ftc, x->x_timeOut);
}

double m5_time_anchor_get_starttime(t_m5TimeAnchor *x) {
	if (x->x_starttime == MARK_TIME_ANCHOR) {
		// get current time on first  access
		x->x_starttime = clock_getlogicaltime();
	}
	return x->x_starttime;
}

unsigned long m5_time_anchor_get_time_since_start(t_m5TimeAnchor *x) {
	double start = m5_time_anchor_get_starttime(x);
	
	double r = ceil(clock_gettimesincewithunits(start, 1, 1));

	return (unsigned long) r;
}

void m5_time_anchor_usedindsp(t_m5TimeAnchor *x)
{
	x->x_usedindsp = 1;
}

void m5_frame_time_code_from_frames(long frames, t_m5FrameTimeCode *out) 
{	
	
	long aframes = labs(frames);
	out->sign = frames < 0 ? -1.0 : 1.0;
	out->epoch = (t_float)(aframes / FRAME_FLOAT_EPOCH);
	out->frames  = (t_float)(aframes % FRAME_FLOAT_EPOCH);
	
}

long m5_frames_from_time_code(t_m5FrameTimeCode *in) {
	return (long)in->sign * ((long)in->epoch * FRAME_FLOAT_EPOCH + (long)in->frames);	
}

void m5_frame_time_code_add(t_m5FrameTimeCode *in1, t_m5FrameTimeCode *in2, t_m5FrameTimeCode *out) 
{
	long in1Frames = m5_frames_from_time_code(in1);
	long in2Frames = m5_frames_from_time_code(in2);
	long sum = in1Frames + in2Frames;
	m5_frame_time_code_from_frames(sum, out);

}

void m5_frame_time_code_multiply_scalar(t_m5FrameTimeCode *in1, t_float s, t_m5FrameTimeCode *out)
{

	long in1Frames = m5_frames_from_time_code(in1);
	long product = (long)(floor((double)in1Frames * (double)s));
	
	m5_frame_time_code_from_frames(product, out);
}

int m5_frame_time_code_compare(t_m5FrameTimeCode *left, t_m5FrameTimeCode *right)
{

	long leftFrames = m5_frames_from_time_code(left);
	long rightFrames = m5_frames_from_time_code(right);

	
	if (leftFrames > rightFrames) return 1;
	else if (leftFrames == rightFrames) return 0;
	else return -1;
}

char m5_loop_position_from_clock_time(double clock, t_m5FrameTimeCode *loop_length, t_m5FrameTimeCode *out)
{
	long loop_frames = m5_frames_from_time_code(loop_length);
	if (loop_frames <= 0) 
	{
		return 1;
	}
	long now_frame = (long)clock % loop_frames;
	m5_frame_time_code_from_frames(now_frame, out);
	return 0;
}

// 'safety' allows for a constant offset for every calculation in case some extra time is needed
char m5_loop_start_from_clock_time(double clock, t_m5FrameTimeCode *offset, t_m5FrameTimeCode *loop_length, long offset_loops, long safety,  t_m5FrameTimeCode *out)
{
	long offset_frames = m5_frames_from_time_code(offset);
	long loop_frames = m5_frames_from_time_code(loop_length);	
	long lclock = (long)clock - offset_frames;
	if (loop_frames < 0) 
	{
		return 1;
	}
	if (loop_frames == 0)	
	{
		m5_frame_time_code_from_frames(lclock + safety, out);
		return 0;
	}
	long now_frame = lclock % loop_frames;	
	if (now_frame == 0) {
		m5_frame_time_code_from_frames(lclock + (offset_loops * loop_frames) + safety, out);
		return 0;
	}
	long next_start_frame = lclock + loop_frames + offset_frames - now_frame + (offset_loops * loop_frames) + safety;
	m5_frame_time_code_from_frames(next_start_frame, out);
	return 0;
}

char m5_loops_containing_duration(t_m5FrameTimeCode *inDuration, t_m5FrameTimeCode *loop_length, double *out_loop_count) 
{
	long duration_frames =  m5_frames_from_time_code(inDuration);
	if (duration_frames < 0)
	{
		return 1;
	}
	long loop_frames = m5_frames_from_time_code(loop_length);
	if (loop_frames <= 0) 
	{
		return 1;
	}
	
	*out_loop_count = (double) duration_frames / (double) loop_frames;
	
	return 0;
	
	
}

void m5_frame_time_code_out(t_m5FrameTimeCode *ftc, t_outlet *outlet) {
	t_atom a[3];
	
	SETFLOAT(a, ftc->sign);
	SETFLOAT(a+1, ftc->epoch);
	SETFLOAT(a+2, ftc->frames);
	
	outlet_list(outlet, &s_list, 3, a);
}

void m5_frame_time_code_out_prepend_symbol(t_symbol* s, t_m5FrameTimeCode *ftc, t_outlet *outlet) {
	t_atom a[3];
	
	SETFLOAT(a, ftc->sign);
	SETFLOAT(a+1, ftc->epoch);
	SETFLOAT(a+2, ftc->frames);
	
	;
	outlet_anything(outlet, s, 3, a);
}

char m5_frame_time_code_from_atoms(int argc, t_atom *a, t_m5FrameTimeCode *out) {
	
	// input check
	if (argc != 3 || a[0].a_type != A_FLOAT || a[1].a_type != A_FLOAT || a[2].a_type != A_FLOAT) 
	{
		return 1;
	}

	out->sign = atom_getfloat(a);
	out->epoch = atom_getfloat(a+1);
	out->frames = atom_getfloat(a+2);
	
	return 0;
}

void m5_frame_time_code_init( t_m5FrameTimeCode *out) 
{
	out->sign = 1.;
	out->epoch = 0.;
	out->frames = 0.;
	
}

void m5_time_anchor_setup(void)
{
	m5_time_anchor_class = class_new(gensym("m5_ftc_anchor"),
		(t_newmethod)m5_time_anchor_new, (t_method)m5_time_anchor_free,
		sizeof(t_m5TimeAnchor), 0, A_DEFSYM, 0);
	
	class_addmethod(m5_time_anchor_class, (t_method)m5_time_anchor_mark, gensym("mark"), 0);
	class_addbang(m5_time_anchor_class, (t_method)m5_time_anchor_bang);
		
}

/*
	
	m5_ftc_add
	
*/

static void m5_ftc_add_list(t_m5FTCAdd *x, t_symbol *s, int argc, t_atom *argv)
{
	t_m5FrameTimeCode inFTC;
	
	if (m5_frame_time_code_from_atoms(argc, argv, &inFTC)) {
		pd_error(x,"m5ftcAdd: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	}
	
	m5_frame_time_code_add(&inFTC, &x->x_ftcToAdd, &x->x_lastResult);
	m5_frame_time_code_out(&x->x_lastResult, x->x_sumOut);
}

static void m5_ftc_add_time2(t_m5FTCAdd *x, t_symbol *s, int argc, t_atom *argv)
{
	
	if (m5_frame_time_code_from_atoms(argc, argv, &x->x_ftcToAdd)) {
		pd_error (x,"m5ftcAdd: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	} 
	
}

static void m5_ftc_add_bang(t_m5FTCAdd *x)
{
	m5_frame_time_code_out(&x->x_lastResult, x->x_sumOut);
}

static void *m5_ftc_add_new(t_symbol*s, int argc, t_atom*argv)
{
	t_m5FTCAdd *x = (t_m5FTCAdd *)pd_new(m5_ftc_add_class);
	m5_frame_time_code_init(&x->x_ftcToAdd);
	m5_frame_time_code_init(&x->x_lastResult);
	m5_frame_time_code_from_atoms(argc, argv, &x->x_ftcToAdd);

	inlet_new(&x->x_obj, &x->x_obj.ob_pd, gensym("list"), gensym("time2"));

	x->x_sumOut = outlet_new(&x->x_obj, &s_list);
	return x;
}


static void m5_ftc_add_free(t_m5FTCAdd *x)
{
	
}
void m5_ftc_add_setup(void)
{
	m5_ftc_add_class = class_new(gensym("m5_ftc_add"),
		(t_newmethod)m5_ftc_add_new, 
		(t_method)m5_ftc_add_free,
		sizeof(t_m5FTCAdd),
		 0, A_GIMME, 0);
		 
	class_addlist(m5_ftc_add_class, m5_ftc_add_list);
	class_addmethod(m5_ftc_add_class, (t_method)m5_ftc_add_time2, gensym("time2"), A_GIMME, 0);
	class_addbang(m5_ftc_add_class, (t_method) m5_ftc_add_bang);
	
}

/*
	
	m5_ftc_mult
	
*/

static void m5_ftc_mult_list(t_m5FTCMult *x, t_symbol *s, int argc, t_atom *argv)
{
	t_m5FrameTimeCode inFTC;
	
	if (m5_frame_time_code_from_atoms(argc, argv, &inFTC)) {
		pd_error(x,"m5ftcMult: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	}
	
	m5_frame_time_code_multiply_scalar(&inFTC, x->x_scalar, &x->x_lastResult);

	m5_frame_time_code_out(&x->x_lastResult, x->x_prodOut);
}

static void m5_f5c_mult_bang(t_m5FTCMult *x)
{
	m5_frame_time_code_out(&x->x_lastResult, x->x_prodOut);
}
static void *m5_ftc_mult_new(t_symbol*s, int argc, t_atom*argv)
{
	t_m5FTCMult *x = (t_m5FTCMult *)pd_new(m5_ftc_mult_class);
	if (argc > 0) {
		x->x_scalar = atom_getfloat(argv);
	} else {
		x->x_scalar = 1.;		
	}
	floatinlet_new(&x->x_obj, &x->x_scalar);
	m5_frame_time_code_init(&x->x_lastResult);

	x->x_prodOut = outlet_new(&x->x_obj, &s_list);
	return x;
}

static void m5_ftc_mult_free(t_m5FTCMult *x)
{
	
}
void m5_ftc_mult_setup(void)
{
	m5_ftc_mult_class = class_new(gensym("m5_ftc_mult"),
		(t_newmethod)m5_ftc_mult_new,
		(t_method)m5_ftc_mult_free,
		sizeof(t_m5FTCMult), 0,
		A_GIMME, 0);		
	
	class_addlist(m5_ftc_mult_class, (t_method) m5_ftc_mult_list);
	class_addbang(m5_ftc_mult_class, (t_method) m5_f5c_mult_bang);
}

/*
	
	m5_ftc_cycles
	
*/

static void m5_ftc_cycles_get_start_time(t_m5FTCCycles *x, long offset_loops, long now) 
{
	
	t_m5FrameTimeCode startFTC;

	char e = 0;
	if ((e = m5_loop_start_from_clock_time(now, &x->x_offset, &x->x_loopLength, offset_loops, x->x_safety, &startFTC)))
	{
		pd_error(x, "m5ftcCycles: Error getting start time: %d", e);
		return;
	}
	m5_frame_time_code_out(&startFTC, x->x_timeOut);
}


static void m5_ftc_cycles_start_time(t_m5FTCCycles *x)
{
	t_m5TimeAnchor *tanchor;
	if (!(tanchor = m5_time_anchor_find(x->x_anchorSym)))
	{
		if (*x->x_anchorSym->s_name) pd_error(x, "m5ftcCycles: %s: no such time anchor",
			x->x_anchorSym->s_name);
		else pd_error(x, "m5ftcCycles: must provide time anchor name parameter to constructor");
		return;
	}
	double now = m5_time_anchor_get_time_since_start(tanchor);
	m5_ftc_cycles_get_start_time(x, 0, now);
}

static void m5_ftc_cycles_float(t_m5FTCCycles *x, t_float f)
{
	t_m5TimeAnchor *tanchor;
	if (!(tanchor = m5_time_anchor_find(x->x_anchorSym)))
	{
		if (*x->x_anchorSym->s_name) pd_error(x, "m5ftcCycles: %s: no such time anchor",
			x->x_anchorSym->s_name);
		else pd_error(x, "m5ftcCycles: must provide time anchor name parameter to constructor");
		return;
	}
	double now = m5_time_anchor_get_time_since_start(tanchor);
	m5_ftc_cycles_get_start_time(x, (long)f, now);
}

static void m5_ftc_cycles_list(t_m5FTCCycles *x, t_symbol *s, int argc, t_atom *argv) 
{
	t_m5FrameTimeCode inFTC;
	long now;
	
	t_float f = atom_getfloat(argv);
	
	if (argc == 1) {
		m5_ftc_cycles_float(x, f);
		return;
	}
	
	if (m5_frame_time_code_from_atoms(argc -1 , argv + 1, &inFTC)) {
		pd_error(x,"m5ftcCycles: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	}
	
	now = m5_frames_from_time_code(&inFTC);
	
	m5_ftc_cycles_get_start_time(x, (long)f, now);
}
static void m5_ftc_cycles_loop_length(t_m5FTCCycles *x, t_symbol *s, int argc, t_atom *argv)
{
	
	if (m5_frame_time_code_from_atoms(argc, argv, &x->x_loopLength)) {
		pd_error (x,"m5ftcCycles looplength: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	} 
	
}

static void m5_ftc_cycles_offset(t_m5FTCCycles *x, t_symbol *s, int argc, t_atom *argv)
{
	if (m5_frame_time_code_from_atoms(argc, argv, &x->x_offset)) {
		pd_error (x,"m5ftcCycles offset: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	} 
}

static void m5_ftc_cycles_loop_count(t_m5FTCCycles *x, t_symbol *s, int argc, t_atom *argv)
{
	t_m5FrameTimeCode in_duration;
	
	if (m5_frame_time_code_from_atoms(argc, argv, &in_duration)) {
		pd_error (x,"m5ftcCycles: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	} 
	
	double count;
	if (m5_loops_containing_duration(&in_duration, &x->x_loopLength, &count)) {
		pd_error(x, "m5ftcCycles loop count: Input error. Duration must be > 0. Loop length must be >= 0.");
		return;
	}
	
	outlet_float(x->x_timeOut, (t_float) count);
	
}

static void *m5_ftc_cycles_new(t_symbol*s, int argc, t_atom*argv)
{
	t_m5FTCCycles *x = (t_m5FTCCycles *)pd_new(m5_ftc_cycles_class);

	x->x_anchorSym = atom_getsymbolarg(0, argc, argv);
	
	m5_frame_time_code_init(&x->x_loopLength);
	m5_frame_time_code_init(&x->x_offset);
	m5_frame_time_code_from_atoms(argc, argv, &x->x_loopLength);
	inlet_new(&x->x_obj, &x->x_obj.ob_pd, gensym("list"), gensym("loop_length"));
	
	x->x_timeOut = outlet_new(&x->x_obj, &s_list);
	x->x_safety = 0;
	
	return x;
}

static void m5_ftc_cycles_free(t_m5FTCCycles *x)
{
	
}

void m5_ftc_cycles_setup(void)
{
	m5_ftc_cycles_class = class_new(gensym("m5_ftc_cycles"),
		(t_newmethod)m5_ftc_cycles_new,
		(t_method)m5_ftc_cycles_free,
		sizeof(t_m5FTCCycles), 0,
		A_GIMME, 0);	
		
	class_addmethod(m5_ftc_cycles_class, (t_method)m5_ftc_cycles_loop_length, gensym("loop_length"), A_GIMME, 0);
	class_addmethod(m5_ftc_cycles_class, (t_method)m5_ftc_cycles_offset, gensym("offset"), A_GIMME, 0);
	class_addmethod(m5_ftc_cycles_class, (t_method)m5_ftc_cycles_start_time, gensym("get_start"), 0);
	class_addmethod(m5_ftc_cycles_class, (t_method)m5_ftc_cycles_loop_count, gensym("count"), A_GIMME, 0);
	class_addlist(m5_ftc_cycles_class, (t_method)m5_ftc_cycles_list);
	class_addfloat(m5_ftc_cycles_class, (t_method)m5_ftc_cycles_float);
	class_addbang(m5_ftc_cycles_class, (t_method)m5_ftc_cycles_start_time);
}

/*
	
	m5_ftc_compare
	
*/

static void m5_ftc_compare_list(t_m5FTCCompare *x, t_symbol *s, int argc, t_atom *argv)
{
	t_m5FrameTimeCode inFTC;
	
	
	if (m5_frame_time_code_from_atoms(argc, argv, &inFTC)) {
		pd_error(x,"m5FTCCompare: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	}
	
	x->x_lastResult = m5_frame_time_code_compare(&inFTC, &x->x_ftcCompareRight);
	outlet_float(x->x_compareOut, x->x_lastResult);

}


static void m5_ftc_compare_right_value(t_m5FTCCompare *x, t_symbol *s, int argc, t_atom *argv)
{
	
	if (m5_frame_time_code_from_atoms(argc, argv, &x->x_ftcCompareRight)) {
		pd_error (x,"m5ftcCompare: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	} 
	
}

static void *m5_ftc_compare_new(t_symbol*s, int argc, t_atom*argv)
{
	t_m5FTCCompare *x = (t_m5FTCCompare *)pd_new(m5_ftc_compare_class);

	
	m5_frame_time_code_init(&x->x_ftcCompareRight);	
	m5_frame_time_code_from_atoms(argc, argv, &x->x_ftcCompareRight);
	inlet_new(&x->x_obj, &x->x_obj.ob_pd, gensym("list"), gensym("right"));
	
	x->x_compareOut = outlet_new(&x->x_obj, &s_list);
	x->x_lastResult = 0;
	
	return x;
}

static void m5_ftc_compare_bang(t_m5FTCCompare *x)
{
	outlet_float(x->x_compareOut, x->x_lastResult);
}


static void m5_ftc_compare_free(t_m5FTCCompare *x)
{
	
}
void m5_ftc_compare_setup(void)
{
	m5_ftc_compare_class = class_new(gensym("m5_ftc_compare"),
	(t_newmethod)m5_ftc_compare_new,
	(t_method)m5_ftc_compare_free,
	sizeof(t_m5FTCCompare), 0,
	A_GIMME, 0);
	
	class_addlist(m5_ftc_compare_class, m5_ftc_compare_list);
	class_addmethod(m5_ftc_compare_class, (t_method)m5_ftc_compare_right_value, gensym("right"), A_GIMME, 0);
	class_addbang(m5_ftc_compare_class, (t_method)m5_ftc_compare_bang);
}