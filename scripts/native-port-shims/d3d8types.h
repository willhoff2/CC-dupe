// Declaration-only stand-in for the Direct3D 8 type header.
//
// WWMath's matrix3d.cpp and matrix4.cpp include this for one reason only: the
// To_D3DMATRIX / To_Matrix4x4 helpers that transpose Westwood's column-vector matrices into
// D3D's row-vector layout. Everything else in those files is platform-neutral, so a struct
// with the same member layout is enough to compile them natively -- see
// docs/porting/native-model-render.md.
//
// The layout matches the real header: an anonymous struct of _11.._44 unioned with m[4][4].
//
// The guard is the vendored header's own, not `#pragma once`: a translation unit that reaches both
// -- the renderer layer's units see the vendored `d3d8types.h`, while `d3dx8math.h` here includes
// `<d3d8types.h>` -- would otherwise define `_D3DMATRIX` twice, since `#pragma once` is per file
// and these are two files. Sharing the guard makes whichever is found first the only definition.
#ifndef _D3D8TYPES_H_
#define _D3D8TYPES_H_

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

#endif /* _D3D8TYPES_H_ */
