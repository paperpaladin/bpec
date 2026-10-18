#!/usr/bin/env python3
"""Reference tokenisations from the HuggingFace library.

Run inside the scratch venv that t/run.sh builds. Reads one input per
line of stdin, writes one line per input: the token ids, space
separated. An empty input line produces an empty output line, which is
a real case and not a parse error.

Inputs are hex-encoded on the way in so a corpus can contain newlines,
NUL and invalid UTF-8 without the line protocol falling apart. The
tokenizer itself is handed real bytes.
"""
import sys

from tokenizers import Tokenizer


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: oracle.py TOKENIZER_JSON  (hex-encoded inputs on stdin)")
    tok = Tokenizer.from_file(sys.argv[1])
    out = []
    for line in sys.stdin:
        line = line.strip()
        if not line:
            out.append("")
            continue
        raw = bytes.fromhex(line)
        # The library takes str, so anything that is not valid UTF-8
        # cannot be asked of it. Those inputs are the C side's business
        # and the harness skips them here rather than inventing an
        # answer: see t/run.sh, which counts the skips out loud.
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            out.append("SKIP-not-utf8")
            continue
        # add_special_tokens=False: post-processing is a separate
        # stage and bpec does not implement it. RoBERTa's wraps output
        # in <s> and </s>; comparing against that would be grading this
        # program on a feature it says it does not have. GPT-2's adds
        # nothing, so no GPT-2 answer changes.
        ids = tok.encode(text, add_special_tokens=False).ids
        out.append(" ".join(str(i) for i in ids))
    sys.stdout.write("\n".join(out) + "\n")


main()
