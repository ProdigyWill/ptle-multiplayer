#pragma once

// An affine matrix (row-vector : rows 0..2 the axes, row 3 the translation) split
// into rotation, scale and translation, for packing (protocol.cpp) and for
// blending two poses (remote.cpp).

#include "ptle/math/Matrix4f.h"


struct Affine
{
	float q[4];         // Rotation quaternion : x, y, z, w (unit length).
	float scale[3];     // Length of each axis row, negative when mirrored.
	float t[3];         // Translation.
	bool flat;          // An axis of length 0 : no rotation could be read (q is identity).
};

// perAxis false : the three scales are their average (bones scale uniformly).
void DecomposeAffine( const Matrix4f& m, Affine* out, bool perAxis );

void ComposeAffine( const Affine& a, Matrix4f* out );

// The matrix between a (f = 0) and b (f = 1) : rotations blended along the shorter
// way, scales and translations linearly. Flat or mirrored-against-not matrices
// don't blend : the nearer one is returned. Both must be sane (IsSaneMatrix).
void BlendAffine( const Matrix4f& a, const Matrix4f& b, float f, Matrix4f* out );
