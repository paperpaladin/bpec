#!/usr/bin/env python3
"""Inputs no reference can answer, and what must be true of them anyway.

Byte-level BPE is defined for all 256 byte values, so there is no input
bpec may refuse. That is the point of the byte-level mapping: text that
is not valid UTF-8 still has to tokenise, which is exactly why models
use it.

The library cannot be the oracle here, because it takes `str` and
cannot be handed bytes that are not text. So this checks the
properties that hold regardless:

  it terminates
  it exits 0
  it emits at least one id for a non-empty input
  every id is inside the vocabulary
  it is deterministic across two runs

and, for the subset that IS valid UTF-8, that the ids match the
library after all. Splitting the corpus that way rather than skipping
the invalid half is the difference between testing this and claiming
to.

Run under ASan as well as normally. The checks above cannot tell a
correct answer from a lucky read past the end of a buffer.
"""
import json
import subprocess
import sys

from tokenizers import Tokenizer

# add_special_tokens=False because post-processing is a separate stage
# and bpec does not implement it. RobertaProcessing wraps output in <s>
# and </s>; comparing against that would be testing a feature this
# program says it does not have. GPT-2's post-processor adds nothing,
# so the flag changes none of its answers.


def cases():
    out = []

    # Every byte on its own. 128 of these are not valid UTF-8.
    for b in range(256):
        out.append(bytes([b]))

    # Every byte repeated, so a run has to be handled too.
    for b in (0x00, 0x09, 0x20, 0x7F, 0x80, 0xC0, 0xED, 0xF4, 0xFE, 0xFF):
        out.append(bytes([b]) * 17)

    # Truncated multi-byte sequences: each valid prefix, cut short.
    for seq in (b"\xc3\xa9", b"\xe6\x97\xa5", b"\xf0\x9f\x99\x82"):
        for k in range(1, len(seq)):
            out.append(seq[:k])
            out.append(b"a" + seq[:k] + b"b")

    # Lone surrogates, which UTF-8 forbids and which a decoder that
    # reaches for the obvious arithmetic will happily produce.
    for cp in (0xD800, 0xDBFF, 0xDC00, 0xDFFF):
        out.append(bytes([0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F),
                          0x80 | (cp & 0x3F)]))

    # Overlong encodings of characters that have a shorter form.
    out += [b"\xc0\x80", b"\xc1\xbf", b"\xe0\x80\x80", b"\xf0\x80\x80\x80"]

    # Above the maximum codepoint.
    out += [b"\xf4\x90\x80\x80", b"\xf7\xbf\xbf\xbf", b"\xf8\x88\x80\x80\x80"]

    # NUL in the middle, which is where a C string implementation stops
    # if it is reading the input as one.
    out += [b"a\x00b", b"\x00", b"\x00" * 8, b"hello\x00world"]

    # Long runs, to reach any fixed buffer.
    out += [b"a" * 5000, b" " * 5000, b"\xff" * 5000, b"ab" * 2500]

    # A piece longer than any single merge, and deep nesting of the
    # same character, which is the worst case for a quadratic loop.
    out += [b"z" * 1000, b"\xe6\x97\xa5" * 500]

    return out


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: hostile.py BPEC_BINARY TOKENIZER_JSON")
    binary, tokfile = sys.argv[1], sys.argv[2]
    tok = Tokenizer.from_file(tokfile)
    vocab_size = max(json.load(open(tokfile))["model"]["vocab"].values()) + 1
    added = {a["id"] for a in json.load(open(tokfile))["added_tokens"]}

    bad = 0
    checked = utf8_checked = 0

    for raw in cases():
        checked += 1
        try:
            res = subprocess.run([binary, "--encode", tokfile], input=raw,
                                 capture_output=True, timeout=60)
        except subprocess.TimeoutExpired:
            print("  HANG on %r" % raw[:32])
            bad += 1
            continue

        if res.returncode != 0:
            print("  EXIT %d on %r" % (res.returncode, raw[:32]))
            if res.stderr:
                print("    %s" % res.stderr.decode("utf-8", "replace")[:200])
            bad += 1
            continue

        ids = [int(x) for x in res.stdout.split()]
        if raw and not ids:
            print("  NO IDS for %r" % raw[:32])
            bad += 1
            continue
        out_of_range = [i for i in ids if not (0 <= i < vocab_size) and i not in added]
        if out_of_range:
            print("  ID OUT OF RANGE %s for %r" % (out_of_range[:4], raw[:32]))
            bad += 1
            continue

        again = subprocess.run([binary, "--encode", tokfile], input=raw,
                               capture_output=True).stdout
        if again != res.stdout:
            print("  NOT DETERMINISTIC for %r" % raw[:32])
            bad += 1
            continue

        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        utf8_checked += 1
        want = tok.encode(text, add_special_tokens=False).ids
        if want != ids:
            print("  MISMATCH on valid UTF-8 %r\n    want %s\n    got  %s"
                  % (text[:40], want[:10], ids[:10]))
            bad += 1

    print("hostile: %d of %d inputs survived, %d of them also matched the "
          "reference" % (checked - bad, checked, utf8_checked))
    return 1 if bad else 0


sys.exit(main())
