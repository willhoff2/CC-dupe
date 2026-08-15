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
// scripts/native-port-shims/, and these symbols are the whole of the "Direct3D 8 / DirectX"
// category in the native link that is not a device interface or a texture/surface utility.
//
// D3DX's convention is a row vector on the left, v' = v * M, while Matrix4x4 stores rows and
// multiplies a column vector on the right, v' = M * v. To_Matrix4x4() already transposes a
// _D3DMATRIX into that convention, which is why the transform below is a conversion followed by
// the ordinary WWMath multiply rather than an index-by-index rewrite.
//
// Two consequences of that transpose, both of which compile silently either way and only show up
// as a wrong picture, so scripts/native-d3dx8math-test.py asserts them:
//
//   * A product does not survive the conversion in place. To_Matrix4x4(A) * To_Matrix4x4(B) is
//     A^T * B^T = (B * A)^T, so D3DXMatrixMultiply(out, A, B) -- which D3DX defines as out = A * B
//     on the stored elements -- has to multiply the *converted* operands in the opposite order.
//   * An inverse does survive it, because transposition and inversion commute and a determinant is
//     transpose-invariant; that is why D3DXMatrixInverse() can hand the work to Matrix4x4 whole.
//
// The definitional builders (identity, transpose, scaling, translation, rotation about Z) are
// written straight into the D3DX layout: routing them through WWMath would only add a transpose to
// undo, and it is the layout -- translation in the fourth row, sin() above the diagonal -- that is
// the contract worth stating explicitly.

#if !defined(_WIN32)

#include "matrix4.h"
#include "vector4.h"

#include <d3d8types.h>
#include <d3dx8math.h>

#include <cmath>

D3DXVECTOR3::D3DXVECTOR3() : x(0.0f), y(0.0f), z(0.0f)
{
}

D3DXVECTOR3::D3DXVECTOR3(float x_value, float y_value, float z_value)
	: x(x_value), y(y_value), z(z_value)
{
}

D3DXVECTOR3::operator float *()
{
	return &x;
}

D3DXVECTOR3::operator const float *() const
{
	return &x;
}

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

D3DXVECTOR4::operator float *()
{
	return &x;
}

D3DXVECTOR4::operator const float *() const
{
	return &x;
}

D3DXMATRIX D3DXMATRIX::operator *(const D3DXMATRIX &other) const
{
	D3DXMATRIX result;
	D3DXMatrixMultiply(&result, this, &other);
	return result;
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

// D3DX transforms a 3-vector as the point (x, y, z, 1), so the fourth row of the matrix is the
// translation that gets added.
D3DXVECTOR4 *D3DXVec3Transform(D3DXVECTOR4 *out, const D3DXVECTOR3 *value,
                               const D3DXMATRIX *matrix)
{
	if (out == nullptr || value == nullptr || matrix == nullptr) {
		return nullptr;
	}

	Matrix4x4 wwm;
	To_Matrix4x4(wwm, *matrix);
	const Vector4 result = wwm * Vector4(value->x, value->y, value->z, 1.0f);

	out->x = result.X;
	out->y = result.Y;
	out->z = result.Z;
	out->w = result.W;
	return out;
}

D3DXMATRIX *D3DXMatrixIdentity(D3DXMATRIX *out)
{
	if (out == nullptr) {
		return nullptr;
	}

	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			out->m[row][column] = (row == column) ? 1.0f : 0.0f;
		}
	}
	return out;
}

D3DXMATRIX *D3DXMatrixTranspose(D3DXMATRIX *out, const D3DXMATRIX *matrix)
{
	if (out == nullptr || matrix == nullptr) {
		return nullptr;
	}

	// The call sites transpose in place (a matrix into itself), so read the source out first.
	const D3DXMATRIX source = *matrix;
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			out->m[row][column] = source.m[column][row];
		}
	}
	return out;
}

D3DXMATRIX *D3DXMatrixMultiply(D3DXMATRIX *out, const D3DXMATRIX *left,
                               const D3DXMATRIX *right)
{
	if (out == nullptr || left == nullptr || right == nullptr) {
		return nullptr;
	}

	Matrix4x4 converted_left;
	Matrix4x4 converted_right;
	To_Matrix4x4(converted_left, *left);
	To_Matrix4x4(converted_right, *right);

	// Converted operands are transposed, so the product that converts back to left * right is
	// right^T * left^T.
	To_D3DMATRIX(*out, converted_right * converted_left);
	return out;
}

D3DXMATRIX *D3DXMatrixInverse(D3DXMATRIX *out, float *determinant, const D3DXMATRIX *matrix)
{
	if (out == nullptr || matrix == nullptr) {
		return nullptr;
	}

	Matrix4x4 converted;
	To_Matrix4x4(converted, *matrix);

	Matrix4x4 inverted;
	if (Matrix4x4::Inverse(&inverted, determinant, &converted) == nullptr) {
		// Singular: D3DX returns null and leaves the output matrix as it found it.
		return nullptr;
	}

	To_D3DMATRIX(*out, inverted);
	return out;
}

D3DXMATRIX *D3DXMatrixScaling(D3DXMATRIX *out, float sx, float sy, float sz)
{
	if (D3DXMatrixIdentity(out) == nullptr) {
		return nullptr;
	}

	out->m[0][0] = sx;
	out->m[1][1] = sy;
	out->m[2][2] = sz;
	return out;
}

D3DXMATRIX *D3DXMatrixTranslation(D3DXMATRIX *out, float x, float y, float z)
{
	if (D3DXMatrixIdentity(out) == nullptr) {
		return nullptr;
	}

	// Row vector on the left puts the translation in the fourth row, not the fourth column.
	out->m[3][0] = x;
	out->m[3][1] = y;
	out->m[3][2] = z;
	return out;
}

D3DXMATRIX *D3DXMatrixRotationZ(D3DXMATRIX *out, float angle)
{
	if (D3DXMatrixIdentity(out) == nullptr) {
		return nullptr;
	}

	const float cosine = cosf(angle);
	const float sine = sinf(angle);

	// Same convention again: (1, 0, 0, 1) * RotationZ(a) is (cos a, sin a, 0, 1), which puts the
	// positive sine above the diagonal.
	out->m[0][0] = cosine;
	out->m[0][1] = sine;
	out->m[1][0] = -sine;
	out->m[1][1] = cosine;
	return out;
}

#endif // !_WIN32
