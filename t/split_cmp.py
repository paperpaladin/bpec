#!/usr/bin/env python3
"""Compare bpec's pre-tokenizer split against the library's, input by input.

Run inside the scratch venv. Prints one summary line and up to five
mismatches.

The corpus is two halves and both are needed. The fixed cases are the
ones somebody thought of, and they are where the known traps live:
contractions, runs of spaces, a space before end of input, mixed
scripts. The random half is where the traps nobody thought of live, and
it is drawn from codepoint ranges chosen to hit class boundaries rather
than uniformly from the whole space, because uniform random Unicode is
almost entirely unassigned and tests nothing.

Comparing the split rather than the ids is deliberate and it is why
this runs before the merge loop exists. A wrong split produces wrong
ids with a merge table that is working perfectly, and a failure here
says "the split is wrong" instead of leaving someone to find that out.
"""
import random
import subprocess
import sys

from tokenizers import pre_tokenizers

FIXED = [
    "hello world", "don't stop", "  two  spaces ", "x1 23 456", "tab\there",
    "Hello, World!", "日本 語", "a  \n b", "", " ", "   ", "\n\n\n",
    "it's a test's test", "ABC123def456", "foo_bar-baz", "(parens) [brackets]",
    "emoji 🙂 here", "Ωmega ß ñ", "¡hola!", "1,234.56", "C++ and C#",
    "tab\tand\ttabs", "trailing   ", "   leading", "mixed 日本語text123",
    "'''", "''s", "a'b'c", "I'LL I'll i'll", "we've WE'VE",
    " nbsp", " emspace", "line1\nline2\r\nline3",
    "<|endoftext|>", "a" * 200, " " * 50, "ab" * 100,
    # Whitespace that is not ASCII is where the first real bug was: the
    # lookahead backtracks by one character and a byte offset split
    # U+00A0 in half.
    "  x", "x  ", "　　 y", " ",
    # A letter-number boundary in both orders, repeatedly.
    "a1a1a1", "1a1a1a", "٠١abc", "abc٠١",
]

# Ranges picked to straddle class boundaries rather than to be uniform.
POOL = (
    list(range(0x20, 0x7F)) +            # ASCII, every class
    [0x09, 0x0A, 0x0D, 0xA0, 0x1680, 0x2000, 0x2003, 0x200A, 0x2028,
     0x202F, 0x205F, 0x3000] +           # whitespace of every width
    list(range(0xC0, 0x180)) +           # Latin letters with marks
    list(range(0x370, 0x400)) +          # Greek
    list(range(0x590, 0x600)) +          # Hebrew, right to left
    list(range(0x660, 0x670)) +          # Arabic-Indic digits
    list(range(0x2000, 0x2070)) +        # punctuation and spaces
    list(range(0x3040, 0x30A0)) +        # kana
    list(range(0x4E00, 0x4E40)) +        # CJK
    [0x1F600, 0x1F1E6, 0x1F3FB, 0x200D]  # emoji, flags, skin tone, ZWJ
)


def random_inputs(n, seed):
    rnd = random.Random(seed)
    out = []
    for _ in range(n):
        k = rnd.randint(0, 24)
        out.append("".join(chr(rnd.choice(POOL)) for _ in range(k)))
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: split_cmp.py BPEC_BINARY RANDOM_COUNT")
    binary, count = sys.argv[1], int(sys.argv[2])
    bl = pre_tokenizers.ByteLevel(add_prefix_space=False, trim_offsets=True)

    cases = FIXED + random_inputs(count, seed=7)
    bad = 0
    for text in cases:
        want = [p[0] for p in bl.pre_tokenize_str(text)]
        res = subprocess.run([binary, "--split"], input=text.encode("utf-8"),
                             capture_output=True)
        got = [g for g in res.stdout.decode("utf-8").split("\n") if g != ""]
        if want != got:
            bad += 1
            if bad <= 5:
                print("  MISMATCH %r" % text[:48])
                print("    want %s" % want[:8])
                print("    got  %s" % got[:8])
    print("split: %d of %d inputs identical" % (len(cases) - bad, len(cases)))
    return 1 if bad else 0


sys.exit(main())
