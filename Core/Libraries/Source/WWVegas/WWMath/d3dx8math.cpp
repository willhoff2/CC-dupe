/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// The D3DX 8 math entry points the engine names, implemented over WWMath off Windows.
//
// On Windows these come from d3dx8.lib, so this translation unit is empty there: defining them
// again would be a duplicate symbol. Off Windows d3dx8math.h is the declaration-only shim in
// scripts/native-port-shims/, and these five symbols were the whole of the "Direct3D 8 / DirectX"
// category in the native link that is not a device interface.
//
// D3DX's convention is a row vector on the left, v' = v * M, while Matrix4x4 stores rows and
// multiplies a column vector on the right, v' = M * v. To_Matrix4x4() already transposes a
// _D3DMATRIX into that convention, which is why the transform below is a conversion followed by
// the ordinary WWMath multiply rather than an index-by-index rewrite.

#if !defined(_WIN32)

#include "matrix4.h"
#include "vector4.h"

#include <d3d8types.h>
#include <d3dx8math.h>

D3DXVECTOR4::D3DXVECTOR4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f)
{
}

D3DXVECTOR4::D3DXVECTOR4(float x_value, float y_value, float z_value, float w_value)
	: x(x_value), y(y_value), z(z_value), w(w_value)
{
}

D3DXMATRIX::D3DXMATRIX(float m11, float m12, float m13, float m14,
                       float m21, float m22, float m23, float m24,
                       float m31, float m32, float m33, float m34,
                       float m41, float m42, float m43, float m44)
{
	m[0][0] = m11; m[0][1] = m12; m[0][2] = m13; m[0][3] = m14;
	m[1][0] = m21; m[1][1] = m22; m[1][2] = m23; m[1][3] = m24;
	m[2][0] = m31; m[2][1] = m32; m[2][2] = m33; m[2][3] = m34;
	m[3][0] = m41; m[3][1] = m42; m[3][2] = m43; m[3][3] = m44;
}

float D3DXVec4Dot(const D3DXVECTOR4 *left, const D3DXVECTOR4 *right)
{
	if (left == nullptr || right == nullptr) {
		return 0.0f;
	}
	return Vector4::Dot_Product(Vector4(left->x, left->y, left->z, left->w),
	                            Vector4(right->x, right->y, right->z, right->w));
}

D3DXVECTOR4 *D3DXVec4Transform(D3DXVECTOR4 *out, const D3DXVECTOR4 *value,
                               const D3DXMATRIX *matrix)
{
	if (out == nullptr || value == nullptr || matrix == nullptr) {
		return nullptr;
	}

	Matrix4x4 wwm;
	To_Matrix4x4(wwm, *matrix);
	const Vector4 result = wwm * Vector4(value->x, value->y, value->z, value->w);

	out->x = result.X;
	out->y = result.Y;
	out->z = result.Z;
	out->w = result.W;
	return out;
}

#endif // !_WIN32
