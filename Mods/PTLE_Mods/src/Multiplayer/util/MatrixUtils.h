#pragma once

#include "ptle/math/Matrix4f.h"

#include <string.h>


// Row-vector matrices (translation in row 3) : world = local * parent.
void MatMul( const Matrix4f& a, const Matrix4f& b, Matrix4f* out );

// Inverse of an affine matrix (3x3 part may be scaled; last column 0,0,0,1).
bool MatInverseAffine( const Matrix4f& m, Matrix4f* out );

// out[i] = m[i] * b (b : a parent to place on, or an inverse to make relative).
void MatsMul( const Matrix4f* m, int count, const Matrix4f& b, Matrix4f* out );


static inline void CopyMatrix( Matrix4f& to, const Matrix4f& from )
{
	memcpy( &to, &from, sizeof(Matrix4f) );
}
