/* Copyright (c) 2025 Michael Spears. 
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

// setup params; m5_float~ anchorname <float>init_value
// messages received: list of 4 floats: time anchor + new value, e.g. "1 0 2400 1.0 " means "start stream of 1.0 at time 1 0 2400"
// single float - set immediately to float value , same as "-1 FRAME_FLOAT_EPOCH FRAME_FLOAT_EPOCH <float>newvalue"

// TODO: figure out DSP setup function, write perform function, write new value schedule input

#include <m_pd.h>
#include "m5_timeanchor.h"

static t_class *m5_float_class;

typedef struct _m5Float
{
	t_object x_obj;
	t_symbol *x_m5TimeAnchorName;
	t_m5TimeAnchor *x_m5TimeAnchor;
	t_float x_value;
	t_float x_nextValue;
	t_m5FrameTimeCode x_nextTimecode;
} t_m5Float;

static void m5_float_time_set(t_m5Float *x, t_symbol *s)
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
		if (*s->s_name) pd_error(x, "m5_float~: %s: no such time anchor",
			x->x_m5TimeAnchorName->s_name);
		x->x_m5TimeAnchor = 0;
	}
	else m5_time_anchor_usedindsp(a);
	
	x->x_m5TimeAnchor = a;
}

static void *m5_float_new(t_symbol *s,  int argc, t_atom*argv)
{
	// todo: handle missing parameters
	t_m5Float *x = (t_m5Float *)pd_new(m5_float_class);
	t_symbol *ts = atom_getsymbolarg(0, argc, argv);
	
	x->x_m5TimeAnchorName = ts;
	m5_float_time_set(x, ts);
	x->x_value = atom_getfloatarg(0, argc, argv);
	x->x_nextValue = x->x_value;
	m5_frame_time_code_init(&x->x_nextTimecode);
	
	// set to earliest possible time
	x->x_nextTimecode.epoch = FRAME_FLOAT_EPOCH;
	x->x_nextTimecode.frames = FRAME_FLOAT_EPOCH;
	x->x_nextTimecode.sign = -1;
	
	if (ts == gensym("s")) {
		x->x_m5TimeAnchor = 0;
	}
	t_m5TimeAnchor *a;
	if (!(a = m5_time_anchor_find(ts))) 
	{
		if (*ts->s_name) pd_error(x, "m5_float~: %s no such time anchor",
			ts->s_name);
		x->x_m5TimeAnchor = 0;
	}
	else m5_time_anchor_usedindsp(a);
	x->x_m5TimeAnchor = a;
	
	return x;
}

static t_int *m5_float_perform(t_int *w)
{
	t_m5Float *x = (t_m5Float *)(w[1]);
	
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
}


static void m5_float_dsp(t_m5Float *x, t_signal **sp)
{
	
	m5_float_time_set(x, x->x_m5TimeAnchorName);
	/// ???
}
static void m5_float_free(t_m5Float *x)
{
	
}

void m5_float_setup(void)
{
	m5_float_class = class_new(gensym("m5_float~"), 
		(t_newmethod) m5_float_new,
	    (t_method) m5_float_free, sizeof(t_m5Float), A_GIMME, A_NULL);
		
	class_addmethod(m5_float_class, (t_method)m5_float_dsp, gensym("dsp"), A_CANT, 0);
		
}