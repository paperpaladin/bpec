# bpec: plan

## Contract

`bpec tokenizer.json` reads text on stdin and writes one token id per
line, matching what the HuggingFace `tokenizers` library produces for the
same tokenizer and the same input. Byte-level BPE only, which is what
GPT-2, Llama, Mistral and most current models use.

One C11 file, no dependencies. The point of it is that tokenising should
not require a Python runtime and 200 MB of wheels.

## Hard invariant

For every input, `bpec` emits exactly the token ids that
`tokenizers.Tokenizer.encode` emits. Not the same count, not the same
text when decoded: the same integers in the same order.

Approximately right is the failure mode this exists to avoid. A
tokenizer that differs on one input in ten thousand produces a model
that works until it does not, and the difference is invisible in every
test that does not compare ids directly. llama.cpp has shipped that bug
repeatedly.

## Oracle, ranked by how little we control it

1. The `tokenizers` library, token for token, over a corpus. It is the
   reference implementation that produced the model's training data, so
   it is not an opinion about what BPE means. Pinned to one version in
   a scratch venv.
2. The tokenizer file itself, fetched and pinned by sha256. A result
   cannot shift because a vocabulary changed underneath it.
3. `tokenizers.pre_tokenizers.ByteLevel.alphabet()`, for the byte-level
   mapping, which is a fixed 256-character set and can be compared
   whole rather than sampled.
4. Round-tripping through our own decoder. The weakest, because a
   symmetric bug in encode and decode cancels: it catches nothing the
   first three miss and is here only to make a failure easier to read.

## Non-goals

- Training. This reads a vocabulary and applies it. Producing merges
  from a corpus is a different program.
- WordPiece, Unigram, SentencePiece. Byte-level BPE only, and the
  README says so rather than failing confusingly on a file it cannot
  handle.
- Normalizers. GPT-2 has none. A tokenizer file that declares one is
  refused by name, not ignored, because ignoring it silently produces
  wrong ids for every input containing the characters it would have
  touched.
- A model runtime. This is the tokenizer and nothing else.
- Speed as a headline. It should not be slow, but exactness is the
  claim and a fast wrong answer is worth nothing.

## Commit sequence

- [x] 01 the oracle rig: pinned tokenizer, pinned library, a corpus, and
      a comparison that reports ids-equal over N inputs
      why: the harness comes first. Everything after this is graded by
           it, and building it first means no slice can be written
           without a way to tell whether it is right.
- [x] 02 the byte-level alphabet, both directions
      why: 256 characters, fixed, comparable against the library whole
           rather than by sampling. The smallest piece with an exact
           external answer.
      proves: our 256 codepoints are the library's, compared as a set.
              Two mutants kill it: shortening the Latin-1 run by one
              byte, and starting the remapped range at 0x101 instead of
              0x100.
      note: the map is derived, not typed. Bytes that are already
            printable stand for themselves and the remaining 68 take
            0x100 upward in byte order. A 256-entry table written by
            hand is 256 chances to be wrong in a way that shows up on
            one input in a million.
- [x] 03 read tokenizer.json: vocab and merges, with a refusal for every
      field we do not implement
      why: the file is the input and most of the ways this goes wrong
           are in reading it. A normalizer or an unsupported model type
           must be refused by name.
      proves: 50257 vocabulary entries and 50000 merges, both counts
              taken from the file by the library and by us
              independently. Seven refusals, each a real tokenizer file
              with one field changed, each required to name the field
              it is refusing: normalizer, model type, dropout,
              unk_token, end_of_word_suffix, pre_tokenizer type, and a
              file written with \u escapes.
      found: two bugs in this commit's own code, both found by the
             refusal tests rather than by reading.
             A refusal did not refuse. "type" comes after vocab and
             merges in a real file, so by the time WordPiece was
             rejected the vocabulary had loaded, and the only gate was
             "did we get a vocabulary", so load_tokenizer returned
             SUCCESS while printing "only BPE is implemented" directly
             above it.
             And an error with no message. A Json error inside
             load_model had its text in j.err, and the caller returned
             before printing it, so every malformed file came out as
             exit 1 and total silence.
      note: \u escapes are refused rather than decoded. Getting them
            wrong corrupts exactly the non-ASCII vocabulary entries
            byte-level BPE depends on, silently. Item 10 will force
            them.
- [x] 04 the GPT-2 pre-tokenizer regex, against the library's split
      why: this is where exactness is lost. The pattern needs Unicode
           letter and number classes, and `\s+(?!\S)` needs lookahead.
           Compared split by split before any merging happens.
      files: bpec.c classes.h tools/gen_classes.py t/split_cmp.py t/run.sh
      proves: 2045 of 2045 inputs split identically to the library, 45
              fixed cases plus 2000 random strings drawn from codepoint
              ranges chosen to straddle class boundaries. Uniform random
              Unicode would have been almost entirely unassigned and
              would have tested nothing.
      found: the lookahead in `\s+(?!\S)` backtracks by one CHARACTER
             and the first version backtracked by one BYTE, so a
             multi-byte space was cut in half: U+00A0 came out as two
             pieces where the reference gives one, and U+2003 the same.
             Found by the corpus. Reading the code would not have shown
             it, because the code looked right for every ASCII space.
      note: hand-written scanner, not a regex engine. The pattern is
            fixed and five of its six alternatives are a character
            class and a plus, so a scanner is smaller and has no
            lookahead machinery to get wrong. It also cannot borrow
            `rx` from this collection, since no suite here may depend
            on a sibling repository.
      note: the three character classes are generated from Python's
            unicodedata, 807 ranges, never typed. The reference gets
            its tables from the Rust regex crate, which tracks its own
            Unicode version, so the two could disagree on codepoints
            assigned between versions. 2045 inputs have not found such
            a disagreement; that is evidence and not proof, and the
            generated header says so.
      note: one equivalent mutant. Swapping ` ?\p{L}+` and ` ?\p{N}+`
            in the alternation changes nothing, because after the
            optional space the next character is either a letter or a
            number and never both, so the two alternatives are
            disjoint. Recorded rather than treated as a gap in the
            test. Two other mutants are caught: dropping the `'ll`
            contraction, and keeping the whole whitespace run instead
            of backtracking.
- [x] 05 the BPE merge loop, rank-ordered
      why: the core, and meaningless without 04 feeding it the right
           pieces.
      files: bpec.c t/encode_cmp.py t/run.sh
      proves: 3789 of 3789 inputs produce identical ids, 64848 tokens
              compared. Three sources: the fixed traps, 2000 random
              strings over class boundaries, and every line of this
              repository's own C, Markdown, shell and Python, which is
              where the merge table is actually exercised since random
              strings rarely contain a sequence the merges were
              trained on.
      found: two mutants killed hard. Merging the highest ranked pair
             instead of the lowest drops agreement to 483 of 1873.
             Taking the first mergeable pair and ignoring rank drops it
             to 606. Rank order is the algorithm.
      note: quadratic per piece, marked `shortcut:`. Pieces are words,
            so the constant is a handful of symbols.
- [x] 06 added and special tokens
      why: `<|endoftext|>` must survive as one id and must not be
           reachable by merging ordinary text.
      files: bpec.c
      proves: with added tokens unmatched, 1869 of 1873 instead of
              1873. A small mutant, and the right size: only inputs
              containing one can see it.
      found: the first version probed for added tokens at
             pre-tokenizer piece boundaries, which misses every token
             that does not start one. In "\"<|endoftext|>\"" the
             punctuation run matches "\"<|" and steps over the start,
             so the token came out as ordinary text. The input is
             split on added tokens now, earliest match first and
             longest at that position. Three corpus inputs caught it
             and none of the hand-written cases did, because every one
             of those put the token after a letter.
      note: lstrip, rstrip and single_word are refused by name. Each
            changes which text the token matches, and GPT-2 sets all
            three false.
- [ ] 07 a corpus sweep: every line of a few megabytes of real text,
      ids compared
      why: the number this project lives or dies by. Millions of
           comparisons, not dozens.
- [x] 08 a hostile-input sweep: lone surrogates, truncated UTF-8, NUL,
      very long runs, every byte 0 to 255
      why: input is untrusted text and the byte-level mapping is
           defined for all 256 bytes, so there is no excuse for a
           crash.
      files: t/hostile.py Makefile bpec.c t/run.sh
      proves: 299 inputs survive, 141 of them also matching the
              reference. The library cannot answer for the other 158
              because it takes str and cannot be handed bytes that are
              not text, so those are held to what holds regardless: it
              terminates, exits 0, emits ids inside the vocabulary, and
              gives the same answer twice. Splitting the corpus that
              way rather than skipping the invalid half is the
              difference between testing this and claiming to.
      found: four defects, none of which the earlier corpora could see.
             A 1 MB static input buffer SILENTLY TRUNCATED anything
             larger. 1.2 MB of input produced 524289 ids and looked
             like a clean run, because every id it did emit was right.
             merge_piece capped at 4096 symbols and dropped the rest of
             a longer piece, the same shape of wrong answer.
             A 4096-byte piece buffer refused a 5000-character word,
             which the reference tokenises without complaint.
             The added-token scan restarted at every piece and ran to
             end of input, making encoding quadratic: 1.2 MB of
             ordinary words did not finish at all. Cached now, 0.1s.
      found: and one more from the corpus itself, after the cache was
             added. The cache validity test was `next_added_at < i`,
             which is false at i = 0, so the scan never ran for the
             first piece. Every hand-written case passed because none
             of them had the token early enough to matter; a line of
             this repository's own README did not, because it quotes
             the token after a backtick and a quote. The flag is
             explicit now.
      note: `make test-asan` added. LeakSanitizer immediately found a
            real 1.3 MB leak: the tokenizer file was read into a local
            buffer and never freed, though every string is copied out
            of it with xdup. Freed. The remaining allocations live in
            globals and are still reachable at exit, which is a design
            choice and not a leak.
- [x] 09 decode, and the round trip
      why: last of the features, and weakest as an oracle, so it comes
           after the ones that can actually catch something.
      done: and it was not the weakest after all. The ids come from
           the library, so the decoder is tested on its own rather
           than with encode and two matching mistakes cancelling.
           The find: encode is not invertible when an added token
           sets lstrip. "a<s>  <mask>b" loses its two spaces into the
           <mask> match and nothing can put them back. Fourteen
           failures that were the tokenizer working as designed and
           the test comparing against the wrong thing; the oracle is
           decode(ids, skip_special_tokens=False).
           264 round trips over raw bytes, including all 256 values
           alone, where the library cannot be asked and the property
           holds anyway.
           Five refusals, one of which needs a constructed file: a
           vocabulary entry outside the byte alphabet. Kept because
           removing the bounds check in front of it turns the guard
           into an out-of-bounds read.
- [x] 10 a second tokenizer file from a different model family
      why: one vocabulary is one data point. A second file uses the
           same machinery with different merges and would catch
           anything hard-coded to GPT-2.
      done: roberta-base, pinned by sha256 like the first. 4269 inputs
           identical, 72388 tokens, first run, no mismatch: nothing was
           hard-coded to GPT-2 after all, which is the answer this item
           existed to get rather than the one it expected.
           It was not free. RoBERTa's <mask> sets lstrip, which no
           GPT-2 file sets, so the file was refused outright until the
           flag was implemented. lstrip makes an added token absorb the
           whitespace in front of it: "a  <mask>" is "a" and "  <mask>",
           not "a", two spaces, and the token.
           Three candidates were rejected before this one and the
           reasons are the gaps. gpt-neox-20b and Qwen2 both carry an
           NFC normalizer, which is refused by name; Qwen additionally
           uses a Sequence pre-tokenizer with its own regex. So the
           second family shares GPT-2's pre-tokenizer, and a tokenizer
           with a different one is still untested.
           RoBERTa has a post-processor that wraps output in <s> and
           </s>. Every comparison passes add_special_tokens=False and
           the README names post-processing as out of scope, because
           comparing against it would be grading this on a feature it
           says it does not have.
           rstrip, lstrip's mirror, is set by no real file here, so the
           code for it would have been untested. The oracle reads a
           file we construct, so the file is constructed: the real
           RoBERTa vocabulary with rstrip added to <s>, and the library
           answers for it. That is what caught the two mutations that
           survived without it, one of them a real bug: the backward
           scan has to stop at the start of the segment, because in
           "a<s>  <mask>b" the rstrip token takes both spaces and the
           lstrip token must not take them a second time.
           Seven mutations, all caught.
- [ ] 11 README with the measured agreement rate and what does not work
      why: last, so the numbers in it are the numbers the suite prints.
