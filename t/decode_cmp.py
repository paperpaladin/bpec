#!/usr/bin/env python3
"""Compare bpec's decode against the library's, and the round trip.

Two different questions, and they need different corpora.

Against the library: the ids come from the LIBRARY and so does the
expected text, so this tests the decoder on its own rather than
testing encode and decode together and letting two matching mistakes
cancel.

The expected text is `decode(ids, skip_special_tokens=False)` and not
the original input, and the difference is not pedantry. Encode is not
invertible when an added token sets `lstrip`: "a<s>  <mask>b" has its
two spaces absorbed into the `<mask>` match, and the token carries no
spaces, so no decoder can put them back. The library returns
"a<s><mask>b" and so do we. Comparing against the original input
instead reports fourteen failures that are the tokenizer working as
designed.

`skip_special_tokens` defaults to True, which deletes added tokens
from the output entirely. That is a different operation and bpec does
not implement it.

The round trip is the other half, and it runs on everything including
the bytes that are not text. Byte-level BPE is lossless by
construction: every one of the 256 byte values has a token, so encode
followed by decode has to give the original bytes back exactly. The
library cannot answer for the invalid half, because it takes `str`,
but this property holds regardless and is the one that would catch a
decoder that quietly dropped a byte.
"""
import random
import subprocess
import sys
from pathlib import Path

from tokenizers import Tokenizer

sys.path.insert(0, str(Path(__file__).resolve().parent))
from encode_cmp import FIXED, random_inputs, own_source  # noqa: E402


def raw_cases():
    """Byte strings, including ones that are not valid UTF-8."""
    out = [bytes([b]) for b in range(256)]
    out += [b"\xff\xfe", b"a\x00b", b"\xc3", b"\xe6\x97", b"\xf0\x9f\x99",
            b"\x80" * 9, b"hello\xffworld", b"\xed\xa0\x80"]
    return out


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: decode_cmp.py BPEC_BINARY TOKENIZER_JSON RANDOM_COUNT")
    binary, tokfile, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
    tok = Tokenizer.from_file(tokfile)

    bad = 0

    # 1. The library's ids, our decoder, against the original text.
    texts = FIXED + random_inputs(count, seed=23) + own_source()
    checked = 0
    for text in texts:
        ids = tok.encode(text, add_special_tokens=False).ids
        if not ids:
            continue
        want = tok.decode(ids, skip_special_tokens=False).encode("utf-8")
        stdin = ("\n".join(str(i) for i in ids) + "\n").encode()
        res = subprocess.run([binary, "--decode", tokfile],
                             input=stdin, capture_output=True)
        if res.returncode != 0:
            print("  DECODE FAILED %r: %s"
                  % (text[:40], res.stderr.decode()[:120]))
            bad += 1
            continue
        checked += 1
        if res.stdout != want:
            bad += 1
            if bad <= 5:
                print("  MISMATCH %r\n    want %r\n    got  %r"
                      % (text[:40], want[:40], res.stdout[:40]))

    # 2. The round trip, over bytes the library cannot be asked about.
    rt = 0
    for raw in raw_cases():
        enc = subprocess.run([binary, "--encode", tokfile],
                             input=raw, capture_output=True)
        if enc.returncode != 0:
            print("  ENCODE FAILED %r" % raw[:24])
            bad += 1
            continue
        dec = subprocess.run([binary, "--decode", tokfile],
                             input=enc.stdout, capture_output=True)
        if dec.returncode != 0:
            print("  DECODE FAILED %r: %s" % (raw[:24], dec.stderr.decode()[:120]))
            bad += 1
            continue
        rt += 1
        if dec.stdout != raw:
            bad += 1
            if bad <= 8:
                print("  ROUND TRIP %r -> %r" % (raw[:24], dec.stdout[:24]))

    print("decode: %d against the library's ids, %d round trips over raw "
          "bytes, %d wrong" % (checked, rt, bad))
    return 1 if bad else 0


sys.exit(main())
