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

// regexpr.cpp

#include "always.h"
#include "regexpr.h"
#include "wwstring.h"
#include <assert.h>
#include <regex.h>
#include <string.h>


// This class used to call the GNU regex extensions -- `re_set_syntax`, `re_compile_pattern`,
// `re_match` and the `RE_*` syntax bits -- through the vendored "gnu_regex.h". That API is glibc's
// own extension set and exists nowhere else: the BSD C library on macOS has the POSIX subset only,
// which is why this was the single translation unit out of 977 that would not compile on Apple
// Silicon (`unknown type name 'reg_syntax_t'`).
//
// The POSIX subset is the language this class was already asking for. The nine syntax bits the
// original selected -- RE_CHAR_CLASSES, RE_CONTEXT_INDEP_ANCHORS, RE_CONTEXT_INDEP_OPS,
// RE_CONTEXT_INVALID_OPS, RE_INTERVALS, RE_NO_BK_BRACES, RE_NO_BK_PARENS, RE_NO_BK_VBAR,
// RE_NO_EMPTY_RANGES -- are nine of the twelve bits in glibc's own RE_SYNTAX_POSIX_EXTENDED
// (`/usr/include/regex.h`), which is what `regcomp` selects for REG_EXTENDED. The three it adds are
// named here, because "equivalent" without the differences written down is how a silent behaviour
// change gets shipped:
//
//   RE_DOT_NEWLINE               `.` did not match a newline; now it does.
//   RE_DOT_NOT_NULL              `.` could match a NUL; now it does not.
//   RE_UNMATCHED_RIGHT_PAREN_ORD an unmatched `)` was invalid; now it is an ordinary character, so
//                                Compile() returns true where it used to return false.
//
// All three are reachable only by a pattern or a subject containing a newline, an embedded NUL, or an
// unbalanced `)`. Everything else -- classes, intervals, alternation, grouping, anchors -- is the
// same language, so this is a translation of the file rather than an approximation of it.
//
// One difference does not follow from the syntax bits at all and would have been silent: `re_match`
// anchors at the start of the subject and returns how far it got, while `regexec` searches anywhere
// in it, so a bare `regexec` would report "abc" as matching "xxabc". The match offset is required to
// be 0 below, which is what `re_match` meant. `Match()` still answers "does this expression match
// from the beginning of the string".
#define OUR_COMPILE_FLAGS	REG_EXTENDED


/*
** Definition of private DataStruct for RegularExpressionClass
*/

struct RegularExpressionClass::DataStruct
{
	DataStruct ()
	:	IsValid(false)
	{
		// Blank out the expression structure.
		memset(&CompiledExpr, 0, sizeof(CompiledExpr));
	}

	~DataStruct ()
	{
		ClearExpression();
	}

	void ClearExpression ()
	{
		// If the expression was valid, let the regex library
		// deallocate any memory it had allocated for it.
		if (IsValid)
			regfree(&CompiledExpr);

		// Blank out the expression structure.
		memset(&CompiledExpr, 0, sizeof(CompiledExpr));

		// Erase the expression string.
		ExprString = "";

		// No longer a valid compiled expression.
		IsValid = false;
	}


	// The regular expression that has been compiled.
	StringClass	ExprString;

	// The library's compiled version of the regular expression used
	// during matching or any form of evaluation
	regex_t		CompiledExpr;

	// True if CompiledExpr is valid.
	bool			IsValid;
};



/*
** RegularExpressionClass Implementation
*/

RegularExpressionClass::RegularExpressionClass (const char *expression)
:	Data(0)
{
	// Allocate our private members.
	Data = new DataStruct;
	assert(Data);

	// Compile the expression if we were given one.
	if (expression)
		Compile(expression);
}


RegularExpressionClass::RegularExpressionClass (const RegularExpressionClass &copy)
:	Data(0)
{
	// Allocate our private members.
	Data = new DataStruct;
	assert(Data);

	// Compile the expression if the given object had one.
	if (copy.Is_Valid())
	{
		Compile(copy.Data->ExprString);
		assert(Is_Valid());
	}
}


RegularExpressionClass::~RegularExpressionClass ()
{
	delete Data;
	Data = 0;
}


bool RegularExpressionClass::Compile (const char *expression)
{
	assert(Data);
	assert(expression);

	// Clear any existing expression data. This makes it safe to
	// call Compile() twice on one object.
	Data->ClearExpression();

	// Compile the given expression in the syntax this class uses.
	int error = regcomp(&Data->CompiledExpr, expression, OUR_COMPILE_FLAGS);

	// If the library reported no error, the expression was good!
	if (error == 0)
	{
		Data->IsValid = true;
		Data->ExprString = expression;
		return true;
	}

	// A failed regcomp() leaves nothing to free, but it may have written into
	// the structure, so it goes back to the blanked-out state a default-
	// constructed DataStruct has.
	memset(&Data->CompiledExpr, 0, sizeof(Data->CompiledExpr));
	return false;
}


bool RegularExpressionClass::Is_Valid () const
{
	assert(Data);
	return Data->IsValid;
}


bool RegularExpressionClass::Match (const char *string) const
{
	assert(Data);

	// If we have no valid compiled expression, we can't match Jack.
	if (!Data->IsValid)
		return false;

	// Try to match the given string against our regular expression. regexec()
	// searches the whole subject, so where the match starts has to be asked
	// for: this class matches from the beginning of the string, and a match of
	// 0 characters there is valid and distinctly different from no match.
	regmatch_t match;
	if (regexec(&Data->CompiledExpr, string, 1, &match, 0) != 0)
		return false;

	// The string matched, but only a match beginning where the string does is
	// one this class reports.
	return match.rm_so == 0;
}


/*
** Operators
*/

RegularExpressionClass & RegularExpressionClass::operator = (const RegularExpressionClass &rhs)
{
	// Check for assignment to self.
	if (*this == rhs)
		return *this;

	// Assign that object to this one.
	assert(rhs.Data);
	Compile(rhs.Data->ExprString);
	assert(Is_Valid());

	// Return this object.
	return *this;
}


bool RegularExpressionClass::operator == (const RegularExpressionClass &rhs) const
{
	// Two RegularExpressionClass objects are equivalent if they both
	// have the same validity state, and if that state is 'true' both
	// of their expressions are the same.

	// Check validity states for equality.
	if (Is_Valid() != rhs.Is_Valid())
		return false;

	// If they're valid, check their expressions.
	if (Is_Valid())
	{
		// The objects are not equivalent if their expression strings
		// don't match.
		if (Data->ExprString != rhs.Data->ExprString)
			return false;
	}
	return true;
}


inline bool RegularExpressionClass::operator != (const RegularExpressionClass &rhs) const
{
	return !(*this == rhs);
}


