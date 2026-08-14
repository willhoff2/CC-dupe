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
 *  The GDI text half of the Win32 compatibility layer: the handful of wingdi.h entry points     *
 *  WW3D2/render2dsentence.cpp's FontCharsClass rasterises its glyph cache with -- CreateFont,   *
 *  CreateCompatibleDC, CreateDIBSection, SelectObject, SetBkColor, SetTextColor,                *
 *  GetTextMetrics, GetTextExtentPoint32W and ExtTextOutW. As with the file/locale layer the      *
 *  definitions are under the Win32 names themselves, so that FontCharsClass compiles and links  *
 *  off Windows without a single #ifdef in it (docs/porting/gdi-font-seam.md).                   *
 *                                                                                             *
 *  One rasteriser on every non-Windows platform, not CoreText on macOS and something else on    *
 *  Linux: there is no GDI to match off Windows, so what is matched is GDI's *metrics*, and       *
 *  those have to come out the same on both platforms or the WND GUI's authored layout numbers    *
 *  land in different places (docs/porting/decisions-resolved.md). The rasteriser is             *
 *  stb_truetype, which the build already vendors for screenshot writing.                        *
 *                                                                                             *
 *  What is modelled, because FontCharsClass consumes it:                                       *
 *                                                                                             *
 *    - a font is an em size in device pixels. GDI's negative lfHeight is "em height in device    *
 *      units", which is what Create_GDI_Font() passes; a positive one is cell height and is      *
 *      matched against ascent+descent.                                                        *
 *    - lfWidth, which Create_GDI_Font() sets for the "Generals" font, is GDI's "make            *
 *      tmAveCharWidth this many pixels" request: it turns into a horizontal scale.              *
 *    - integer advances. GDI rounds each glyph's advance at the ppem and sums those, so         *
 *      GetTextExtentPoint32W() of one character is the number the layout code stores as the     *
 *      character width. Everything is compared against recorded GDI numbers by                  *
 *      scripts/ci/check-font-metrics.py.                                                       *
 *    - a top-down 24bpp DIB section with one glyph drawn into it per call, TA_TOP|TA_LEFT, the   *
 *      background painted for ETO_OPAQUE. The caller reads one byte per pixel out of it and      *
 *      keeps the top 4 bits as alpha.                                                           *
 *                                                                                             *
 *  What is deliberately not modelled: clipping regions, transforms, text alignment other than    *
 *  the default, ClearType/sub-pixel output (coverage is grey, as ANTIALIASED_QUALITY asks for),  *
 *  kerning, synthetic italic, and every GDI object other than a font and a DIB section. The      *
 *  entry points that would need them are not in the engine's font path.                         *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "WWLib/platform/platform_win32_compat.h"

#ifdef WWPLATFORM_WIN32_COMPAT

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

namespace
{

/*
**	Every GDI object this layer hands out is one of these, so that SelectObject() -- which takes a
**	void* HGDIOBJ and is given an HFONT or an HBITMAP -- can tell them apart, and so that a handle
**	from somewhere else is refused rather than dereferenced.
*/
enum GDIObjectKind
{
	GDI_KIND_FONT = 0x464F4E54,		// 'FONT'
	GDI_KIND_BITMAP = 0x424D4150,	// 'BMAP'
	GDI_KIND_DC = 0x4D454D44		// 'MEMD'
};

struct GDIObject
{
	GDIObject(GDIObjectKind kind) : Kind(kind) {}
	GDIObjectKind Kind;
};

/*
**	A font: the file's bytes, stb_truetype's view of them, and the metrics GetTextMetrics()
**	reports. The scales are pixels per font unit, separately per axis because lfWidth condenses.
*/
struct GDIFontObject : public GDIObject
{
	GDIFontObject() :
		GDIObject(GDI_KIND_FONT),
		ScaleX(0.0f),
		ScaleY(0.0f),
		EmPixels(0),
		Height(0),
		Ascent(0),
		Descent(0),
		InternalLeading(0),
		ExternalLeading(0),
		AveCharWidth(0),
		MaxCharWidth(0),
		Weight(FW_NORMAL),
		Italic(0),
		CondenseNumerator(0),
		CondenseDenominator(0)
	{
		memset(&Info, 0, sizeof(Info));
	}

	std::vector<unsigned char> Data;
	stbtt_fontinfo Info;
	std::string FaceName;
	std::string FilePath;
	float ScaleX;
	float ScaleY;
	int EmPixels;
	int Height;
	int Ascent;
	int Descent;
	int InternalLeading;
	int ExternalLeading;
	int AveCharWidth;
	int MaxCharWidth;
	int Weight;
	int Italic;
	// A non-zero lfWidth is a fraction, not a scale factor, because GDI applies it to the whole
	// pixel advances and rounds the result up. Kept as the two integers so the advance for one
	// character is the same arithmetic GDI does (see Advance_Of).
	int CondenseNumerator;
	int CondenseDenominator;
};

/*
**	A DIB section: the caller was handed the bit pointer, so the bits have to outlive every copy
**	of it until DeleteObject(). Top-down (negative biHeight) is what the font cache asks for and
**	the only orientation modelled; 24bpp likewise.
*/
struct GDIBitmapObject : public GDIObject
{
	GDIBitmapObject() :
		GDIObject(GDI_KIND_BITMAP),
		Width(0),
		Height(0),
		Stride(0),
		BitsPerPixel(24),
		Bits(nullptr)
	{
	}

	~GDIBitmapObject() { free(Bits); }

	int Width;
	int Height;
	int Stride;
	int BitsPerPixel;
	unsigned char * Bits;
};

struct GDIDeviceContext : public GDIObject
{
	GDIDeviceContext() :
		GDIObject(GDI_KIND_DC),
		Font(nullptr),
		Bitmap(nullptr),
		BkColor(RGB(255, 255, 255)),
		TextColor(RGB(0, 0, 0))
	{
	}

	GDIFontObject * Font;
	GDIBitmapObject * Bitmap;
	COLORREF BkColor;
	COLORREF TextColor;
};

GDIObject * Object_From_Handle(void * handle, GDIObjectKind kind)
{
	if (handle == nullptr) {
		return nullptr;
	}

	GDIObject * object = (GDIObject *)handle;
	if (object->Kind != kind) {
		return nullptr;
	}

	return object;
}

GDIDeviceContext * DC_From_Handle(HDC dc)
{
	return (GDIDeviceContext *)Object_From_Handle((void *)dc, GDI_KIND_DC);
}

/*
**	The screen DC GetDC(nullptr-or-window) hands back. GDI's screen DC is a real drawing surface;
**	here it exists only because CreateCompatibleDC() and CreateDIBSection() are handed one, and
**	neither of them reads anything out of it. Nothing draws through it, so one shared instance is
**	enough and it is never freed.
*/
GDIDeviceContext * Screen_DC()
{
	static GDIDeviceContext screen;
	return &screen;
}

int Round_To_Int(float value)
{
	return (int)floorf(value + 0.5f);
}

/*
**	Case insensitive compare, ASCII only: face names are ASCII and the C library's
**	strcasecmp/_stricmp split is not worth a platform check.
*/
bool Same_Name(const char * left, const char * right)
{
	while (*left != 0 && *right != 0) {
		if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
			return false;
		}
		++left;
		++right;
	}

	return *left == *right;
}

/***********************************************************************************************
 *                                                                                             *
 *  Finding a font file for a GDI face name.                                                   *
 *                                                                                             *
 *  GDI matches a face name against installed fonts. Here the mapping is a table, because what   *
 *  the engine asks for is a short and fixed list -- Arial (Create_GDI_Font() already rewrites    *
 *  "Generals" to it), Times New Roman and Courier New -- and because the substitutes have to be  *
 *  the metric-compatible ones or the layout numbers move: Liberation Sans/Serif/Mono are         *
 *  designed to have Arial's, Times New Roman's and Courier New's advance widths.                *
 *                                                                                             *
 *  WW_FONT_PATH overrides the search directories, colon separated, so a build can point at a     *
 *  pinned font directory; scripts/ci/check-font-metrics.py uses it to compare against the        *
 *  recorded numbers with the same file the reference was recorded with.                          *
 *                                                                                             *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

struct FaceSubstitution
{
	const char * Face;
	const char * Regular;
	const char * Bold;
};

const FaceSubstitution FACE_SUBSTITUTIONS[] =
{
	{ "Arial",           "Arial.ttf",             "Arial_Bold.ttf" },
	{ "Arial",           "arial.ttf",             "arialbd.ttf" },
	{ "Arial",           "LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf" },
	{ "Arial",           "Helvetica.ttc",         "Helvetica.ttc" },
	{ "Arial",           "DejaVuSans.ttf",        "DejaVuSans-Bold.ttf" },
	{ "Times New Roman", "times.ttf",             "timesbd.ttf" },
	{ "Times New Roman", "LiberationSerif-Regular.ttf", "LiberationSerif-Bold.ttf" },
	{ "Times New Roman", "DejaVuSerif.ttf",       "DejaVuSerif-Bold.ttf" },
	{ "Courier New",     "cour.ttf",              "courbd.ttf" },
	{ "Courier New",     "LiberationMono-Regular.ttf", "LiberationMono-Bold.ttf" },
	{ "Courier New",     "DejaVuSansMono.ttf",    "DejaVuSansMono-Bold.ttf" },
	{ nullptr, nullptr, nullptr }
};

/*
**	Where a face name that is not in the table above still gets a file: the same list of
**	substitutes as Arial, because a sans serif face is the better guess for a UI font.
*/
const FaceSubstitution * Default_Substitutions()
{
	return FACE_SUBSTITUTIONS;
}

const char * const FONT_DIRECTORIES[] =
{
	// The game's own directory first: a shipped font is the one the retail build would have
	// installed and used.
	"Data/Fonts",
	"Fonts",
#if defined(__APPLE__)
	"/System/Library/Fonts",
	"/System/Library/Fonts/Supplemental",
	"/Library/Fonts",
#endif
	"/usr/share/fonts",
	"/usr/local/share/fonts",
	"/usr/share/fonts/truetype",
	nullptr
};

bool File_Exists(const std::string & path)
{
	FILE * handle = fopen(path.c_str(), "rb");
	if (handle == nullptr) {
		return false;
	}

	fclose(handle);
	return true;
}

/*
**	One directory, one filename, recursively: system font directories are a tree
**	(/usr/share/fonts/truetype/liberation/...), and the caller only knows the file name.
*/
bool Find_In_Directory(const std::string & directory, const char * name, int depth,
	std::string & result)
{
	if (name == nullptr || name[0] == 0) {
		return false;
	}

	std::string direct = directory + "/" + name;
	if (File_Exists(direct)) {
		result = direct;
		return true;
	}

	if (depth <= 0) {
		return false;
	}

	DIR * handle = opendir(directory.c_str());
	if (handle == nullptr) {
		return false;
	}

	bool found = false;
	struct dirent * entry = nullptr;
	while (!found && (entry = readdir(handle)) != nullptr) {
		if (entry->d_name[0] == '.') {
			continue;
		}

		std::string child = directory + "/" + entry->d_name;
		struct stat details;
		if (stat(child.c_str(), &details) != 0) {
			continue;
		}

		if (S_ISDIR(details.st_mode)) {
			found = Find_In_Directory(child, name, depth - 1, result);
		} else if (Same_Name(entry->d_name, name)) {
			result = child;
			found = true;
		}
	}

	closedir(handle);
	return found;
}

bool Find_Font_File(const char * name, std::string & result)
{
	const char * override_path = getenv("WW_FONT_PATH");
	if (override_path != nullptr && override_path[0] != 0) {
		std::string list(override_path);
		std::string::size_type start = 0;
		while (start <= list.size()) {
			std::string::size_type end = list.find(':', start);
			if (end == std::string::npos) {
				end = list.size();
			}
			std::string directory = list.substr(start, end - start);
			if (!directory.empty() && Find_In_Directory(directory, name, 4, result)) {
				return true;
			}
			start = end + 1;
		}

		// An explicit search path is an instruction, not a hint: falling back to the system
		// fonts would silently measure a different file than the caller asked for.
		return false;
	}

	for (int index = 0; FONT_DIRECTORIES[index] != nullptr; ++index) {
		if (Find_In_Directory(FONT_DIRECTORIES[index], name, 4, result)) {
			return true;
		}
	}

	const char * home = getenv("HOME");
	if (home != nullptr) {
		static const char * const HOME_DIRECTORIES[] =
		{
#if defined(__APPLE__)
			"/Library/Fonts",
#endif
			"/.fonts",
			"/.local/share/fonts",
			nullptr
		};

		for (int index = 0; HOME_DIRECTORIES[index] != nullptr; ++index) {
			std::string directory(home);
			directory += HOME_DIRECTORIES[index];
			if (Find_In_Directory(directory, name, 4, result)) {
				return true;
			}
		}
	}

	return false;
}

bool Load_File(const std::string & path, std::vector<unsigned char> & result)
{
	FILE * handle = fopen(path.c_str(), "rb");
	if (handle == nullptr) {
		return false;
	}

	fseek(handle, 0, SEEK_END);
	long length = ftell(handle);
	fseek(handle, 0, SEEK_SET);
	if (length <= 0) {
		fclose(handle);
		return false;
	}

	result.resize((size_t)length);
	size_t read = fread(&result[0], 1, (size_t)length, handle);
	fclose(handle);
	return read == (size_t)length;
}

/*
**	The file for one face name and weight, tried in the table's order so that a real Arial beats
**	a metric-compatible substitute for it.
*/
bool Open_Face(const char * face_name, bool bold, std::vector<unsigned char> & data,
	std::string & path)
{
	for (int pass = 0; pass < 2; ++pass) {
		const FaceSubstitution * candidates =
			(pass == 0) ? FACE_SUBSTITUTIONS : Default_Substitutions();

		for (int index = 0; candidates[index].Face != nullptr; ++index) {
			if (pass == 0 && !Same_Name(candidates[index].Face, face_name)) {
				continue;
			}

			const char * name = bold ? candidates[index].Bold : candidates[index].Regular;
			if (Find_Font_File(name, path) && Load_File(path, data)) {
				return true;
			}

			// A bold face that is not installed falls back to the regular one. GDI would
			// synthesise the emboldening; that is an open item in the seam's doc, and the
			// advances are the regular face's either way.
			if (bold && Find_Font_File(candidates[index].Regular, path) &&
				Load_File(path, data)) {
				return true;
			}
		}
	}

	return false;
}

/*
**	Big endian reads, for the two font tables stb_truetype does not surface. TrueType is big
**	endian everywhere.
*/
unsigned int Read_U16(const unsigned char * data)
{
	return ((unsigned int)data[0] << 8) | (unsigned int)data[1];
}

unsigned int Read_U32(const unsigned char * data)
{
	return (Read_U16(data) << 16) | Read_U16(data + 2);
}

/*
**	OS/2's xAvgCharWidth, in font units, which is where GDI's tmAveCharWidth comes from -- not the
**	advance of 'x', which is a different number for most faces. It matters beyond the reported
**	metric because lfWidth is a request about it. Zero if the face has no OS/2 table.
*/
int Design_Ave_Char_Width(const std::vector<unsigned char> & data)
{
	if (data.size() < 12) {
		return 0;
	}

	unsigned int base = (unsigned int)stbtt_GetFontOffsetForIndex(&data[0], 0);
	if (base + 12 > data.size()) {
		return 0;
	}

	unsigned int table_count = Read_U16(&data[base + 4]);
	for (unsigned int index = 0; index < table_count; ++index) {
		size_t record = (size_t)base + 12 + ((size_t)index * 16);
		if (record + 16 > data.size()) {
			break;
		}

		if (memcmp(&data[record], "OS/2", 4) == 0) {
			size_t table = (size_t)Read_U32(&data[record + 8]);
			if (table + 4 > data.size()) {
				return 0;
			}

			return (int)(short)(unsigned short)Read_U16(&data[table + 2]);
		}
	}

	return 0;
}

/*
**	The advance width of one code point, in whole pixels, the way GDI reports it: the design
**	advance scaled to the ppem and rounded, per glyph, with no kerning.
**
**	A condensed face -- a non-zero lfWidth -- is GDI's own second step on top of that: the whole
**	pixel advance times lfWidth over the natural tmAveCharWidth, rounded *up*. Rounding up rather
**	than to nearest is not a guess; it is what the recorded GDI advances show, and it is the
**	difference between the "Generals" font's menu labels fitting and not
**	(scripts/ci/check-font-metrics.py).
*/
int Advance_Of(const GDIFontObject & font, int code_point)
{
	int glyph = stbtt_FindGlyphIndex(&font.Info, code_point);
	int advance = 0;
	int bearing = 0;
	stbtt_GetGlyphHMetrics(&font.Info, glyph, &advance, &bearing);
	int pixels = Round_To_Int((float)advance * font.ScaleY);

	if (font.CondenseDenominator > 0) {
		int numerator = pixels * font.CondenseNumerator;
		pixels = (numerator + font.CondenseDenominator - 1) / font.CondenseDenominator;
	}

	return pixels;
}

}	// namespace

extern "C" {

/***********************************************************************************************
 * CreateFontA -- open a font file for a face name and size                                     *
 *                                                                                             *
 *  A null return is a font GDI could not create, which Create_GDI_Font() already reports as a   *
 *  failure. That is what happens when no font file is installed at all: the alternative --      *
 *  drawing nothing and reporting success -- would show empty menus with no explanation.        *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
HFONT CreateFontA(int height, int width, int escapement, int orientation, int weight,
	DWORD italic, DWORD underline, DWORD strike_out, DWORD char_set, DWORD out_precision,
	DWORD clip_precision, DWORD quality, DWORD pitch_and_family, LPCSTR face_name)
{
	(void)escapement;
	(void)orientation;
	(void)underline;
	(void)strike_out;
	(void)char_set;
	(void)out_precision;
	(void)clip_precision;
	(void)quality;
	(void)pitch_and_family;

	const char * face = (face_name != nullptr) ? face_name : "Arial";
	bool bold = (weight >= FW_BOLD);

	GDIFontObject * font = new GDIFontObject;
	font->FaceName = face;
	font->Weight = (weight == 0) ? FW_NORMAL : weight;
	font->Italic = (italic != 0) ? 1 : 0;

	if (!Open_Face(face, bold, font->Data, font->FilePath)) {
		WWPlatform::Win32::Report_Stub("CreateFont", face);
		delete font;
		return nullptr;
	}

	if (stbtt_InitFont(&font->Info, &font->Data[0],
			stbtt_GetFontOffsetForIndex(&font->Data[0], 0)) == 0) {
		WWPlatform::Win32::Report_Stub("CreateFont", font->FilePath.c_str());
		delete font;
		return nullptr;
	}

	int ascent = 0;
	int descent = 0;
	int line_gap = 0;
	stbtt_GetFontVMetrics(&font->Info, &ascent, &descent, &line_gap);

	/*
	**	lfHeight < 0 is "the em square is this many device pixels", which is what
	**	Create_GDI_Font() asks for; lfHeight > 0 is "ascent + descent is this many pixels";
	**	lfHeight == 0 means a default size, and GDI's default is 12 points at 96 dpi.
	*/
	float per_unit = stbtt_ScaleForMappingEmToPixels(&font->Info, 1.0f);
	int units_per_em = (per_unit > 0.0f) ? Round_To_Int(1.0f / per_unit) : 2048;

	float scale = 0.0f;
	if (height < 0) {
		font->EmPixels = -height;
		scale = stbtt_ScaleForMappingEmToPixels(&font->Info, (float)font->EmPixels);
	} else {
		int cell = (height == 0) ? 16 : height;
		scale = stbtt_ScaleForPixelHeight(&font->Info, (float)cell);
		font->EmPixels = Round_To_Int((float)units_per_em * scale);
	}

	font->ScaleY = scale;
	font->ScaleX = scale;

	font->Ascent = Round_To_Int((float)ascent * scale);
	font->Descent = Round_To_Int((float)-descent * scale);
	font->Height = font->Ascent + font->Descent;
	font->InternalLeading = font->Height - font->EmPixels;
	if (font->InternalLeading < 0) {
		font->InternalLeading = 0;
	}
	font->ExternalLeading = Round_To_Int((float)line_gap * scale);
	if (font->ExternalLeading < 0) {
		font->ExternalLeading = 0;
	}

	/*
	**	A non-zero lfWidth asks for a face whose average character width is that many pixels, and
	**	Create_GDI_Font() uses it for the "Generals" font: the tighter advances it produces are
	**	what the menu layout numbers were authored against. The ratio is against the natural
	**	tmAveCharWidth, in whole pixels, as GDI computes it.
	*/
	int natural_ave = Round_To_Int((float)Design_Ave_Char_Width(font->Data) * scale);
	if (natural_ave <= 0) {
		natural_ave = Advance_Of(*font, 'x');
	}

	font->AveCharWidth = natural_ave;
	if (width != 0 && natural_ave > 0) {
		font->CondenseNumerator = abs(width);
		font->CondenseDenominator = natural_ave;
		font->ScaleX = scale * ((float)font->CondenseNumerator / (float)natural_ave);
		font->AveCharWidth = abs(width);
	}

	font->MaxCharWidth = 0;
	for (int code_point = 32; code_point < 127; ++code_point) {
		int advance = Advance_Of(*font, code_point);
		if (advance > font->MaxCharWidth) {
			font->MaxCharWidth = advance;
		}
	}

	return (HFONT)font;
}

/***********************************************************************************************
 * CreateCompatibleDC -- a memory DC to select a font and a DIB section into                    *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
HDC CreateCompatibleDC(HDC reference)
{
	(void)reference;
	return (HDC)(new GDIDeviceContext);
}

BOOL DeleteDC(HDC dc)
{
	GDIDeviceContext * context = DC_From_Handle(dc);
	if (context == nullptr || context == Screen_DC()) {
		return FALSE;
	}

	delete context;
	return TRUE;
}

HDC GetDC(HWND window)
{
	(void)window;
	return (HDC)Screen_DC();
}

int ReleaseDC(HWND window, HDC dc)
{
	(void)window;
	(void)dc;
	return 1;
}

/***********************************************************************************************
 * CreateDIBSection -- a bitmap whose bits the caller reads directly                            *
 *                                                                                             *
 *  Only the shape the font cache asks for: 24bpp BI_RGB, top-down (negative biHeight). Anything *
 *  else is refused rather than half drawn into.                                                *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO * info, UINT usage, void ** bits,
	HANDLE section, DWORD offset)
{
	(void)dc;
	(void)usage;

	if (info == nullptr || bits == nullptr || section != nullptr || offset != 0) {
		return nullptr;
	}

	const BITMAPINFOHEADER & header = info->bmiHeader;
	if (header.biBitCount != 24 || header.biCompression != BI_RGB || header.biWidth <= 0 ||
		header.biHeight == 0) {
		WWPlatform::Win32::Report_Stub("CreateDIBSection", "only 24bpp BI_RGB is implemented");
		return nullptr;
	}

	GDIBitmapObject * bitmap = new GDIBitmapObject;
	bitmap->Width = (int)header.biWidth;
	bitmap->Height = (int)labs((long)header.biHeight);
	bitmap->BitsPerPixel = 24;
	bitmap->Stride = ((bitmap->Width * 3) + 3) & ~3;
	bitmap->Bits = (unsigned char *)calloc((size_t)bitmap->Stride * (size_t)bitmap->Height, 1);
	if (bitmap->Bits == nullptr) {
		delete bitmap;
		return nullptr;
	}

	*bits = bitmap->Bits;
	return (HBITMAP)bitmap;
}

/***********************************************************************************************
 * SelectObject / DeleteObject                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
HGDIOBJ SelectObject(HDC dc, HGDIOBJ object)
{
	GDIDeviceContext * context = DC_From_Handle(dc);
	if (context == nullptr || object == nullptr) {
		return nullptr;
	}

	GDIObject * selected = (GDIObject *)object;
	if (selected->Kind == GDI_KIND_FONT) {
		GDIFontObject * previous = context->Font;
		context->Font = (GDIFontObject *)selected;
		return (HGDIOBJ)previous;
	}

	if (selected->Kind == GDI_KIND_BITMAP) {
		GDIBitmapObject * previous = context->Bitmap;
		context->Bitmap = (GDIBitmapObject *)selected;
		return (HGDIOBJ)previous;
	}

	return nullptr;
}

BOOL DeleteObject(HGDIOBJ object)
{
	if (object == nullptr) {
		return FALSE;
	}

	GDIObject * target = (GDIObject *)object;
	if (target->Kind == GDI_KIND_FONT) {
		delete (GDIFontObject *)target;
		return TRUE;
	}

	if (target->Kind == GDI_KIND_BITMAP) {
		delete (GDIBitmapObject *)target;
		return TRUE;
	}

	return FALSE;
}

COLORREF SetBkColor(HDC dc, COLORREF colour)
{
	GDIDeviceContext * context = DC_From_Handle(dc);
	if (context == nullptr) {
		return CLR_INVALID;
	}

	COLORREF previous = context->BkColor;
	context->BkColor = colour;
	return previous;
}

COLORREF SetTextColor(HDC dc, COLORREF colour)
{
	GDIDeviceContext * context = DC_From_Handle(dc);
	if (context == nullptr) {
		return CLR_INVALID;
	}

	COLORREF previous = context->TextColor;
	context->TextColor = colour;
	return previous;
}

/***********************************************************************************************
 * GetTextMetricsA -- the font's vertical metrics, which is where CharHeight comes from          *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
BOOL GetTextMetricsA(HDC dc, LPTEXTMETRICA metrics)
{
	GDIDeviceContext * context = DC_From_Handle(dc);
	if (context == nullptr || metrics == nullptr || context->Font == nullptr) {
		return FALSE;
	}

	const GDIFontObject & font = *context->Font;
	memset(metrics, 0, sizeof(*metrics));
	metrics->tmHeight = font.Height;
	metrics->tmAscent = font.Ascent;
	metrics->tmDescent = font.Descent;
	metrics->tmInternalLeading = font.InternalLeading;
	metrics->tmExternalLeading = font.ExternalLeading;
	metrics->tmAveCharWidth = font.AveCharWidth;
	metrics->tmMaxCharWidth = font.MaxCharWidth;
	metrics->tmWeight = font.Weight;
	// GDI reports no overhang for a TrueType face; the field exists for the raster fonts whose
	// synthetic bold and italic spill outside the advance.
	metrics->tmOverhang = 0;
	metrics->tmDigitizedAspectX = 96;
	metrics->tmDigitizedAspectY = 96;
	metrics->tmFirstChar = 32;
	metrics->tmLastChar = 255;
	metrics->tmDefaultChar = 31;
	metrics->tmBreakChar = 32;
	metrics->tmItalic = (BYTE)font.Italic;
	metrics->tmPitchAndFamily = TMPF_TRUETYPE;
	metrics->tmCharSet = ANSI_CHARSET;
	return TRUE;
}

/***********************************************************************************************
 * GetTextExtentPoint32W -- the advance width the layout code stores as a character's width      *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
BOOL GetTextExtentPoint32W(HDC dc, LPCWSTR text, int count, LPSIZE size)
{
	GDIDeviceContext * context = DC_From_Handle(dc);
	if (context == nullptr || size == nullptr || context->Font == nullptr) {
		return FALSE;
	}

	const GDIFontObject & font = *context->Font;
	int width = 0;
	for (int index = 0; text != nullptr && index < count; ++index) {
		width += Advance_Of(font, (int)text[index]);
	}

	size->cx = width;
	size->cy = font.Height;
	return TRUE;
}

/***********************************************************************************************
 * ExtTextOutW -- draw the glyphs into the selected DIB section                                  *
 *                                                                                             *
 *  TA_TOP|TA_LEFT, i.e. (x, y) is the top left of the cell and the baseline is y + tmAscent,     *
 *  which is the alignment the DC starts out with and the one the font cache relies on. Coverage  *
 *  is blended between the background and the text colour, so the caller reading one byte per     *
 *  pixel gets the glyph's alpha.                                                               *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
BOOL ExtTextOutW(HDC dc, int x, int y, UINT options, const RECT * rect, LPCWSTR text, UINT count,
	const INT * spacing)
{
	(void)spacing;

	GDIDeviceContext * context = DC_From_Handle(dc);
	if (context == nullptr || context->Font == nullptr || context->Bitmap == nullptr) {
		return FALSE;
	}

	const GDIFontObject & font = *context->Font;
	GDIBitmapObject & bitmap = *context->Bitmap;

	unsigned char bk_blue = GetBValue(context->BkColor);
	unsigned char bk_green = GetGValue(context->BkColor);
	unsigned char bk_red = GetRValue(context->BkColor);
	unsigned char text_blue = GetBValue(context->TextColor);
	unsigned char text_green = GetGValue(context->TextColor);
	unsigned char text_red = GetRValue(context->TextColor);

	/*
	**	ETO_OPAQUE paints the rectangle with the background colour first. The font cache passes
	**	the whole scratch surface, and relies on the previous character having been erased.
	*/
	if ((options & ETO_OPAQUE) != 0 && rect != nullptr) {
		int left = (int)rect->left < 0 ? 0 : (int)rect->left;
		int top = (int)rect->top < 0 ? 0 : (int)rect->top;
		int right = (int)rect->right > bitmap.Width ? bitmap.Width : (int)rect->right;
		int bottom = (int)rect->bottom > bitmap.Height ? bitmap.Height : (int)rect->bottom;

		for (int row = top; row < bottom; ++row) {
			unsigned char * line = bitmap.Bits + ((size_t)row * (size_t)bitmap.Stride);
			for (int column = left; column < right; ++column) {
				line[(column * 3) + 0] = bk_blue;
				line[(column * 3) + 1] = bk_green;
				line[(column * 3) + 2] = bk_red;
			}
		}
	}

	int pen_x = x;
	int baseline = y + font.Ascent;

	for (UINT index = 0; text != nullptr && index < count; ++index) {
		int code_point = (int)text[index];
		int glyph = stbtt_FindGlyphIndex(&font.Info, code_point);

		int box_x0 = 0;
		int box_y0 = 0;
		int box_x1 = 0;
		int box_y1 = 0;
		stbtt_GetGlyphBitmapBox(&font.Info, glyph, font.ScaleX, font.ScaleY,
			&box_x0, &box_y0, &box_x1, &box_y1);

		int glyph_width = box_x1 - box_x0;
		int glyph_height = box_y1 - box_y0;
		if (glyph_width > 0 && glyph_height > 0) {
			std::vector<unsigned char> coverage((size_t)glyph_width * (size_t)glyph_height, 0);
			stbtt_MakeGlyphBitmap(&font.Info, &coverage[0], glyph_width, glyph_height,
				glyph_width, font.ScaleX, font.ScaleY, glyph);

			for (int row = 0; row < glyph_height; ++row) {
				int destination_row = baseline + box_y0 + row;
				if (destination_row < 0 || destination_row >= bitmap.Height) {
					continue;
				}

				unsigned char * line =
					bitmap.Bits + ((size_t)destination_row * (size_t)bitmap.Stride);

				for (int column = 0; column < glyph_width; ++column) {
					int destination_column = pen_x + box_x0 + column;
					if (destination_column < 0 || destination_column >= bitmap.Width) {
						continue;
					}

					int alpha = coverage[((size_t)row * (size_t)glyph_width) + (size_t)column];
					if (alpha == 0) {
						continue;
					}

					unsigned char * pixel = line + (destination_column * 3);
					pixel[0] = (unsigned char)(((text_blue * alpha) +
						(pixel[0] * (255 - alpha))) / 255);
					pixel[1] = (unsigned char)(((text_green * alpha) +
						(pixel[1] * (255 - alpha))) / 255);
					pixel[2] = (unsigned char)(((text_red * alpha) +
						(pixel[2] * (255 - alpha))) / 255);
				}
			}
		}

		pen_x += Advance_Of(font, code_point);
	}

	return TRUE;
}

/***********************************************************************************************
 * MulDiv -- (a * b) / c in 64 bits, rounded, as kernel32 does it                                *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
int MulDiv(int number, int numerator, int denominator)
{
	if (denominator == 0) {
		return -1;
	}

	long long product = (long long)number * (long long)numerator;
	long long half = (long long)denominator / 2;
	if ((product < 0) != (denominator < 0)) {
		half = -half;
	}

	long long result = (product + half) / (long long)denominator;
	if (result > 2147483647LL || result < -2147483647LL - 1LL) {
		return -1;
	}

	return (int)result;
}

}	// extern "C"

#endif	// WWPLATFORM_WIN32_COMPAT
