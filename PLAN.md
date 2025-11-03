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
- [ ] 03 read tokenizer.json: vocab and merges, with a refusal for every
      field we do not implement
      why: the file is the input and most of the ways this goes wrong
           are in reading it. A normalizer or an unsupported model type
           must be refused by name.
- [ ] 04 the GPT-2 pre-tokenizer regex, against the library's split
      why: this is where exactness is lost. The pattern needs Unicode
           letter and number classes, and `\s+(?!\S)` needs lookahead.
           Compared split by split before any merging happens.
- [ ] 05 the BPE merge loop, rank-ordered
      why: the core, and meaningless without 04 feeding it the right
           pieces.
- [ ] 06 added and special tokens
      why: `<|endoftext|>` must survive as one id and must not be
           reachable by merging ordinary text.
- [ ] 07 a corpus sweep: every line of a few megabytes of real text,
      ids compared
      why: the number this project lives or dies by. Millions of
           comparisons, not dozens.
- [ ] 08 a hostile-input sweep: lone surrogates, truncated UTF-8, NUL,
      very long runs, every byte 0 to 255
      why: input is untrusted text and the byte-level mapping is
           defined for all 256 bytes, so there is no excuse for a
           crash.
- [ ] 09 decode, and the round trip
      why: last of the features, and weakest as an oracle, so it comes
           after the ones that can actually catch something.
- [ ] 10 a second tokenizer file from a different model family
      why: one vocabulary is one data point. A Llama tokenizer uses the
           same machinery with different merges and would catch
           anything hard-coded to GPT-2.
- [ ] 11 README with the measured agreement rate and what does not work
      why: last, so the numbers in it are the numbers the suite prints.
