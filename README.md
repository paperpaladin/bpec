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

`make test`: **45 passed, 0 failed.** `make test-asan`: the same under
ASan and UBSan, clean, and it takes about an hour. The corpus is
thousands of separate runs and each one loads a 50257-entry
vocabulary; under a sanitizer that is twenty times slower. It is slow
rather than hung.

## Why exactness and not "close"

A tokenizer that differs on one input in ten thousand produces a model
that works until it does not, and the difference is invisible to every
test that does not compare ids directly. Decoded text looks fine.
Round trips pass. llama.cpp has shipped that bug more than once.

So the oracle is the reference implementation itself, token for token,
and nothing here is graded against a number anybody typed.

## The oracle

The `tokenizers` library, pinned to one version in a scratch venv, with
two tokenizer files, GPT-2 and roberta-base, each fetched and pinned by
sha256 so a result cannot shift because a vocabulary changed underneath
it. Nothing is vendored: the files belong to their models and the
library is 30 MB of wheels.

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

The ids themselves. 4949 of 4949 inputs identical, 81666 tokens
compared, against the fixed traps, the random strings, and every line
of this repository's own C, Markdown, shell and Python. Real text is
where the merge table is exercised, because random strings rarely
contain a sequence the merges were trained on.

Added tokens. `<|endoftext|>` survives as one id and is not reachable
by merging ordinary text.

A second model family. The whole corpus again against roberta-base,
4949 of 4949 identical, 81566 tokens, on the first run.

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

## The second family, and what it cost

Everything above was GPT-2, which is one vocabulary and therefore one
data point. roberta-base is the same byte-level BPE over a different
vocabulary with five added tokens instead of one.

It agreed on the first run, every input, which says nothing here was
hard-coded to GPT-2. That is the answer the comparison existed to get
and not the one it expected.

Getting as far as running it was the work. RoBERTa's `<mask>` sets
`lstrip`, which no GPT-2 file sets, so the file was refused outright
by name. `lstrip` makes an added token absorb the whitespace in front
of it: `"a  <mask>"` is `"a"` and `"  <mask>"`, not `"a"`, two spaces
and the token. `<s>` does not set it and is the control, its space
tokenises separately as the ordinary space token.

Three other candidates were rejected first and the reasons are the
gaps. `gpt-neox-20b` and `Qwen2` both carry an NFC normalizer, which
is refused by name, and Qwen uses a `Sequence` pre-tokenizer with its
own regex. So the second family shares GPT-2's pre-tokenizer, and one
with a different pre-tokenizer is still untested.

The new cases also found a leak that had been there since the input
buffer was made growable, and the way it surfaced is the part worth
keeping. LeakSanitizer reported it on three inputs and stayed silent
on the hundreds of others, which read as input-dependent and sent me
looking for a path through the added-token branch. There is no such
path: `buf` and `piece` were freed on none of the five returns, and on
the quiet inputs the dead pointer was still sitting in stack memory
below the frame, where LSan's conservative scan counts it as
reachable. A leak detector that finds a leak sometimes is reporting
the stack layout, not the leak. One exit now, and both are freed.

`rstrip` is `lstrip`'s mirror and no real file here sets it, so the
code for it would have been untested against anything. The oracle will
read a file we construct, so the file is constructed: the real RoBERTa
vocabulary with `rstrip` added to `<s>`, and the library answers for
it. Two mutations survived without that file and one of them was a
real bug. The backward scan has to stop at the start of the segment
rather than the start of the input, because in `"a<s>  <mask>b"` the
`rstrip` token takes both spaces and the `lstrip` token must not take
them a second time.

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

## Four megabytes of real text

1824803 tokens over 144 files and 4017774 bytes of the Python
standard library as installed on this machine, identical to the
library for both tokenizer families on the first run.

Volume is half the point and realism is the other half. Random
strings almost never contain a sequence the merges were trained on,
so a random corpus can agree perfectly while saying nothing about the
part of the merge table that real input actually reaches.

The corpus is not vendored and not downloaded: it is whatever large
body of text the machine already has, so the suite reports what it
swept rather than asserting a number that would be wrong on another
machine. It skips loudly when there is nothing large to read, and
also when the directory exists but holds far less than asked for,
because a sweep that swept almost nothing would otherwise report
success.

Comparison is per file, because a line is one subprocess and four
megabytes is a hundred thousand of them. A mismatch bisects the file
to the smallest failing line and prints it with both id sequences, so
a failure names something a person can read instead of "somewhere in
4 MB". That path was checked by planting a merge-rank defect rather
than assumed to work.

Thirteen seconds per family. The ASan target sweeps a tenth of it,
since a sanitizer costs about twenty times per run.

## Decode, and where encode stops being invertible

Ids back to bytes: vocabulary entries concatenated, then each
codepoint mapped back to the byte it stands for. The inverse of the
byte map is derived from the forward one rather than written out
again, because a hand-written inverse is a second place for the same
fact to be wrong and the two would disagree only for the 68 bytes
that do not stand for themselves, which is the half nobody checks by
eye.

4540 decodes against the library per tokenizer, and the ids come from
the library rather than from us, so this is the decoder on its own
rather than encode and decode together with two matching mistakes
cancelling.

The expected text is `decode(ids, skip_special_tokens=False)` and not
the original input, and finding out why took fourteen failures.
**Encode is not invertible when an added token sets `lstrip`.**
`"a<s>  <mask>b"` has its two spaces absorbed into the `<mask>`
match, and the token carries no spaces, so nothing can put them
back. The library returns `"a<s><mask>b"` and so do we. The fourteen
failures were the tokenizer working as designed and the test
measuring the wrong thing.

The round trip is the other half and it runs on bytes the library
cannot be asked about: 264 cases including every one of the 256 byte
values alone, truncated UTF-8, lone surrogates and NUL. Byte-level
BPE is lossless by construction, so encode then decode has to return
the original bytes exactly, and that is the property that would catch
a decoder quietly dropping one.

Four refusals: an id past the end of the vocabulary, an id inside it
with no entry, a negative id, and a line that is not a number.
Decoding any of those to nothing, or to a replacement character,
would turn a corrupt id stream into plausible text.

One more refusal needs a constructed file, because no real tokenizer
reaches it: a vocabulary entry containing a codepoint the byte
alphabet never produces. Worth keeping rather than deleting, because
dropping the bounds check in front of it turns the guard into an
out-of-bounds read, and the test catches that as a crash with no
message rather than as a wrong answer.

## A survivor, written down

Removing the added-token pass from the decode table changes nothing,
and it is not an untested branch: every added token in both files is
also a vocabulary entry at the same id. `<|endoftext|>` is 50256 in
GPT-2's vocabulary as well as in its added tokens, and all five of
RoBERTa's are the same. The pass is kept because a file where they
differ is allowed by the format, and it is ordered after the
vocabulary because an added token that collides with one is the
added token. Neither file exercises that, which is why this
paragraph exists instead of a test.

## What does not work

- `skip_special_tokens`, the library's default on decode, which
  deletes added tokens from the output entirely. That is a different
  operation and this does not implement it.
- `\u` escapes in a tokenizer file are refused rather than decoded.
  Decoding them wrongly corrupts exactly the non-ASCII vocabulary
  entries byte-level BPE depends on, and silently. Neither file here
  uses them, so the refusal still stands.
- Post-processing. RoBERTa's wraps output in `<s>` and `</s>`, and
  every comparison here passes `add_special_tokens=False`. Comparing
  against a stage this program does not implement would be grading it
  on a feature it says it does not have.
- `single_word` on an added token is refused by name. No file here
  sets it, and the rule above about untested code applies: it would be
  a feature with no oracle.
- The backward scan for `lstrip` recognises ASCII whitespace only. A
  multi-byte Unicode space immediately before an `lstrip` token would
  not be absorbed. Fixing it needs a backwards UTF-8 scan, and no file
  or corpus here has produced the case.
- Normalizers, WordPiece, Unigram and SentencePiece are refused by
  name. A field quietly ignored produces wrong ids for every input that
  would have touched it.
- Training. This applies a vocabulary and does not produce one.
- Two tokenizer files is two data points, and they share a
  pre-tokenizer. A family with a different one, or with a normalizer,
  is refused rather than measured.
- The character classes come from Python's `unicodedata`, Unicode
  15.1.0. The reference gets its tables from the Rust regex crate,
  which tracks its own version, so the two could disagree on codepoints
  assigned between versions. 2045 inputs have not found such a
  disagreement, which is evidence and not proof.

GPL-3.0-or-later.
