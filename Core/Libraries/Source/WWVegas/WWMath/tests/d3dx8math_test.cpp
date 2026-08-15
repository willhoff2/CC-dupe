/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

/***********************************************************************************************
 *                                                                                             *
 *                 Project Name : Westwood Math Library                                        *
 *                                                                                             *
 *  Numerical test for the D3DX 8 math entry points WWMath/d3dx8math.cpp implements off Windows. *
 *                                                                                             *
 *  A compile proves nothing about these. Every one of them is a convention: D3DX puts the       *
 *  vector on the LEFT (v' = v * M) and the translation in the fourth ROW, while WWMath's        *
 *  Matrix4x4 puts it on the right and the translation in the fourth COLUMN, so an              *
 *  implementation with the operands or the transpose the wrong way round links, runs, and       *
 *  renders a subtly wrong picture. The call sites make that worse by reinterpret-casting a      *
 *  Matrix4x4 straight to a D3DXMATRIX& (sortingrenderer.cpp, pointgr.cpp), which is only        *
 *  self-consistent if these functions do exactly what d3dx8.lib does.                          *
 *                                                                                             *
 *  So Windows is the oracle here as everywhere else: every expectation below is D3DX's          *
 *  documented behaviour on the STORED elements, checked either against a reference written out  *
 *  from the definition (the multiply, the transform) or against a hand-computed matrix. The     *
 *  test builds on Windows too, where it exercises d3dx8.lib itself.                            *
 *                                                                                             *
 *  Run through scripts/native-d3dx8math-test.py.                                               *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <d3dx8math.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static int _Failures = 0;
static int _Checks = 0;

static void Check(bool condition, const char * what)
{
	_Checks++;
	if (!condition) {
		_Failures++;
		printf("FAIL: %s\n", what);
	}
}

static void Check_Close(float actual, float expected, float tolerance, const char * what)
{
	_Checks++;
	if (!(fabsf(actual - expected) <= tolerance)) {
		_Failures++;
		printf("FAIL: %s (got %.6f, expected %.6f)\n", what, actual, expected);
	}
}

static void Check_Matrix_Close(const D3DXMATRIX & actual, const D3DXMATRIX & expected,
	float tolerance, const char * what)
{
	_Checks++;
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			if (!(fabsf(actual.m[row][column] - expected.m[row][column]) <= tolerance)) {
				_Failures++;
				printf("FAIL: %s (m[%d][%d] is %.6f, expected %.6f)\n", what, row, column,
					actual.m[row][column], expected.m[row][column]);
				return;
			}
		}
	}
}

// D3DX's multiply, written out from its definition on the stored elements, so the assertion is
// against the definition rather than against another arrangement of the same code.
static D3DXMATRIX Reference_Multiply(const D3DXMATRIX & left, const D3DXMATRIX & right)
{
	D3DXMATRIX product;
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			float sum = 0.0f;
			for (int k = 0; k < 4; k++) {
				sum += left.m[row][k] * right.m[k][column];
			}
			product.m[row][column] = sum;
		}
	}
	return product;
}

// D3DX transforms a row vector: out[j] = sum_i v[i] * M[i][j], with v[3] = 1 for a point.
static void Reference_Transform(float out[4], const float value[4], const D3DXMATRIX & matrix)
{
	for (int column = 0; column < 4; column++) {
		float sum = 0.0f;
		for (int row = 0; row < 4; row++) {
			sum += value[row] * matrix.m[row][column];
		}
		out[column] = sum;
	}
}

static D3DXMATRIX Identity()
{
	D3DXMATRIX identity;
	D3DXMatrixIdentity(&identity);
	return identity;
}

// A matrix with no symmetry and no repeated element, so an accidental transpose anywhere shows up.
static D3DXMATRIX Asymmetric_A()
{
	return D3DXMATRIX(
		 1.0f,  2.0f,  3.0f,  4.0f,
		 5.0f,  6.0f,  7.0f,  8.0f,
		 9.0f, 10.0f, 11.0f, 12.0f,
		13.0f, 14.0f, 15.0f, 16.0f);
}

static D3DXMATRIX Asymmetric_B()
{
	return D3DXMATRIX(
		17.0f,  2.0f, -3.0f,  0.5f,
		 0.0f, 19.0f,  4.0f, -1.5f,
		-6.0f,  1.0f, 23.0f,  2.5f,
		 7.0f, -8.0f,  9.0f, 29.0f);
}

// An invertible affine transform built the way the renderer builds them: scale, then rotate about
// Z, then translate. Determinant is the scale product, 2 * 3 * 4 = 24, for any rotation.
static D3DXMATRIX Affine_Transform()
{
	D3DXMATRIX scaling;
	D3DXMATRIX rotation;
	D3DXMATRIX translation;
	D3DXMatrixScaling(&scaling, 2.0f, 3.0f, 4.0f);
	D3DXMatrixRotationZ(&rotation, 0.7f);
	D3DXMatrixTranslation(&translation, 10.0f, -20.0f, 30.0f);

	D3DXMATRIX transform;
	D3DXMatrixMultiply(&transform, &scaling, &rotation);
	D3DXMatrixMultiply(&transform, &transform, &translation);
	return transform;
}


/***********************************************************************************************
 *  The layout of the definitional builders. Which row the translation lands in is the whole    *
 *  convention question, and it is invisible to the compiler.                                   *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Builders()
{
	D3DXMATRIX identity;
	Check(D3DXMatrixIdentity(&identity) == &identity, "D3DXMatrixIdentity returns its output");
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			Check(identity.m[row][column] == ((row == column) ? 1.0f : 0.0f),
				"D3DXMatrixIdentity writes the identity");
		}
	}

	D3DXMATRIX scaling;
	D3DXMatrixScaling(&scaling, 2.0f, 3.0f, 4.0f);
	Check(scaling.m[0][0] == 2.0f && scaling.m[1][1] == 3.0f && scaling.m[2][2] == 4.0f
		&& scaling.m[3][3] == 1.0f, "D3DXMatrixScaling scales along the diagonal");
	Check(scaling.m[3][0] == 0.0f && scaling.m[0][3] == 0.0f,
		"D3DXMatrixScaling leaves the translation row and column clear");

	D3DXMATRIX translation;
	D3DXMatrixTranslation(&translation, 10.0f, -20.0f, 30.0f);
	// The row-vector convention: v * T, so T's fourth ROW is the offset. The transposed spelling
	// compiles identically and would break every shader constant the water renderer uploads.
	Check(translation.m[3][0] == 10.0f && translation.m[3][1] == -20.0f
		&& translation.m[3][2] == 30.0f,
		"D3DXMatrixTranslation puts the offset in the fourth row");
	Check(translation.m[0][3] == 0.0f && translation.m[1][3] == 0.0f
		&& translation.m[2][3] == 0.0f && translation.m[3][3] == 1.0f,
		"D3DXMatrixTranslation leaves the fourth column as the identity's");

	// Same convention for the rotation: (1, 0, 0, 1) * RotationZ(a) is (cos a, sin a, 0, 1), which
	// is the positive sine ABOVE the diagonal. pointgr.cpp reinterpret-casts the result into a
	// Matrix4x4, so the sign here is what decides which way its quads spin.
	const float angle = 0.7f;
	D3DXMATRIX rotation;
	D3DXMatrixRotationZ(&rotation, angle);
	Check_Close(rotation.m[0][0], cosf(angle), 1.0e-6f, "RotationZ m[0][0] is cos");
	Check_Close(rotation.m[0][1], sinf(angle), 1.0e-6f, "RotationZ m[0][1] is +sin");
	Check_Close(rotation.m[1][0], -sinf(angle), 1.0e-6f, "RotationZ m[1][0] is -sin");
	Check_Close(rotation.m[1][1], cosf(angle), 1.0e-6f, "RotationZ m[1][1] is cos");
	Check(rotation.m[2][2] == 1.0f && rotation.m[3][3] == 1.0f,
		"RotationZ leaves Z and W alone");

	float rotated[4];
	const float unit_x[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
	Reference_Transform(rotated, unit_x, rotation);
	Check_Close(rotated[0], cosf(angle), 1.0e-6f, "RotationZ turns +X towards +Y (x)");
	Check_Close(rotated[1], sinf(angle), 1.0e-6f, "RotationZ turns +X towards +Y (y)");
}


/***********************************************************************************************
 *  Transpose, including the in-place call W3DWater.cpp makes before uploading a matrix to the   *
 *  vertex shader constants.                                                                     *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Transpose()
{
	const D3DXMATRIX source = Asymmetric_A();

	D3DXMATRIX transposed;
	Check(D3DXMatrixTranspose(&transposed, &source) == &transposed,
		"D3DXMatrixTranspose returns its output");
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			Check(transposed.m[row][column] == source.m[column][row],
				"D3DXMatrixTranspose swaps rows and columns");
		}
	}

	// W3DWater.cpp: D3DXMatrixTranspose(&matWorldViewProj, &matWorldViewProj).
	D3DXMATRIX in_place = source;
	D3DXMatrixTranspose(&in_place, &in_place);
	Check_Matrix_Close(in_place, transposed, 0.0f, "D3DXMatrixTranspose transposes in place");

	D3DXMATRIX round_trip;
	D3DXMatrixTranspose(&round_trip, &transposed);
	Check_Matrix_Close(round_trip, source, 0.0f, "transposing twice is the original");
}


/***********************************************************************************************
 *  The multiply, which is the trap: routing it through Matrix4x4 without reversing the operands  *
 *  produces (B * A)^T. Both orders compile; only one draws the right picture.                    *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Multiply_Order()
{
	const D3DXMATRIX a = Asymmetric_A();
	const D3DXMATRIX b = Asymmetric_B();

	const D3DXMATRIX expected = Reference_Multiply(a, b);
	const D3DXMATRIX wrong_order = Reference_Multiply(b, a);

	// Guard the test itself: if these two agreed, the assertion below could not fire.
	bool differs = false;
	for (int row = 0; row < 4 && !differs; row++) {
		for (int column = 0; column < 4 && !differs; column++) {
			differs = (fabsf(expected.m[row][column] - wrong_order.m[row][column]) > 1.0f);
		}
	}
	Check(differs, "the chosen operands do not commute, so the order assertion can fail");

	D3DXMATRIX product;
	Check(D3DXMatrixMultiply(&product, &a, &b) == &product,
		"D3DXMatrixMultiply returns its output");
	Check_Matrix_Close(product, expected, 1.0e-3f, "D3DXMatrixMultiply(out, A, B) is A * B");

	// The operator sortingrenderer.cpp uses has to be the same product in the same order.
	const D3DXMATRIX by_operator = a * b;
	Check_Matrix_Close(by_operator, expected, 1.0e-3f, "D3DXMATRIX::operator * is A * B");

	// Aliasing: W3DWater.cpp accumulates world * view * proj through one output matrix.
	D3DXMATRIX accumulated = a;
	D3DXMatrixMultiply(&accumulated, &accumulated, &b);
	Check_Matrix_Close(accumulated, expected, 1.0e-3f,
		"D3DXMatrixMultiply is safe when the output aliases the left operand");
	accumulated = b;
	D3DXMatrixMultiply(&accumulated, &a, &accumulated);
	Check_Matrix_Close(accumulated, expected, 1.0e-3f,
		"D3DXMatrixMultiply is safe when the output aliases the right operand");

	// The same statement in the terms the renderer thinks in: scale then translate. With the
	// operands the WWMath way round, the point below would come out (22, 43, 64) -- translated
	// first and then scaled -- and nothing in the build would say so.
	D3DXMATRIX scaling;
	D3DXMATRIX translation;
	D3DXMatrixScaling(&scaling, 2.0f, 3.0f, 4.0f);
	D3DXMatrixTranslation(&translation, 10.0f, 20.0f, 30.0f);

	D3DXMATRIX scale_then_translate;
	D3DXMatrixMultiply(&scale_then_translate, &scaling, &translation);
	Check(scale_then_translate.m[3][0] == 10.0f && scale_then_translate.m[3][1] == 20.0f
		&& scale_then_translate.m[3][2] == 30.0f,
		"scaling * translation keeps the untouched offset in the fourth row");

	D3DXVECTOR3 point(1.0f, 1.0f, 1.0f);
	D3DXVECTOR4 transformed;
	D3DXVec3Transform(&transformed, &point, &scale_then_translate);
	Check_Close(transformed.x, 12.0f, 1.0e-4f, "scale then translate, x");
	Check_Close(transformed.y, 23.0f, 1.0e-4f, "scale then translate, y");
	Check_Close(transformed.z, 34.0f, 1.0e-4f, "scale then translate, z");
	Check_Close(transformed.w, 1.0f, 1.0e-4f, "a transformed point keeps w = 1");
}


/***********************************************************************************************
 *  The transforms, against the row-vector definition.                                           *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Transforms()
{
	const D3DXMATRIX matrix = Asymmetric_B();

	const float vector[4] = { 1.5f, -2.5f, 3.5f, -4.5f };
	float expected[4];
	Reference_Transform(expected, vector, matrix);

	D3DXVECTOR4 value(vector[0], vector[1], vector[2], vector[3]);
	D3DXVECTOR4 result;
	Check(D3DXVec4Transform(&result, &value, &matrix) == &result,
		"D3DXVec4Transform returns its output");
	Check_Close(result.x, expected[0], 1.0e-3f, "D3DXVec4Transform is v * M, x");
	Check_Close(result.y, expected[1], 1.0e-3f, "D3DXVec4Transform is v * M, y");
	Check_Close(result.z, expected[2], 1.0e-3f, "D3DXVec4Transform is v * M, z");
	Check_Close(result.w, expected[3], 1.0e-3f, "D3DXVec4Transform is v * M, w");

	// sortingrenderer.cpp transforms a bounding-sphere centre this way and then subscripts the
	// result, so the w component and the subscripting are both part of the contract.
	D3DXVECTOR3 point(1.5f, -2.5f, 3.5f);
	const float as_point[4] = { 1.5f, -2.5f, 3.5f, 1.0f };
	Reference_Transform(expected, as_point, matrix);

	D3DXVECTOR4 transformed;
	D3DXVec3Transform(&transformed, &point, &matrix);
	Check_Close(transformed[0], expected[0], 1.0e-3f, "D3DXVec3Transform is (v, 1) * M, x");
	Check_Close(transformed[1], expected[1], 1.0e-3f, "D3DXVec3Transform is (v, 1) * M, y");
	Check_Close(transformed[2], expected[2], 1.0e-3f, "D3DXVec3Transform is (v, 1) * M, z");
	Check_Close(transformed[3], expected[3], 1.0e-3f, "D3DXVec3Transform is (v, 1) * M, w");

	D3DXVECTOR4 left(1.0f, 2.0f, 3.0f, 4.0f);
	D3DXVECTOR4 right(-1.0f, 0.5f, 2.0f, -0.25f);
	Check_Close(D3DXVec4Dot(&left, &right), -1.0f + 1.0f + 6.0f - 1.0f, 1.0e-6f,
		"D3DXVec4Dot sums the four products");
}


/***********************************************************************************************
 *  The inverse: the composition assertion, the determinant out-parameter (which the water        *
 *  renderer passes and which may be null), and the singular case.                                *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Inverse()
{
	const D3DXMATRIX transform = Affine_Transform();
	const D3DXMATRIX identity = Identity();

	float determinant = 0.0f;
	D3DXMATRIX inverse;
	Check(D3DXMatrixInverse(&inverse, &determinant, &transform) == &inverse,
		"D3DXMatrixInverse returns its output for an invertible matrix");
	Check_Close(determinant, 24.0f, 1.0e-3f,
		"the determinant out-parameter is the scale product of the transform");

	// Composed either way round, within tolerance, which is the assertion that would catch a
	// transpose in either direction of the conversion.
	Check_Matrix_Close(Reference_Multiply(transform, inverse), identity, 1.0e-4f,
		"M * M^-1 is the identity");
	Check_Matrix_Close(Reference_Multiply(inverse, transform), identity, 1.0e-4f,
		"M^-1 * M is the identity");

	// A point pushed through both is where it started.
	const float point[4] = { 3.0f, -7.0f, 11.0f, 1.0f };
	float forward[4];
	float back[4];
	Reference_Transform(forward, point, transform);
	Reference_Transform(back, forward, inverse);
	for (int component = 0; component < 4; component++) {
		Check_Close(back[component], point[component], 1.0e-3f,
			"a point transformed and inverse-transformed is itself");
	}

	// The inverse of a transpose is the transpose of the inverse. This is the identity that lets
	// the implementation hand the work to Matrix4x4 across the conversion, so assert it rather
	// than trusting the argument.
	D3DXMATRIX transposed;
	D3DXMatrixTranspose(&transposed, &transform);
	D3DXMATRIX inverse_of_transpose;
	D3DXMatrixInverse(&inverse_of_transpose, nullptr, &transposed);
	D3DXMATRIX transpose_of_inverse;
	D3DXMatrixTranspose(&transpose_of_inverse, &inverse);
	Check_Matrix_Close(inverse_of_transpose, transpose_of_inverse, 1.0e-4f,
		"inversion and transposition commute");

	// The out-parameter is optional; W3DWater.cpp passes one, W3DShaderManager's callers do not.
	D3DXMATRIX without_determinant;
	Check(D3DXMatrixInverse(&without_determinant, nullptr, &transform) == &without_determinant,
		"D3DXMatrixInverse accepts a null determinant");
	Check_Matrix_Close(without_determinant, inverse, 0.0f,
		"the inverse does not depend on the determinant out-parameter");

	// Singular: a projection-flattened matrix, the shape a degenerate view matrix actually takes.
	// D3DX returns null; the call sites test the return and keep their previous matrix, so the
	// output must be left exactly as it was rather than filled with infinities.
	D3DXMATRIX singular = Identity();
	singular.m[2][2] = 0.0f;

	const D3DXMATRIX sentinel = Asymmetric_A();
	D3DXMATRIX output = sentinel;
	float singular_determinant = 12345.0f;
	Check(D3DXMatrixInverse(&output, &singular_determinant, &singular) == nullptr,
		"D3DXMatrixInverse returns null for a singular matrix");
	Check_Matrix_Close(output, sentinel, 0.0f,
		"a failed inverse does not write anything to its output");
	Check_Close(singular_determinant, 0.0f, 1.0e-6f,
		"a singular matrix reports a zero determinant");

	// A rank-deficient matrix whose zero is not on the diagonal, so the singularity has to be
	// found by the determinant rather than by a zero pivot: two identical rows.
	D3DXMATRIX repeated_row = Asymmetric_A();
	for (int column = 0; column < 4; column++) {
		repeated_row.m[2][column] = repeated_row.m[1][column];
	}
	output = sentinel;
	Check(D3DXMatrixInverse(&output, nullptr, &repeated_row) == nullptr,
		"D3DXMatrixInverse returns null for a matrix with two identical rows");
	Check_Matrix_Close(output, sentinel, 0.0f,
		"a failed inverse of a rank-deficient matrix writes nothing either");
}


/***********************************************************************************************
 *  D3DX_PI, which pointgr.cpp names.                                                            *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
static void Test_Constants()
{
	Check_Close(D3DX_PI, 3.141592654f, 1.0e-6f, "D3DX_PI is pi");
	// A full turn is the identity, which is what pointgr.cpp's 2 * D3DX_PI scaling assumes.
	D3DXMATRIX full_turn;
	D3DXMatrixRotationZ(&full_turn, 2.0f * D3DX_PI);
	Check_Matrix_Close(full_turn, Identity(), 1.0e-6f, "a full turn about Z is the identity");
}


int main()
{
	Test_Builders();
	Test_Transpose();
	Test_Multiply_Order();
	Test_Transforms();
	Test_Inverse();
	Test_Constants();

	printf("%d checks, %d failure(s)\n", _Checks, _Failures);
	return (_Failures == 0) ? 0 : 1;
}
