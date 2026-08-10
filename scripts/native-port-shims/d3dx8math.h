// Declaration-only stand-in for the D3DX 8 math header.
//
// WWMath needs D3DXMATRIX for conversion helpers, while the GameEngine's Bezier code also
// names D3DXVECTOR4 and two vector helpers. Declarations are enough for syntax probing and
// for the model renderer, which only default-constructs matrices and fills their base fields.
#pragma once

#include <d3d8types.h>

typedef struct D3DXVECTOR4 {
	float x, y, z, w;

	D3DXVECTOR4();
	D3DXVECTOR4(float x_value, float y_value, float z_value, float w_value);
} D3DXVECTOR4;

typedef struct D3DXMATRIX : public _D3DMATRIX {
	D3DXMATRIX() = default;
	D3DXMATRIX(float m11, float m12, float m13, float m14,
	           float m21, float m22, float m23, float m24,
	           float m31, float m32, float m33, float m34,
	           float m41, float m42, float m43, float m44);
} D3DXMATRIX;

D3DXVECTOR4 *D3DXVec4Transform(D3DXVECTOR4 *out, const D3DXVECTOR4 *value,
                              const D3DXMATRIX *matrix);
float D3DXVec4Dot(const D3DXVECTOR4 *left, const D3DXVECTOR4 *right);
