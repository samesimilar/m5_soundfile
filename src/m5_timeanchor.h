/* Copyright (c) 2025 Michael Spears. 
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

#pragma once

#include "m_pd.h"

// Basic FTC structure to count frames with t_float.
typedef struct _m5FrameTimeCode
{
	t_float sign;
	t_float epoch;
	t_float frames;
} t_m5FrameTimeCode;

// An object that stores an absolute time that defines t=0 for
// the group of objects that reference it.
typedef struct _m5TimeAnchor
{
	t_object x_obj;
	t_symbol *x_sym;
	char x_usedindsp;
	
	double x_starttime;
	
	t_outlet *x_timeOut;
	t_outlet *x_deltaOut;

} t_m5TimeAnchor;

// Add two FTC values.
typedef struct _m5FTCAdd
{
	t_object x_obj;
	t_outlet *x_sumOut;  
	t_m5FrameTimeCode x_ftcToAdd;	
	t_m5FrameTimeCode x_lastResult;
	
} t_m5FTCAdd;

// Multiply an FTC value * a t_float.
typedef struct _m5FTCmult
{
	t_object x_obj;
	t_outlet *x_prodOut;
	t_float x_scalar;
	t_m5FrameTimeCode x_lastResult;
	
} t_m5FTCMult;

// Compare two FTC values.
// Output -1 when left < right
// Output 0 when left == right
// Output +1 when left > right
typedef struct _m5FTCcompare
{
	t_object x_obj;
	t_outlet *x_compareOut;
	t_m5FrameTimeCode x_ftcCompareRight;
	t_float x_lastResult;
} t_m5FTCCompare;

// Compute next start frame (FTC) of a loop for a given 'loop length',
// (relative to a given FTC 'time anchor').
typedef struct _m5FTCcycles
{
	t_object x_obj;
	t_outlet *x_timeOut;
	t_symbol *x_anchorSym;
	t_m5FrameTimeCode x_loopLength;
	long x_safety;
	
} t_m5FTCCycles;

// Pd object definitions
void m5_time_anchor_setup(void);
void m5_ftc_add_setup(void);
void m5_ftc_mult_setup(void);
void m5_ftc_cycles_setup(void);
void m5_ftc_compare_setup(void);

// Useful functions for working with FTCs and FTC time anchors...

void m5_time_anchor_usedindsp(t_m5TimeAnchor *x);
double m5_time_anchor_get_starttime(t_m5TimeAnchor *x);
unsigned long m5_time_anchor_get_time_since_start(t_m5TimeAnchor *x);

// find FTC anchor in patcher forgiven ID symbol
t_m5TimeAnchor* m5_time_anchor_find(t_symbol *s) ;
void m5_time_anchor_usedindsp(t_m5TimeAnchor *x);

// conversions
void m5_frame_time_code_from_frames(long frames, t_m5FrameTimeCode *out);
long m5_frames_from_time_code(t_m5FrameTimeCode *in);

// FTC input / output
void m5_frame_time_code_out(t_m5FrameTimeCode *ftc, t_outlet *outlet);
void m5_frame_time_code_out_prepend_symbol(t_symbol* s, t_m5FrameTimeCode *ftc, t_outlet *outlet);
char m5_frame_time_code_from_atoms(int argc, t_atom *a, t_m5FrameTimeCode *out);

