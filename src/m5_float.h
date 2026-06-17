/* Copyright (c) 2025 Michael Spears. 
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#pragma once

#include "m_pd.h"
#include "m5_timeanchor.h"


static t_class *m5_float_class;

typedef struct _m5Float
{
	t_object x_obj;
	t_canvas *x_canvas;
	int x_vecsize;                    /**< vector size for transfers */
	t_sample *(x_outvec[1]); 
	
	t_symbol *x_m5TimeAnchorName;
	t_m5TimeAnchor *x_m5TimeAnchor;
	/* store t=0 if m5_ftc_anchor is not specified */
	double x_m5LocalTimeAnchor;
	
	t_float x_value;
	t_float x_nextValue;
	double x_nextValueTime;
} t_m5Float;


void m5_float_setup(void);
