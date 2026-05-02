#!/usr/bin/env python3
"""Compare bpec's token ids against the library's, input by input.

Run inside the scratch venv. Prints one summary line and up to five
mismatches.

Three sources, because each finds a different thing.

The fixed cases are the traps somebody thought of: contractions, runs
of spaces, mixed scripts, a space at end of input.

The random cases are drawn from codepoint ranges chosen to straddle
class boundaries. Uniform random Unicode is almost entirely unassigned
and would test nothing while producing an impressive number.

The repository's own files are real text: C with its punctuation and
identifiers, Markdown with its prose, shell with its sigils. Real text
is where the merge table is actually exercised, since random strings
rarely contain a sequence the merges were trained on.

bpec is run once per input rather than once for the whole corpus. That
is slower and it is deliberate: a per-input comparison names the input
that disagrees, and a batched one would report a diff somewhere in a
megabyte.
"""
import random
import subprocess
import sys
from pathlib import Path

from tokenizers import Tokenizer

# add_special_tokens=False because post-processing is a separate stage
# and bpec does not implement it. RobertaProcessing wraps output in <s>
# and </s>; comparing against that would be testing a feature this
# program says it does not have. GPT-2's post-processor adds nothing,
# so the flag changes none of its answers.

FIXED = [
    "hello world", "don't stop", "  two  spaces ", "x1 23 456", "tab\there",
    "Hello, World!", "日本 語", "a  \n b", "", " ", "   ", "\n\n\n",
    "it's a test's test", "ABC123def456", "foo_bar-baz", "(parens) [brackets]",
    "emoji 🙂 here", "Ωmega ß ñ", "¡hola!", "1,234.56", "C++ and C#",
    "trailing   ", "   leading", "mixed 日本語text123", "'''", "''s",
    "I'LL I'll i'll", "we've WE'VE", " nbsp", " emspace",
    "line1\nline2\r\nline3", "a" * 200, " " * 50, "ab" * 100,
    "  x", "x  ", "　　 y", "a1a1a1", "1a1a1a", "٠١abc", "abc٠١",
]

POOL = (
    list(range(0x20, 0x7F)) +
    [0x09, 0x0A, 0x0D, 0xA0, 0x1680, 0x2000, 0x2003, 0x200A, 0x202F, 0x3000] +
    list(range(0xC0, 0x180)) + list(range(0x370, 0x400)) +
    list(range(0x590, 0x600)) + list(range(0x660, 0x670)) +
    list(range(0x2000, 0x2070)) + list(range(0x3040, 0x30A0)) +
    list(range(0x4E00, 0x4E40)) + [0x1F600, 0x1F1E6, 0x1F3FB, 0x200D]
)


def random_inputs(n, seed):
    rnd = random.Random(seed)
    out = []
    for _ in range(n):
        out.append("".join(chr(rnd.choice(POOL))
                           for _ in range(rnd.randint(0, 24))))
    return out


def own_source():
    """Every line of this repository's own tracked text."""
    out = []
    root = Path(__file__).resolve().parent.parent
    for pat in ("*.c", "*.h", "*.md", "t/*.sh", "t/*.py", "tools/*.py"):
        for f in sorted(root.glob(pat)):
            if "classes.h" in f.name:        # 800 lines of hex, no prose
                continue
            try:
                out += f.read_text(encoding="utf-8").split("\n")
            except (UnicodeDecodeError, OSError):
                continue
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: encode_cmp.py BPEC_BINARY TOKENIZER_JSON RANDOM_COUNT")
    binary, tokfile, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
    tok = Tokenizer.from_file(tokfile)

    cases = FIXED + random_inputs(count, seed=11) + own_source()
    bad = tokens = 0
    for text in cases:
        want = tok.encode(text, add_special_tokens=False).ids
        res = subprocess.run([binary, "--encode", tokfile],
                             input=text.encode("utf-8"), capture_output=True)
        got = [int(x) for x in res.stdout.split()]
        tokens += len(want)
        if want != got:
            bad += 1
            if bad <= 5:
                print("  MISMATCH %r" % text[:60])
                print("    want %s" % want[:12])
                print("    got  %s" % got[:12])
    print("encode: %d of %d inputs identical, %d tokens compared"
          % (len(cases) - bad, len(cases), tokens))
    return 1 if bad else 0


sys.exit(main())
