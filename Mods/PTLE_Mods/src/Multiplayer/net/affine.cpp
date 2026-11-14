#include "net/affine.h"

#include <math.h>


void DecomposeAffine( const Matrix4f& m, Affine* out, bool perAxis )
{
	const float (*a)[4] = m.data;
	float len[3];
	for ( int i = 0; i < 3; i++ ) len[i] = sqrtf( a[i][0] * a[i][0] + a[i][1] * a[i][1] + a[i][2] * a[i][2] );
	if ( !perAxis ) len[0] = len[1] = len[2] = (len[0] + len[1] + len[2]) / 3.0F;
	float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
	          - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
	          + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	bool mirrored = det < 0.0F;

	// The rotation (a mirrored matrix is minus a rotation).
	float r[3][3];
	bool flat = false;
	for ( int i = 0; i < 3; i++ ) {
		if ( len[i] <= 1e-6F ) flat = true;
		float k = len[i] > 1e-6F ? (mirrored ? -1.0F : 1.0F) / len[i] : 0.0F;
		for ( int j = 0; j < 3; j++ ) r[i][j] = a[i][j] * k;
	}

	float* q = out->q;   // x, y, z, w.
	if ( flat ) {
		q[0] = q[1] = q[2] = 0.0F; q[3] = 1.0F;
	}
	else {
		float trace = r[0][0] + r[1][1] + r[2][2];
		if ( trace > 0.0F ) {
			float s = 0.5F / sqrtf( trace + 1.0F );
			q[3] = 0.25F / s;
			q[0] = (r[2][1] - r[1][2]) * s;
			q[1] = (r[0][2] - r[2][0]) * s;
			q[2] = (r[1][0] - r[0][1]) * s;
		}
		else if ( r[0][0] > r[1][1] && r[0][0] > r[2][2] ) {
			float s = 2.0F * sqrtf( 1.0F + r[0][0] - r[1][1] - r[2][2] );
			q[3] = (r[2][1] - r[1][2]) / s;
			q[0] = 0.25F * s;
			q[1] = (r[0][1] + r[1][0]) / s;
			q[2] = (r[0][2] + r[2][0]) / s;
		}
		else if ( r[1][1] > r[2][2] ) {
			float s = 2.0F * sqrtf( 1.0F + r[1][1] - r[0][0] - r[2][2] );
			q[3] = (r[0][2] - r[2][0]) / s;
			q[0] = (r[0][1] + r[1][0]) / s;
			q[1] = 0.25F * s;
			q[2] = (r[1][2] + r[2][1]) / s;
		}
		else {
			float s = 2.0F * sqrtf( 1.0F + r[2][2] - r[0][0] - r[1][1] );
			q[3] = (r[1][0] - r[0][1]) / s;
			q[0] = (r[0][2] + r[2][0]) / s;
			q[1] = (r[1][2] + r[2][1]) / s;
			q[2] = 0.25F * s;
		}
		float n = sqrtf( q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] );
		for ( int i = 0; i < 4; i++ ) q[i] /= n;
	}

	for ( int i = 0; i < 3; i++ ) {
		out->scale[i] = mirrored ? -len[i] : len[i];
		out->t[i] = a[3][i];
	}
	out->flat = flat;
}

void ComposeAffine( const Affine& in, Matrix4f* out )
{
	float x = in.q[0], y = in.q[1], z = in.q[2], w = in.q[3];
	const float* s = in.scale;
	float (*a)[4] = out->data;
	a[0][0] = (1.0F - 2.0F * (y * y + z * z)) * s[0];
	a[0][1] = 2.0F * (x * y - z * w) * s[0];
	a[0][2] = 2.0F * (x * z + y * w) * s[0];
	a[1][0] = 2.0F * (x * y + z * w) * s[1];
	a[1][1] = (1.0F - 2.0F * (x * x + z * z)) * s[1];
	a[1][2] = 2.0F * (y * z - x * w) * s[1];
	a[2][0] = 2.0F * (x * z - y * w) * s[2];
	a[2][1] = 2.0F * (y * z + x * w) * s[2];
	a[2][2] = (1.0F - 2.0F * (x * x + y * y)) * s[2];
	for ( int j = 0; j < 3; j++ ) {
		a[3][j] = in.t[j];
		a[j][3] = 0.0F;
	}
	a[3][3] = 1.0F;
}

void BlendAffine( const Matrix4f& ma, const Matrix4f& mb, float f, Matrix4f* out )
{
	Affine a, b;
	DecomposeAffine( ma, &a, true );
	DecomposeAffine( mb, &b, true );
	if ( a.flat || b.flat || (a.scale[0] < 0.0F) != (b.scale[0] < 0.0F) ) {
		*out = f < 0.5F ? ma : mb;
		return;
	}

	// Normalized lerp of the quaternions, the shorter way round (q and -q are the
	// same rotation). Snapshots are a frame or two apart : the angles are small.
	float dot = a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3];
	float sb = dot < 0.0F ? -f : f;
	Affine r;
	float n = 0.0F;
	for ( int i = 0; i < 4; i++ ) {
		r.q[i] = a.q[i] * (1.0F - f) + b.q[i] * sb;
		n += r.q[i] * r.q[i];
	}
	if ( n < 1e-12F ) { *out = f < 0.5F ? ma : mb; return; }
	n = 1.0F / sqrtf( n );
	for ( int i = 0; i < 4; i++ ) r.q[i] *= n;
	for ( int i = 0; i < 3; i++ ) {
		r.scale[i] = a.scale[i] + (b.scale[i] - a.scale[i]) * f;
		r.t[i] = a.t[i] + (b.t[i] - a.t[i]) * f;
	}
	r.flat = false;
	ComposeAffine( r, out );
}
