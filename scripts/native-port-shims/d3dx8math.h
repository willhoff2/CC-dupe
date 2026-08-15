// Declaration-only stand-in for the D3DX 8 math header.
//
// WWMath needs D3DXMATRIX for conversion helpers, while the GameEngine's Bezier code also
// names D3DXVECTOR4 and two vector helpers. Declarations are enough for syntax probing and
// for the model renderer, which only default-constructs matrices and fills their base fields.
//
// The matrix entry points below are named by the water renderer (W3DWater.cpp), the point-group
// renderer (pointgr.cpp) and the sorting renderer (sortingrenderer.cpp). Their signatures and
// their conventions are the real header's: D3DX treats a vector as a row on the left of the
// matrix (v' = v * M) and stores translation in the fourth row. That is the opposite of WWMath's
// Matrix4x4, so the implementation in WWMath/d3dx8math.cpp is not a straight forwarding layer --
// read the comment at the top of it before changing anything here.
//
// This header replaces the vendored d3dx8math.h rather than complementing it, so a translation
// unit must not reach both: the vendored d3dx8core.h/d3dx8tex.h pull in their own copy through a
// quoted include that resolves beside them, and a unit needing those (W3DWater.cpp) should take
// its matrix declarations from there instead of from here. The entry points are declared with C
// linkage precisely so that it does not matter which of the two a unit saw -- both spell the same
// symbol, and WWMath/d3dx8math.cpp defines it once for either.
#pragma once

#include <d3d8types.h>

#define D3DX_PI ((float) 3.141592654f)

typedef struct D3DXVECTOR3 {
	float x, y, z;

	D3DXVECTOR3();
	D3DXVECTOR3(float x_value, float y_value, float z_value);

	operator float *();
	operator const float *() const;
} D3DXVECTOR3;

typedef struct D3DXVECTOR4 {
	float x, y, z, w;

	D3DXVECTOR4();
	D3DXVECTOR4(float x_value, float y_value, float z_value, float w_value);

	// The call sites pass a D3DXVECTOR4 where a shader constant's `const void *` is wanted and
	// subscript one as if it were a float array, both of which the real header's conversion
	// operator is what makes legal.
	operator float *();
	operator const float *() const;
} D3DXVECTOR4;

typedef struct D3DXMATRIX : public _D3DMATRIX {
	D3DXMATRIX() = default;
	D3DXMATRIX(float m11, float m12, float m13, float m14,
	           float m21, float m22, float m23, float m24,
	           float m31, float m32, float m33, float m34,
	           float m41, float m42, float m43, float m44);

	D3DXMATRIX operator *(const D3DXMATRIX &other) const;
} D3DXMATRIX;

extern "C" {

D3DXVECTOR4 *D3DXVec4Transform(D3DXVECTOR4 *out, const D3DXVECTOR4 *value,
                              const D3DXMATRIX *matrix);
float D3DXVec4Dot(const D3DXVECTOR4 *left, const D3DXVECTOR4 *right);
D3DXVECTOR4 *D3DXVec3Transform(D3DXVECTOR4 *out, const D3DXVECTOR3 *value,
                               const D3DXMATRIX *matrix);

D3DXMATRIX *D3DXMatrixIdentity(D3DXMATRIX *out);
D3DXMATRIX *D3DXMatrixTranspose(D3DXMATRIX *out, const D3DXMATRIX *matrix);
D3DXMATRIX *D3DXMatrixMultiply(D3DXMATRIX *out, const D3DXMATRIX *left,
                               const D3DXMATRIX *right);
// Returns null and leaves *out alone when the matrix is singular. `determinant` may be null.
D3DXMATRIX *D3DXMatrixInverse(D3DXMATRIX *out, float *determinant, const D3DXMATRIX *matrix);
D3DXMATRIX *D3DXMatrixScaling(D3DXMATRIX *out, float sx, float sy, float sz);
D3DXMATRIX *D3DXMatrixTranslation(D3DXMATRIX *out, float x, float y, float z);
D3DXMATRIX *D3DXMatrixRotationZ(D3DXMATRIX *out, float angle);

} // extern "C"
