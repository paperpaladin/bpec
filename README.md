# bpec

Byte-level BPE tokenisation in one C11 file, no dependencies. The kind
GPT-2, Llama and Mistral use.

The claim is exactness. For any input, the same token ids as the
HuggingFace `tokenizers` library, in the same order. Not the same count,
not the same text when decoded: the same integers.

```
$ printf 'hello world' | ./bpec --encode tokenizer.json
31373
995
```

`make test`: **19 passed, 0 failed.** `make test-asan`: the same under
ASan and UBSan.

## Why exactness and not "close"

A tokenizer that differs on one input in ten thousand produces a model
that works until it does not, and the difference is invisible to every
test that does not compare ids directly. Decoded text looks fine.
Round trips pass. llama.cpp has shipped that bug more than once.

So the oracle is the reference implementation itself, token for token,
and nothing here is graded against a number anybody typed.

## The oracle

The `tokenizers` library, pinned to one version in a scratch venv, with
the GPT-2 tokenizer file fetched and pinned by sha256 so a result cannot
shift because a vocabulary changed underneath it. Neither is vendored:
the file belongs to its model and the library is 30 MB of wheels.

Offline, the suite skips and says so. An exactness claim with no
reference implementation to check against is not a weaker claim, it is
no claim.

The rig checks itself before it checks anything. It tokenises a known
string and requires the known ids; it tokenises a different string and
requires a different answer, which is what says the oracle is reading
its input rather than returning a constant; it requires an empty input
to produce nothing; and it requires invalid UTF-8 to come back marked
unanswerable, because the library takes `str` and cannot be asked about
bytes that are not text. Without those four, "agrees with the oracle"
could mean the oracle never ran.

## What works

The byte-level alphabet, 256 codepoints, compared against the library
as a whole set rather than sampled. The map is derived, not typed:
printable bytes stand for themselves and the remaining 68 take 0x100
upward in byte order.

Reading a tokenizer file: 50257 vocabulary entries and 50000 merges,
each count taken from the file independently by the library and by us.

The pre-tokenizer split, which is where exactness is won or lost. 2045
of 2045 inputs split identically to the library, 45 fixed cases and
2000 random strings drawn from codepoint ranges chosen to straddle
class boundaries. Uniform random Unicode would have been almost
entirely unassigned and would have tested nothing while looking
thorough.

The ids themselves. 4049 of 4049 inputs identical, 68640 tokens
compared, against the fixed traps, the random strings, and every line
of this repository's own C, Markdown, shell and Python. Real text is
where the merge table is exercised, because random strings rarely
contain a sequence the merges were trained on.

Added tokens. `<|endoftext|>` survives as one id and is not reachable
by merging ordinary text.

The split is a hand-written scanner rather than a regex engine. The
pattern is fixed and five of its six alternatives are a character class
and a plus, so a scanner is smaller and has no lookahead machinery to
get wrong.

## Two things the tests found that reading did not

The lookahead in `\s+(?!\S)` backtracks by one character, and the first
version backtracked by one byte. A multi-byte space was cut in half:
U+00A0 came out as two pieces where the reference gives one. Every
ASCII space behaved perfectly, which is why it survived inspection.

A refusal that did not refuse. `"type"` comes after `vocab` and
`merges` in a real tokenizer file, so by the time a WordPiece model was
rejected the vocabulary had already loaded, and the only gate was
whether a vocabulary existed. The loader returned success while
printing "only BPE is implemented" on the line above.

## A third bug the corpus found

The first version of added-token handling probed at pre-tokenizer piece
boundaries. That misses every token which does not begin one: in
`"<|endoftext|>"` the punctuation run matches `"<|` and steps straight
over the start, so the token came out as thirteen ordinary tokens. The
input is split on added tokens now, earliest match first and longest at
that position.

Three corpus inputs caught it and not one of the hand-written cases
did, because every one of those had put the token after a letter.

## Hostile input, where the oracle runs out

Byte-level BPE is defined for all 256 byte values, so there is no input
bpec may refuse: text that is not valid UTF-8 still has to tokenise,
which is the reason models use byte level at all.

The library cannot be the oracle for that half, because it takes `str`
and cannot be handed bytes that are not text. So 158 of the 299 cases
are held to what is true regardless: it terminates, exits 0, emits ids
inside the vocabulary, and gives the same answer twice. The other 141
are valid UTF-8 and are still compared against the reference. Splitting
the corpus that way rather than skipping the invalid half is the
difference between testing this and claiming to.

That sweep found four defects, and two of them are the same shape: a
wrong answer that looks like a right one.

A 1 MB static input buffer silently truncated anything larger. 1.2 MB
of input produced 524289 ids and looked like a clean run, because every
id it did emit was correct. `merge_piece` capped at 4096 symbols and
dropped the rest of a longer piece the same way. Both read the whole
input now.

A 4096-byte piece buffer refused a 5000-character word that the
reference tokenises without complaint. Sized from the piece now.

And the added-token scan restarted at every piece and ran to the end of
input, making encoding quadratic in the input size: 1.2 MB of ordinary
words never finished. It is cached, and that input takes 0.1 seconds.

`make test-asan` was added for this and found a real 1.3 MB leak on its
first run: the tokenizer file was read into a local buffer and never
freed, though every string is copied out of it. The remaining
allocations live in globals and are still reachable at exit, which is a
choice rather than a leak.

## What does not work

- Decoding, and the round trip. Item 09.
- `\u` escapes in a tokenizer file are refused rather than decoded.
  Decoding them wrongly corrupts exactly the non-ASCII vocabulary
  entries byte-level BPE depends on, and silently. A second tokenizer
  from another model family, item 10, will force them.
- Normalizers, WordPiece, Unigram and SentencePiece are refused by
  name. A field quietly ignored produces wrong ids for every input that
  would have touched it.
- Training. This applies a vocabulary and does not produce one.
- One tokenizer file is one data point. Everything measured here is
  GPT-2 until item 10 adds a second family.
- The character classes come from Python's `unicodedata`, Unicode
  15.1.0. The reference gets its tables from the Rust regex crate,
  which tracks its own version, so the two could disagree on codepoints
  assigned between versions. 2045 inputs have not found such a
  disagreement, which is evidence and not proof.

GPL-3.0-or-later.
