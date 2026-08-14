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

/***********************************************************************************************
 *                                                                                             *
 *                 Project Name : Westwood Library                                             *
 *                                                                                             *
 *  Prints, as JSON, the glyph metrics a GDI text implementation reports for a font file: the    *
 *  TEXTMETRIC fields and the GetTextExtentPoint32W() advance of every printable ASCII           *
 *  character, at the sizes and in the shapes FontCharsClass asks for.                          *
 *                                                                                             *
 *  One source, two builds, because the point is to compare them:                               *
 *                                                                                             *
 *    - on Windows (the recording is made with VC6 under Wine, in the container                 *
 *      scripts/docker-build.sh uses) it runs against the real GDI and produces the reference    *
 *      data, scripts/ci/font-metrics-reference.json.                                           *
 *                                                                                             *
 *    - off Windows it links against WWLib/platform/platform_win32_gdi_font.cpp -- the port's    *
 *      own CreateFont/GetTextMetrics/GetTextExtentPoint32W -- and produces the numbers          *
 *      scripts/ci/check-font-metrics.py compares against that reference.                       *
 *                                                                                             *
 *  The font file is an argument on both sides, and both sides are pointed at the same file, so  *
 *  that a difference in the output is a difference in the text stack rather than in the         *
 *  typeface. On Windows the file is registered with AddFontResourceEx() and the face name GDI   *
 *  actually selected is printed, so a silent font substitution shows up in the reference        *
 *  instead of being averaged into it.                                                          *
 *                                                                                             *
 *  Usage: gdi_font_metrics_dump <font-file> <face-name>                                        *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include <windows.h>

#include <stdio.h>
#include <string.h>

/*
**	The sizes and shapes the engine creates fonts in. Create_GDI_Font() turns a point size into
**	lfHeight = -MulDiv(points, 96, 72) and, for the "Generals" face only, an lfWidth of
**	-lfHeight * 0.40 -- a condensed face. Both are reproduced here rather than described.
*/
struct FontCase
{
	int PointSize;
	int Bold;
	int Condensed;
};

static const FontCase FONT_CASES[] =
{
	{  8, 0, 0 },
	{ 10, 0, 0 },
	{ 12, 0, 0 },
	{ 12, 1, 0 },
	{ 14, 0, 0 },
	{ 16, 0, 0 },
	{ 18, 0, 0 },
	{ 18, 1, 0 },
	{ 24, 0, 0 },
	{ 12, 0, 1 },
	{ 16, 0, 1 },
	{ 18, 0, 1 },
	{ 24, 0, 1 },
	{ 12, 1, 1 },
	{ 0, 0, 0 }
};

static const int FIRST_CHAR = 32;
static const int LAST_CHAR = 126;

#ifdef _WIN32
/*
**	AddFontResourceExA is Windows 2000 and later; the VC6 headers this is compiled with predate
**	it, so it is fetched out of gdi32 by hand.
*/
typedef int (WINAPI * AddFontResourceExAProc)(LPCSTR, DWORD, void *);
#define FR_PRIVATE_FLAG 0x10

static int Register_Font_File(const char * path)
{
	HMODULE gdi = GetModuleHandleA("gdi32.dll");
	if (gdi == NULL) {
		return 0;
	}

	AddFontResourceExAProc add_font =
		(AddFontResourceExAProc)GetProcAddress(gdi, "AddFontResourceExA");
	if (add_font == NULL) {
		// Windows 9x. The recording is not made there; fall back to the global installer, which
		// needs the file to be in the Fonts directory already.
		return AddFontResourceA(path);
	}

	return add_font(path, FR_PRIVATE_FLAG, NULL);
}
#else
static int Register_Font_File(const char * path)
{
	// Off Windows the seam takes the file from WW_FONT_PATH, which the caller sets: there is no
	// font database to register anything with.
	(void)path;
	return 1;
}
#endif

static void Print_Escaped(const char * text)
{
	for (const char * cursor = text; *cursor != 0; ++cursor) {
		if (*cursor == '\\' || *cursor == '"') {
			printf("\\%c", *cursor);
		} else {
			printf("%c", *cursor);
		}
	}
}

int main(int argc, char ** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <font-file> <face-name>\n", argv[0]);
		return 2;
	}

	const char * font_file = argv[1];
	const char * face_name = argv[2];

	if (Register_Font_File(font_file) == 0) {
		fprintf(stderr, "could not register %s\n", font_file);
		return 3;
	}

	HDC screen_dc = GetDC(NULL);
	HDC dc = CreateCompatibleDC(screen_dc);

	/*
	**	The same scratch surface FontCharsClass draws into: a top-down 24bpp DIB section. The
	**	metrics do not depend on it, but selecting a bitmap is what makes the DC a drawable one,
	**	and this keeps the recording and the comparison in the same shape as the engine's use.
	*/
	BITMAPINFOHEADER bitmap_info;
	memset(&bitmap_info, 0, sizeof(bitmap_info));
	bitmap_info.biSize = sizeof(BITMAPINFOHEADER);
	bitmap_info.biWidth = 128;
	bitmap_info.biHeight = -128;
	bitmap_info.biPlanes = 1;
	bitmap_info.biBitCount = 24;
	bitmap_info.biCompression = BI_RGB;

	unsigned char * bits = NULL;
	HBITMAP bitmap = CreateDIBSection(screen_dc, (const BITMAPINFO *)&bitmap_info,
		DIB_RGB_COLORS, (void **)&bits, NULL, 0);
	ReleaseDC(NULL, screen_dc);
	if (bitmap == NULL) {
		fprintf(stderr, "could not create the scratch DIB section\n");
		return 4;
	}

	HBITMAP old_bitmap = (HBITMAP)SelectObject(dc, bitmap);

	printf("{\n");
	printf("  \"font_file\": \"");
	Print_Escaped(font_file);
	printf("\",\n");
	printf("  \"face_requested\": \"");
	Print_Escaped(face_name);
	printf("\",\n");
	printf("  \"first_char\": %d,\n", FIRST_CHAR);
	printf("  \"last_char\": %d,\n", LAST_CHAR);
	printf("  \"cases\": [\n");

	int case_index = 0;
	for (; FONT_CASES[case_index].PointSize != 0; ++case_index) {
		const FontCase & font_case = FONT_CASES[case_index];

		// Create_GDI_Font()'s own arithmetic, kept identical on purpose.
		int font_height = -MulDiv(font_case.PointSize, 96, 72);
		int font_width = 0;
		if (font_case.Condensed != 0) {
			font_width = (int)(-font_height * 0.40f);
		}

		HFONT font = CreateFont(font_height, font_width, 0, 0,
			font_case.Bold != 0 ? FW_BOLD : FW_NORMAL, 0,
			FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
			CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, face_name);
		if (font == NULL) {
			fprintf(stderr, "CreateFont failed for %s at %d points\n", face_name,
				font_case.PointSize);
			return 5;
		}

		HFONT old_font = (HFONT)SelectObject(dc, font);

		TEXTMETRIC text_metric;
		memset(&text_metric, 0, sizeof(text_metric));
		GetTextMetrics(dc, &text_metric);

		char actual_face[128];
		memset(actual_face, 0, sizeof(actual_face));
#ifdef _WIN32
		GetTextFaceA(dc, sizeof(actual_face) - 1, actual_face);
#else
		strncpy(actual_face, face_name, sizeof(actual_face) - 1);
#endif

		printf("    {\n");
		printf("      \"point_size\": %d,\n", font_case.PointSize);
		printf("      \"bold\": %s,\n", font_case.Bold != 0 ? "true" : "false");
		printf("      \"condensed\": %s,\n", font_case.Condensed != 0 ? "true" : "false");
		printf("      \"lf_height\": %d,\n", font_height);
		printf("      \"lf_width\": %d,\n", font_width);
		printf("      \"face_actual\": \"");
		Print_Escaped(actual_face);
		printf("\",\n");
		printf("      \"tm_height\": %ld,\n", (long)text_metric.tmHeight);
		printf("      \"tm_ascent\": %ld,\n", (long)text_metric.tmAscent);
		printf("      \"tm_descent\": %ld,\n", (long)text_metric.tmDescent);
		printf("      \"tm_internal_leading\": %ld,\n", (long)text_metric.tmInternalLeading);
		printf("      \"tm_external_leading\": %ld,\n", (long)text_metric.tmExternalLeading);
		printf("      \"tm_ave_char_width\": %ld,\n", (long)text_metric.tmAveCharWidth);
		printf("      \"tm_max_char_width\": %ld,\n", (long)text_metric.tmMaxCharWidth);
		printf("      \"tm_overhang\": %ld,\n", (long)text_metric.tmOverhang);
		printf("      \"advances\": [");

		for (int code_point = FIRST_CHAR; code_point <= LAST_CHAR; ++code_point) {
			WCHAR character = (WCHAR)code_point;
			SIZE extent;
			extent.cx = 0;
			extent.cy = 0;
			GetTextExtentPoint32W(dc, &character, 1, &extent);

			if (code_point > FIRST_CHAR) {
				printf(", ");
			}
			printf("%ld", (long)extent.cx);
		}

		printf("]\n");
		printf("    }%s\n", FONT_CASES[case_index + 1].PointSize != 0 ? "," : "");

		SelectObject(dc, old_font);
		DeleteObject(font);
	}

	printf("  ]\n");
	printf("}\n");

	SelectObject(dc, old_bitmap);
	DeleteObject(bitmap);
	DeleteDC(dc);
	return 0;
}
