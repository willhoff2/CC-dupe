// Declaration-only stand-in for the Direct3D 8 type header.
//
// WWMath's matrix3d.cpp and matrix4.cpp include this for one reason only: the
// To_D3DMATRIX / To_Matrix4x4 helpers that transpose Westwood's column-vector matrices into
// D3D's row-vector layout. Everything else in those files is platform-neutral, so a struct
// with the same member layout is enough to compile them natively -- see
// docs/porting/native-model-render.md.
//
// The layout matches the real header: an anonymous struct of _11.._44 unioned with m[4][4].
#pragma once

typedef struct _D3DMATRIX {
	union {
		struct {
			float _11, _12, _13, _14;
			float _21, _22, _23, _24;
			float _31, _32, _33, _34;
			float _41, _42, _43, _44;
		};
		float m[4][4];
	};
} D3DMATRIX;
