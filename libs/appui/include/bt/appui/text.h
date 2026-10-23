// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Free text someone typed or chose (a memo, a name, an asset's name or ticker) that a trusted dialog shows: it must read
// as what it is. Refused: text that is not valid UTF-8, and any code point of general category Cc (controls: a line
// break would add lines to the dialog), Cf (format: bidi marks turn the text, zero-width ones hide parts of it), Zl, Zp,
// Co (private use), Cs (surrogates), Cn (unassigned), or Default_Ignorable_Code_Point (fillers and variation selectors
// that draw nothing), as the Unicode Character Database of DISPLAY_TEXT_UNICODE says them (display_text.inc, made by
// tools/unicode/gen_display_text.py). The pages check the same with their regular expression (app/ui/text.ts); the
// node's check is the one that counts.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bt::appui {

/** "" when `text` may be shown in a trusted dialog, else why not (in English, with the code point). */
std::string display_text_problem(std::string_view text);

/** Text of the chain that someone else chose (a market's title, a decision's question, a swap's L1 address), as a trusted
 *  dialog may show it: itself when display_text_problem takes it, else each refused code point written as <U+XXXX>
 *  (a byte that is not UTF-8 as <0xXX>), so that the dialog shows what is there and nothing turns or hides it. */
std::string display_text_shown(std::string_view text);

/** Whether a code point is one display_text_problem refuses. */
bool display_text_refused(uint32_t code_point);

/** The Unicode version of the table. */
const char* display_text_unicode_version();

} // namespace bt::appui
