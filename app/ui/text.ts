// Free text a trusted dialog shows (a memo, a name, an asset's name or ticker) must read as what it is: the pages' twin
// of the node's bt::appui::display_text_problem (libs/appui/include/bt/appui/text.h), which is the check that counts.
// Refused: any code point of general category Cc, Cf, Zl, Zp, Co, Cs (a lone surrogate) or Cn, or Default_Ignorable_Code_Point,
// as this JavaScript engine's Unicode says them (the node's table is of one stated version: a character assigned since
// may pass here and be refused there).
const REFUSED = /[\p{Cc}\p{Cf}\p{Zl}\p{Zp}\p{Co}\p{Cs}\p{Cn}\p{Default_Ignorable_Code_Point}]/u;

/** '' when the text may be shown in a trusted dialog, else why not (English, translated where shown). */
export function displayTextProblem(text: string): string {
  const m = REFUSED.exec(text);
  if (!m) return '';
  const cp = m[0].codePointAt(0) ?? 0;
  return `No control, format or invisible character (U+${cp.toString(16).toUpperCase().padStart(4, '0')})`;
}
