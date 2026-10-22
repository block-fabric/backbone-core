// The pages' twin of the node's display_text_problem: the same code points refused (controls, format characters, line
// and paragraph separators, private use, lone surrogates, unassigned, default ignorables), their neighbours taken.
import { describe, expect, it } from 'vitest';
import { displayTextProblem } from './text';

const ch = (cp: number) => String.fromCodePoint(cp);

describe('displayTextProblem', () => {
  it('refuses controls, format, separators, private use, unassigned and default-ignorable code points', () => {
    const refused = [
      0x0000, 0x0009, 0x000a, 0x001f, 0x007f, 0x0080, 0x009f, 0x00ad, 0x034f, 0x061c, 0x115f, 0x1160, 0x17b4, 0x17b5, 0x180b, 0x180c, 0x180d,
      0x180e, 0x180f, 0x200b, 0x200c, 0x200d, 0x200e, 0x200f, 0x2028, 0x2029, 0x202a, 0x202e, 0x2060, 0x2064, 0x2066, 0x2069, 0x3164, 0xfe00,
      0xfe0f, 0xfeff, 0xffa0, 0xe000, 0xf8ff, 0x0378, 0xfffe, 0xffff, 0x1bca0, 0x1d173, 0xe0000, 0xe0001, 0xe007f, 0xe0100, 0xe0fff, 0xf0000,
      0x10fffd, 0x10ffff,
    ];
    for (const cp of refused) expect(displayTextProblem(`a${ch(cp)}b`), cp.toString(16)).not.toBe('');
    expect(displayTextProblem('x\u202ey')).toBe('No control, format or invisible character (U+202E)');
    // A lone surrogate (what a JavaScript string can hold and UTF-8 cannot).
    expect(displayTextProblem('a\ud800b')).not.toBe('');
  });

  it('takes their neighbours, spaces, letters, marks, symbols and emoji', () => {
    const taken = [
      0x0020, 0x007e, 0x00a0, 0x00ac, 0x00ae, 0x034e, 0x0350, 0x00e9, 0x115e, 0x1161, 0x17b3, 0x17b6, 0x180a, 0x1810, 0x200a, 0x2010, 0x2027,
      0x202f, 0x205f, 0x3163, 0x3165, 0xfdff, 0xfe10, 0xff9f, 0xffa1, 0xf900, 0x1f35c, 0x1f600, 0x20000, 0x323af,
    ];
    for (const cp of taken) expect(displayTextProblem(`a${ch(cp)}b`), cp.toString(16)).toBe('');
    expect(displayTextProblem('')).toBe('');
    expect(displayTextProblem('Thanks for lunch 🍜')).toBe('');
  });
});
