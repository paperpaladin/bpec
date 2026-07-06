#!/usr/bin/env python3
"""Sweep megabytes of real text, file by file, against the library.

The corpus is whatever large body of real text is installed on this
machine, by default the top level of the Python standard library:
several megabytes of code and prose with identifiers, punctuation,
docstrings, and a long tail of non-ASCII in author names and
examples. It is not vendored and not downloaded. The point is volume
and realism, and the exact bytes differ between machines, so the
suite reports what it actually swept rather than asserting a number
somebody wrote down.

Real text is where a merge table is exercised. Random strings almost
never contain a sequence the merges were trained on, so a random
corpus can agree perfectly while saying nothing about the part of the
table that real input reaches.

Compared per file rather than per line, because a line is one
subprocess and a few megabytes is a hundred thousand of them. On a
mismatch it bisects the file down to the smallest failing line and
prints that, so a failure still names something a person can read.
"""
import os
import subprocess
import sys

from tokenizers import Tokenizer

DEFAULT_ROOTS = ["/usr/lib/python3.13", "/usr/lib/python3.12",
                 "/usr/lib/python3.11", "/usr/share/doc"]


def pick_root():
    env = os.environ.get("BPEC_CORPUS")
    if env:
        return env if os.path.isdir(env) else None
    for r in DEFAULT_ROOTS:
        if os.path.isdir(r):
            return r
    return None


def files(root, budget):
    """Top-level text files, in name order, until the byte budget."""
    out, total = [], 0
    try:
        names = sorted(os.listdir(root))
    except OSError:
        return out, 0
    for name in names:
        if not name.endswith((".py", ".txt", ".md", ".rst")):
            continue
        path = os.path.join(root, name)
        if not os.path.isfile(path):
            continue
        try:
            data = open(path, "rb").read()
            data.decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        out.append((path, data))
        total += len(data)
        if total >= budget:
            break
    return out, total


def encode(binary, tokfile, data):
    r = subprocess.run([binary, "--encode", tokfile], input=data,
                       capture_output=True)
    if r.returncode != 0:
        return None, r.stderr.decode()[:160]
    return [int(x) for x in r.stdout.split()], None


def blame(binary, tokfile, tok, data):
    """The first line of `data` whose ids differ. Printed, not returned."""
    for i, line in enumerate(data.split(b"\n"), 1):
        want = tok.encode(line.decode("utf-8"), add_special_tokens=False).ids
        got, err = encode(binary, tokfile, line)
        if err or want != got:
            print("    line %d: %r" % (i, line[:70]))
            print("      want %s" % want[:12])
            print("      got  %s" % (got[:12] if got is not None else err))
            return
    print("    no single line differs, so the disagreement is about how "
          "lines join")


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: corpus_cmp.py BPEC_BINARY TOKENIZER_JSON BYTE_BUDGET")
    binary, tokfile, budget = sys.argv[1], sys.argv[2], int(sys.argv[3])

    root = pick_root()
    if root is None:
        print("SKIP: no large text corpus found on this machine")
        return 0

    corpus, total = files(root, budget)
    if not corpus:
        print("SKIP: %s held no readable UTF-8 text" % root)
        return 0

    # A sweep that swept almost nothing would report success. The
    # budget is what was asked for; this is what was actually read.
    if total < budget // 4:
        print("SKIP: only %d bytes available under %s, wanted %d"
              % (total, root, budget))
        return 0

    tok = Tokenizer.from_file(tokfile)
    bad = tokens = 0
    for path, data in corpus:
        want = tok.encode(data.decode("utf-8"), add_special_tokens=False).ids
        got, err = encode(binary, tokfile, data)
        tokens += len(want)
        if err or want != got:
            bad += 1
            if bad <= 3:
                print("  MISMATCH %s" % path)
                blame(binary, tokfile, tok, data)

    print("corpus: %d files, %d bytes of %s, %d tokens, %d files differ"
          % (len(corpus), total, root, tokens, bad))
    return 1 if bad else 0


sys.exit(main())
