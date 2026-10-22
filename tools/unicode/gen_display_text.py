#!/usr/bin/env python3
# Copyright (c) 2026 The Backbone developers
# Distributed under the MIT software license.
"""Makes libs/appui/src/display_text.inc: the code points a free text shown in a trusted dialog may not hold
(bt::appui::display_text_problem): general category Cc, Cf, Zl, Zp, Co, Cs or Cn, or Default_Ignorable_Code_Point,
from the Unicode Character Database of one version (UNICODE_VERSION), as sorted, merged ranges.

    gen_display_text.py [UCD folder, default /usr/share/unicode (Debian's unicode-data)] > libs/appui/src/display_text.inc

Reads extracted/DerivedGeneralCategory.txt (it lists Cn) and DerivedCoreProperties.txt; refuses another version.
"""

import re
import sys
from pathlib import Path

UNICODE_VERSION = "15.1.0"
CATEGORIES = {"Cc", "Cf", "Zl", "Zp", "Co", "Cs", "Cn"}


def ranges(path, want):
    """(first, last) of every line whose property value is in `want`; the file's version checked."""
    text = path.read_text(encoding="utf-8")
    m = re.search(r"^# \S+-(\d+\.\d+\.\d+)\.txt", text, re.M)
    if not m or m.group(1) != UNICODE_VERSION:
        sys.exit(f"{path}: Unicode {m.group(1) if m else '?'}, this table is of {UNICODE_VERSION}")
    out = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cps, prop = (x.strip() for x in line.split(";")[:2])
        if prop not in want:
            continue
        first, _, last = cps.partition("..")
        out.append((int(first, 16), int(last or first, 16)))
    return out


def main():
    ucd = Path(sys.argv[1] if len(sys.argv) > 1 else "/usr/share/unicode")
    all_ranges = ranges(ucd / "extracted" / "DerivedGeneralCategory.txt", CATEGORIES)
    all_ranges += ranges(ucd / "DerivedCoreProperties.txt", {"Default_Ignorable_Code_Point"})
    merged = []
    for first, last in sorted(all_ranges):
        if merged and first <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], last)
        else:
            merged.append([first, last])
    print(f"// Made by tools/unicode/gen_display_text.py from the Unicode Character Database {UNICODE_VERSION}: the code points")
    print("// of general category Cc, Cf, Zl, Zp, Co, Cs, Cn and Default_Ignorable_Code_Point, as merged ranges. Do not edit.")
    print(f'inline constexpr const char* DISPLAY_TEXT_UNICODE{{"{UNICODE_VERSION}"}};')
    print("inline constexpr std::pair<uint32_t, uint32_t> DISPLAY_TEXT_REFUSED[]{")
    for i in range(0, len(merged), 4):
        print("    " + " ".join(f"{{0x{a:04X}, 0x{b:04X}}}," for a, b in merged[i:i + 4]))
    print("};")


if __name__ == "__main__":
    main()
