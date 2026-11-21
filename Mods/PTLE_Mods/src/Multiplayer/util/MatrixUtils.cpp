#include "MatrixUtils.h"

#include <math.h>


void MatMul( const Matrix4f& a, const Matrix4f& b, Matrix4f* out )
{
	Matrix4f r;
	for ( int i = 0; i < 4; i++ )
		for ( int j = 0; j < 4; j++ )
			r.data[i][j] = a.data[i][0] * b.data[0][j] + a.data[i][1] * b.data[1][j]
			+ a.data[i][2] * b.data[2][j] + a.data[i][3] * b.data[3][j];
	*out = r;
}

bool MatInverseAffine( const Matrix4f& m, Matrix4f* out )
{
	const float (*a)[4] = m.data;
	float c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
	float c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
	float c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
	float det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
	if ( fabsf(det) < 1e-8F ) return false;
	float k = 1.0F / det;
	Matrix4f r;
	r.data[0][0] = c00 * k;
	r.data[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * k;
	r.data[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * k;
	r.data[1][0] = c01 * k;
	r.data[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * k;
	r.data[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * k;
	r.data[2][0] = c02 * k;
	r.data[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * k;
	r.data[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * k;
	for ( int j = 0; j < 3; j++ ) {
		r.data[3][j] = -(a[3][0] * r.data[0][j] + a[3][1] * r.data[1][j] + a[3][2] * r.data[2][j]);
		r.data[j][3] = 0.0F;
	}
	r.data[3][3] = 1.0F;
	*out = r;
	return true;
}

void MatsMul( const Matrix4f* m, int count, const Matrix4f& b, Matrix4f* out )
{
	for ( int i = 0; i < count; i++ )
		MatMul( m[i], b, &out[i] );
}
