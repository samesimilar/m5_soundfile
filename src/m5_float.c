/* Copyright (c) 2025 Michael Spears. 
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

// setup params; m5_float~ anchorname <float>init_value
// messages received: list of 4 floats: time anchor + new value, e.g. "1 0 2400 1.0 " means "start stream of 1.0 at time 1 0 2400"
// single float - set immediately to float value , same as "1 0 0 <float>newvalue"
// 'cancel' - keep the current value and don't change
// 'now' - immediately change to the next value

#include <m_pd.h>
#include <math.h>
#include "m5_float.h"



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
	
	t_m5Float *x = (t_m5Float *)pd_new(m5_float_class);
	
	int i;
	int nchannels = 1;
	for (i = 0; i < nchannels; i++)
		outlet_new(&x->x_obj, gensym("signal"));
	
	// set basic values
	x->x_m5TimeAnchorName = 0;
	x->x_m5TimeAnchor = 0;
	x->x_m5LocalTimeAnchor = 0;
	x->x_value = 0.0;
	x->x_nextValue = 0.0;
	x->x_nextValueTime = 0.0;

	t_symbol *ts = 0;
	
	if (argc > 0) {
		if (argv->a_type == A_SYMBOL) {
			ts = atom_getsymbolarg(0, argc, argv);
		} else {
			if (argc == 1) {
				x->x_value = atom_getfloatarg(0, argc, argv);
				x->x_nextValue = x->x_value;
			} else {
				pd_error(x, "m5_float~: Object parameters should be: m5_float~ timeanchor_name(optional) float(optional)");
			}
		}
		if (argc == 2) {
			x->x_value = atom_getfloatarg(1, argc, argv);
			x->x_nextValue = x->x_value;
		}
		if (argc > 2) {
			pd_error(x, "m5_float~: Object parameters should be: m5_float~ timeanchor_name(optional) float(optional)");
		}
	}
	
	m5_float_time_set(x, ts);

	
	return x;
}

static void m5_fill_step_buffer(t_sample *buffer, size_t t_1, size_t vecsize, double t_change, t_sample K, t_sample J) {
	
	size_t t_2 = t_1 + vecsize;
	
	long side_A_size_l = ((long)t_2 < (long)t_change) ? (long)vecsize : (long)t_change - (long)t_1;
	
	
	size_t side_A_size = (size_t)(side_A_size_l < 0 ? 0 : side_A_size_l);
	
	
	size_t i;
	for (i = 0; i < side_A_size; i++) {
		buffer[i] = K;
	}
	for (i = side_A_size; i < vecsize; i++) {
		buffer[i] = J;
	}

}
static t_int *m5_float_perform(t_int *w)
{
	t_m5Float *x = (t_m5Float *)(w[1]);
	int vecsize = x->x_vecsize;

	t_sample *fp;
	
	
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
	
	fp = x->x_outvec[0];
	m5_fill_step_buffer(fp, blockStartTime, (size_t)vecsize, x->x_nextValueTime, x->x_value, x->x_nextValue);

	return w + 2;
}


static void m5_float_dsp(t_m5Float *x, t_signal **sp)
{
	
	// m5_float_time_set(x, x->x_m5TimeAnchorName);
	int i, noutlets = 1;
	x->x_vecsize = sp[0]->s_n;
	for (i = 0; i < noutlets; i++)
		x->x_outvec[i] = sp[i]->s_vec;
	
	dsp_add(m5_float_perform, 1, x);	
}
static void m5_float_free(t_m5Float *x)
{
	
}

static void m5_float_float(t_m5Float *x, t_float f) 
{
	x->x_nextValueTime = 0;
	
	x->x_nextValue = f;

}

static void m5_float_list(t_m5Float *x, t_symbol *s, int argc, t_atom *argv)
{
	t_m5FrameTimeCode ftc;
	if (argc < 4) {
		pd_error(x, "m5_float~ list message: must have 4 floats in list, first 3 comprise FTC value, last is next float value");
	}
	if (m5_frame_time_code_from_atoms(3, argv, &ftc)) {
		pd_error (x,"m5_float~: A frame time code must be three floats... 1|-1, epoch, frames.");
		return;
	}
	long ll = m5_frames_from_time_code(&ftc);
	if (ll < 0) {
		pd_error (x,"m5_float~: start time must be >= 0 frames.");
		return;
	}
	x->x_nextValue = atom_getfloatarg(3, argc, argv);
	x->x_nextValueTime = (double)ll;
	
	x->x_m5LocalTimeAnchor = clock_getlogicaltime();
	
	
}

static void m5_float_cancel(t_m5Float *x) {
	x->x_nextValue = x->x_value;
	x->x_nextValueTime = 0;
}
static void m5_float_now(t_m5Float *x) {
	x->x_nextValueTime = 0;
}

void m5_float_setup(void)
{
	m5_float_class = class_new(gensym("m5_float~"), 
		(t_newmethod) m5_float_new,
	    (t_method) m5_float_free, sizeof(t_m5Float), 0,  A_GIMME, A_NULL);
		
	class_addmethod(m5_float_class, (t_method)m5_float_dsp, gensym("dsp"), A_CANT, 0);
	class_addmethod(m5_float_class, (t_method)m5_float_cancel, gensym("cancel"), 0);
	class_addmethod(m5_float_class, (t_method)m5_float_now, gensym("now"), 0);
	
	class_addfloat(m5_float_class, (t_method)m5_float_float);
	class_addlist(m5_float_class, m5_float_list);
		
}