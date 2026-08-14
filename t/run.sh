#!/bin/sh
# Oracle: the HuggingFace tokenizers library, token for token.
#
# Nothing here is vendored. The tokenizer file belongs to its model and
# the library is 30 MB of wheels, so both are fetched into scratch/,
# pinned, and checked. Offline, this skips loudly: an exactness claim
# with no reference implementation to check it against is not a weaker
# claim, it is no claim.
set -u
cd "$(dirname "$0")/.."

TOK_URL=https://huggingface.co/gpt2/raw/main/tokenizer.json
TOK_SHA=8414cab924d8b9b33013f0d221c5862f365ee9be39c5c2bfae8a5a9e970478a6
TOK=scratch/gpt2-tokenizer.json
# A second family, because one tokenizer file is one data point. RoBERTa
# shares byte-level BPE with GPT-2 and differs where it matters: a
# different vocabulary, five added tokens instead of one, and <mask>
# with lstrip set, which no GPT-2 file exercises.
TOK2_URL=https://huggingface.co/roberta-base/raw/main/tokenizer.json
TOK2_SHA=847bbeab6174d66a88898f729d52fa8d355fafe1bea101cf960dd404581df70e
TOK2=scratch/roberta-tokenizer.json
VENV=scratch/venv
LIB_VERSION=0.23.3
: "${BPEC:=./bpec}"
# Fewer random cases under a sanitizer, which costs about 20x per run.
: "${BPEC_RANDOM:=2000}"
# Bytes of real text to sweep. Not vendored and not downloaded: the
# corpus is whatever large body of text is installed here, by default
# the top of the Python standard library. BPEC_CORPUS points it
# elsewhere.
: "${BPEC_CORPUS_BYTES:=4000000}"

pass=0
fail=0
ok()   { pass=$((pass + 1)); printf 'ok    %s\n' "$1"; }
nok()  { fail=$((fail + 1)); printf 'FAIL  %s\n' "$1"
         [ $# -gt 1 ] && printf '        %s\n' "$2"; return 0; }
skip() { printf 'SKIP  %s\n' "$1"; }
eq()   { if [ "$2" = "$3" ]; then ok "$1"; else nok "$1" "want [$2] got [$3]"; fi; }

mkdir -p scratch scratch/bad
TMP_SAN=scratch/sanprobe.$$.c

# ---------------------------------------------------------- the tokenizer
# Returns 1 if the file could not be fetched, so a caller can decide
# whether that is a skip or the end of the run. A sha mismatch is
# neither: the pin exists so a result cannot shift under a changed
# vocabulary, and a changed vocabulary has to be looked at by a person.
fetch_tok() { # path, url, sha
	if [ ! -f "$1" ]; then
		command -v curl >/dev/null 2>&1 || return 1
		curl -sSL --max-time 180 -o "$1.part" "$2" 2>/dev/null || {
			rm -f "$1.part"; return 1; }
		mv "$1.part" "$1"
	fi
	got=$(sha256sum "$1" | cut -d' ' -f1)
	if [ "$got" != "$3" ]; then
		echo "FAIL: $1 sha256 is $got, expected $3" >&2
		echo "  the pin exists so a result cannot shift under a changed vocabulary." >&2
		rm -f "$1"
		exit 1
	fi
	return 0
}

fetch_tok "$TOK" "$TOK_URL" "$TOK_SHA" || {
	echo "SKIP: cannot fetch $TOK_URL, so there is no oracle to check against"
	exit 0; }

# ---------------------------------------------------------- the library
if [ ! -x "$VENV/bin/python" ]; then
	python3 -m venv "$VENV" >/dev/null 2>&1 || {
		echo "SKIP: python3 -m venv failed, so the oracle cannot be installed"
		exit 0; }
fi
if ! "$VENV/bin/python" -c 'import tokenizers' 2>/dev/null; then
	"$VENV/bin/pip" install -q "tokenizers==$LIB_VERSION" >/dev/null 2>&1 || {
		echo "SKIP: cannot install tokenizers==$LIB_VERSION (offline?)"
		exit 0; }
fi
have=$("$VENV/bin/python" -c 'import tokenizers; print(tokenizers.__version__)')
eq "the oracle is the pinned library version" "$LIB_VERSION" "$have"

# ------------------------------------------------- the rig checks itself
# Before comparing anything against the oracle, the oracle has to be
# shown to work and to be capable of disagreeing. A harness that reports
# agreement with something it never actually ran is the failure this
# whole collection is about.
probe=$(printf '%s\n' "$(printf 'hello world' | od -An -tx1 | tr -d ' \n')" |
        "$VENV/bin/python" t/oracle.py "$TOK")
eq "the oracle tokenises a known string" "31373 995" "$probe"

probe=$(printf '%s\n' "$(printf 'hello worlds' | od -An -tx1 | tr -d ' \n')" |
        "$VENV/bin/python" t/oracle.py "$TOK")
if [ "$probe" = "31373 995" ]; then
	nok "the oracle gives a DIFFERENT answer for a different string" \
	    "it returned the same ids, so it is not reading its input"
else
	ok "the oracle gives a different answer for a different string"
fi

probe=$(printf '\n' | "$VENV/bin/python" t/oracle.py "$TOK")
eq "an empty input produces no ids" "" "$probe"

# Invalid UTF-8 has no reference answer, so the rig must say so rather
# than quietly dropping the case.
probe=$(printf 'ff\n' | "$VENV/bin/python" t/oracle.py "$TOK")
eq "invalid UTF-8 is reported as unanswerable, not skipped silently" \
   "SKIP-not-utf8" "$probe"

# ------------------------------------------------- the byte-level alphabet
# 256 characters, fixed. Comparable whole rather than sampled, which is
# why it is the first thing implemented after the rig.
"$VENV/bin/python" - "$TOK" > scratch/alphabet.txt <<'PY'
import sys
from tokenizers import pre_tokenizers
for ch in pre_tokenizers.ByteLevel.alphabet():
    sys.stdout.write("%d\n" % ord(ch))
PY
n=$(sort -n scratch/alphabet.txt | uniq | wc -l)
eq "the byte-level alphabet has 256 distinct codepoints" "256" "$n"

if [ -x "$BPEC" ]; then
	ours=$("$BPEC" --alphabet | sort -n | uniq | md5sum | cut -d' ' -f1)
	theirs=$(sort -n scratch/alphabet.txt | uniq | md5sum | cut -d' ' -f1)
	eq "our byte-level alphabet is the library's, character for character" \
	   "$theirs" "$ours"
else
	skip "$BPEC is not built yet, so only the oracle side ran"
fi

# --------------------------------------------- reading the tokenizer file
if [ -x "$BPEC" ]; then
	want_v=$("$VENV/bin/python" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["model"]["vocab"]))' "$TOK")
	want_m=$("$VENV/bin/python" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["model"]["merges"]))' "$TOK")
	got=$("$BPEC" --load "$TOK")
	eq "the vocabulary size is the file's" "vocab $want_v" "$(echo "$got" | sed -n 1p)"
	eq "the merge count is the file's" "merges $want_m" "$(echo "$got" | sed -n 2p)"

	# Every unimplemented field must be refused BY NAME. A field quietly
	# ignored produces wrong ids for every input it would have touched,
	# and nothing fails until someone compares against the reference.
	# Each case is the real tokenizer file with one field changed.
	mkdir -p scratch/bad
	refuses() { # name, python edit, expected substring in the message
		"$VENV/bin/python" -c "
import json, sys
d = json.load(open(sys.argv[1]))
$2
json.dump(d, open(sys.argv[2], 'w'), ensure_ascii=False)
" "$TOK" "scratch/bad/$1.json" || { nok "refuses $1" "could not build the case"; return 0; }
		out=$("$BPEC" --load "scratch/bad/$1.json" 2>&1)
		st=$?
		if [ "$st" -eq 0 ]; then
			nok "refuses $1" "it was accepted"
		elif ! printf '%s' "$out" | grep -q "$3"; then
			nok "refuses $1" "message did not mention $3: $out"
		else
			ok "refuses $1, by name"
		fi
	}
	refuses normalizer "d['normalizer'] = {'type': 'NFC'}" "normalizer"
	refuses wordpiece "d['model']['type'] = 'WordPiece'" "WordPiece"
	refuses dropout "d['model']['dropout'] = 0.1" "dropout"
	refuses unk "d['model']['unk_token'] = '<unk>'" "unk_token"
	refuses suffix "d['model']['end_of_word_suffix'] = '</w>'" "end_of_word_suffix"
	refuses pretok "d['pre_tokenizer'] = {'type': 'Whitespace'}" "Whitespace"

	# A \u escape decoded wrongly would corrupt exactly the non-ASCII
	# vocabulary entries that byte-level BPE depends on, so it is
	# refused rather than guessed at. ensure_ascii writes every one of
	# them as \uXXXX, which makes the whole file a test case.
	"$VENV/bin/python" -c "
import json, sys
json.dump(json.load(open(sys.argv[1])), open(sys.argv[2], 'w'), ensure_ascii=True)
" "$TOK" scratch/bad/uesc.json
	out=$("$BPEC" --load scratch/bad/uesc.json 2>&1)
	if [ $? -eq 0 ]; then
		nok "refuses a file written with backslash-u escapes" "it was accepted"
	elif printf '%s' "$out" | grep -q 'u escapes'; then
		ok "refuses a file written with backslash-u escapes, by name"
	else
		nok "refuses a file written with backslash-u escapes" "$out"
	fi
fi

# ------------------------------------------------- the pre-tokenizer split
# Compared split by split, before any merging. A wrong split produces
# wrong ids with a merge table that is working perfectly, so a failure
# here has to say "the split is wrong" and not "the ids differ".
if [ -x "$BPEC" ]; then
	out=$("$VENV/bin/python" t/split_cmp.py "$BPEC" "$BPEC_RANDOM" 2>&1)
	st=$?
	line=$(printf '%s' "$out" | tail -1)
	if [ "$st" -eq 0 ]; then
		ok "$line"
	else
		nok "$line"
		printf '%s\n' "$out" | head -20 | sed 's/^/        /'
	fi
fi

# ----------------------------------------------------------- the token ids
# The claim the whole project exists for. Per input rather than batched:
# a per-input comparison names the input that disagrees, where a batched
# one reports a diff somewhere in a megabyte.
if [ -x "$BPEC" ]; then
	out=$("$VENV/bin/python" t/encode_cmp.py "$BPEC" "$TOK" "$BPEC_RANDOM" 2>&1)
	st=$?
	line=$(printf '%s' "$out" | tail -1)
	if [ "$st" -eq 0 ]; then
		ok "$line"
	else
		nok "$line"
		printf '%s\n' "$out" | head -20 | sed 's/^/        /'
	fi
fi

# ----------------------------------- the sanitizer can actually see
# A buffer larger than the data makes ASan useless for the one class
# of bug it is most wanted for. The input buffer doubles, so after
# reading 1 KB it is 64 KB, and a read one byte past the end of the
# input lands 63 KB inside a live allocation. Nothing is reported.
#
# This is not hypothetical and it is not a guess. Built both ways
# below with a deliberate one-past-the-end read planted: untrimmed,
# ASan reports nothing at all; trimmed, it reports the overflow. An
# hour-long clean ASan run was blind to the whole class before the
# trim went in.
#
# A sibling repository found the same thing independently, where
# deleting a bounds check produced 33 wrong answers and zero
# sanitizer reports.
if command -v gcc >/dev/null 2>&1; then
	"$VENV/bin/python" - bpec.c "$TMP_SAN" <<'PLANT' 2>/dev/null || true
import sys
lines = open(sys.argv[1], encoding="utf-8").read().split("\n")
idx = max(k for k, l in enumerate(lines) if l == "\twhile (i < n) {")
lines.insert(idx, "\tif (n) { volatile unsigned char probe = buf[n]; (void)probe; }")
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(lines))
PLANT
	if [ -s "$TMP_SAN" ] && gcc -std=c11 -g -O1 -w -fsanitize=address \
	     -fno-omit-frame-pointer -I. -o "$TMP_SAN.bin" "$TMP_SAN" 2>/dev/null; then
		hits=$(printf 'hello' | "$TMP_SAN.bin" --encode "$TOK" 2>&1 |
		       grep -c 'heap-buffer-overflow' || true)
		if [ "$hits" -gt 0 ]; then
			ok "a read one byte past the input is visible to ASan"
		else
			nok "a read one byte past the input is visible to ASan" \
			    "ASan reported nothing, so the input buffer is larger than the input again"
		fi
	else
		echo "SKIP  could not build the sanitizer probe"
	fi
	rm -f "$TMP_SAN" "$TMP_SAN.bin"
fi

# ------------------------------------------------ megabytes of real text
# Volume, and real text specifically. Random strings almost never
# contain a sequence the merges were trained on, so a random corpus
# can agree perfectly while saying nothing about the part of the merge
# table that real input actually reaches.
#
# Compared per file, because a line is one subprocess and four
# megabytes is a hundred thousand of them. On a mismatch it bisects to
# the smallest failing line, so a failure still names something a
# person can read rather than "somewhere in 4 MB".
#
# The corpus is not vendored, not downloaded, and differs between
# machines, so the suite reports what it swept rather than asserting a
# number. It skips loudly when there is nothing large to read.
if [ -x "$BPEC" ]; then
	out=$("$VENV/bin/python" t/corpus_cmp.py "$BPEC" "$TOK" \
	      "$BPEC_CORPUS_BYTES" 2>&1)
	st=$?
	line=$(printf '%s' "$out" | tail -1)
	case "$line" in
		SKIP*) printf 'SKIP  %s\n' "$line" ;;
		*) if [ "$st" -eq 0 ]; then
		     ok "$line"
		   else
		     nok "$line"
		     printf '%s\n' "$out" | head -14 | sed 's/^/        /'
		   fi ;;
	esac
fi

# ---------------------------------------------------------- decode# ---------------------------------------------------------- decode
# The ids come from the library and so does the expected text, so
# this tests the decoder on its own rather than testing encode and
# decode together and letting two matching mistakes cancel.
#
# The expected text is decode(ids, skip_special_tokens=False) and not
# the original input. Encode is not invertible when an added token
# sets lstrip: "a<s>  <mask>b" has its two spaces absorbed into the
# <mask> match and the token carries no spaces, so nothing can put
# them back. The library answers "a<s><mask>b" and so do we.
# Comparing against the input instead reported fourteen failures that
# were the tokenizer working as designed.
if [ -x "$BPEC" ]; then
	out=$("$VENV/bin/python" t/decode_cmp.py "$BPEC" "$TOK" "$BPEC_RANDOM" 2>&1)
	st=$?
	line=$(printf '%s' "$out" | tail -1)
	if [ "$st" -eq 0 ]; then
		ok "$line"
	else
		nok "$line"
		printf '%s\n' "$out" | head -20 | sed 's/^/        /'
	fi

	# An id the vocabulary does not have is refused. Emitting nothing
	# for it, or a replacement character, would turn a corrupt id
	# stream into plausible-looking text.
	for bad in 99999 50257 -1 "not-a-number"; do
		out=$(printf '%s\n' "$bad" | "$BPEC" --decode "$TOK" 2>&1 >/dev/null)
		if [ $? -eq 0 ]; then
			nok "refuses [$bad] as a token id" "it was accepted"
		elif [ -z "$out" ]; then
			nok "refuses [$bad] as a token id" "it failed without saying why"
		else
			ok "refuses [$bad] as a token id"
		fi
	done

	# A vocabulary entry holding a codepoint the byte alphabet never
	# produces. Unreachable with any real file, so a mutation that
	# deletes the guard survives; the file is constructed instead.
	# Decoding it to a replacement character, or to nothing, would
	# turn a corrupt vocabulary into plausible text.
	"$VENV/bin/python" -c "
import json, sys
d = json.load(open(sys.argv[1]))
v = d['model']['vocab']
victim = [k for k, i in v.items() if i == 500][0]
del v[victim]
v['\u03a9x'] = 500
json.dump(d, open(sys.argv[2], 'w'), ensure_ascii=False)
" "$TOK" scratch/bad/outside.json
	out=$(printf '500\n' | "$BPEC" --decode scratch/bad/outside.json 2>&1 >/dev/null)
	if [ $? -eq 0 ]; then
		nok "refuses a vocabulary entry outside the byte alphabet" "it was accepted"
	elif printf '%s' "$out" | grep -q 'not in the byte alphabet'; then
		ok "refuses a vocabulary entry outside the byte alphabet, by name"
	else
		nok "refuses a vocabulary entry outside the byte alphabet" "$out"
	fi

	# The inverse byte map is not checked separately here. Every one
	# of the 256 byte values is a round-trip case in decode_cmp.py,
	# so a wrong inverse fails there with the byte named. A count of
	# how many bytes exist would have passed without touching bpec at
	# all, which is the kind of test this repository is about not
	# writing.
fi

# ------------------------------------------------------- hostile input# ------------------------------------------------------- hostile input
# Byte-level BPE is defined for all 256 byte values, so there is no
# input bpec may refuse. The library cannot be the oracle for the half
# that is not valid UTF-8, so what is checked there is what holds
# regardless: it terminates, exits 0, emits ids inside the vocabulary,
# and gives the same answer twice. The valid half is still compared
# against the reference rather than waved through.
if [ -x "$BPEC" ]; then
	out=$("$VENV/bin/python" t/hostile.py "$BPEC" "$TOK" 2>&1)
	st=$?
	line=$(printf '%s' "$out" | tail -1)
	if [ "$st" -eq 0 ]; then
		ok "$line"
	else
		nok "$line"
		printf '%s\n' "$out" | head -20 | sed 's/^/        /'
	fi
fi

# ------------------------------------------- a second tokenizer family
# Everything above is GPT-2, which is one vocabulary and therefore one
# data point. RoBERTa is the same byte-level BPE over a different
# vocabulary, with five added tokens instead of one, and <mask> sets
# lstrip, which no GPT-2 file exercises.
#
# Its post-processor wraps output in <s> and </s>. That is a separate
# stage bpec does not implement, so every comparison here passes
# add_special_tokens=False and the README says post-processing is out
# of scope. Comparing against it would be grading this program on a
# feature it says it does not have.
if [ -x "$BPEC" ] && fetch_tok "$TOK2" "$TOK2_URL" "$TOK2_SHA"; then
	want_v=$("$VENV/bin/python" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["model"]["vocab"]))' "$TOK2")
	want_m=$("$VENV/bin/python" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["model"]["merges"]))' "$TOK2")
	got=$("$BPEC" --load "$TOK2")
	eq "the second family's vocabulary size is its file's" \
	   "vocab $want_v" "$(echo "$got" | sed -n 1p)"
	eq "the second family's merge count is its file's" \
	   "merges $want_m" "$(echo "$got" | sed -n 2p)"

	# lstrip makes the added-token match absorb the whitespace in front
	# of it, so "a  <mask>" is "a" and "  <mask>" rather than "a", two
	# spaces, and the token. <s> does not set it and is the control:
	# its space stays a separate token. Named separately from the
	# corpus run so a failure says lstrip and not "ids differ".
	for t in "a <mask>" "a  <mask> b" "a<mask>b" "<mask>" " <mask>" \
	         "a <s> b" " <s>" "a\t\n<mask>"; do
		hex=$(printf '%b' "$t" | od -An -tx1 | tr -d ' \n')
		want=$(printf '%s\n' "$hex" | "$VENV/bin/python" t/oracle.py "$TOK2")
		ours=$(printf '%b' "$t" | "$BPEC" --encode "$TOK2" | tr '\n' ' ' | sed 's/ $//')
		eq "lstrip: [$t]" "$want" "$ours"
	done

	# rstrip is the mirror of lstrip and no real file here sets it, so
	# the code for it would be untested against anything. The oracle
	# will read a file we construct, so the file is constructed: the
	# real RoBERTa vocabulary with rstrip added to <s>. The library
	# honours the flag and answers for it, which makes this a
	# comparison and not an assertion about what we believe.
	#
	# "a<s>  <mask>b" is the case that matters, and it is why the
	# backward scan stops at the start of the segment rather than at
	# the start of the input: <s> takes both spaces, and <mask> must
	# not take them a second time.
	"$VENV/bin/python" -c "
import json, sys
d = json.load(open(sys.argv[1]))
for a in d['added_tokens']:
    if a['content'] == '<s>':
        a['rstrip'] = True
json.dump(d, open(sys.argv[2], 'w'), ensure_ascii=False)
" "$TOK2" scratch/roberta-rstrip.json
	for t in "a<s>  b" "a<s>  <mask>b" "a <s>  <mask> b" "<s>   " "a<s>b"; do
		hex=$(printf '%b' "$t" | od -An -tx1 | tr -d ' \n')
		want=$(printf '%s\n' "$hex" |
		       "$VENV/bin/python" t/oracle.py scratch/roberta-rstrip.json)
		ours=$(printf '%b' "$t" |
		       "$BPEC" --encode scratch/roberta-rstrip.json |
		       tr '\n' ' ' | sed 's/ $//')
		eq "rstrip: [$t]" "$want" "$ours"
	done

	out=$("$VENV/bin/python" t/encode_cmp.py "$BPEC" "$TOK2" "$BPEC_RANDOM" 2>&1)
	st=$?
	line=$(printf '%s' "$out" | tail -1)
	if [ "$st" -eq 0 ]; then
		ok "second family, $line"
	else
		nok "second family, $line"
		printf '%s\n' "$out" | head -20 | sed 's/^/        /'
	fi

	out=$("$VENV/bin/python" t/corpus_cmp.py "$BPEC" "$TOK2" \
	      "$BPEC_CORPUS_BYTES" 2>&1)
	st=$?
	line=$(printf '%s' "$out" | tail -1)
	case "$line" in
		SKIP*) printf 'SKIP  second family, %s\n' "$line" ;;
		*) if [ "$st" -eq 0 ]; then
		     ok "second family, $line"
		   else
		     nok "second family, $line"
		     printf '%s\n' "$out" | head -14 | sed 's/^/        /'
		   fi ;;
	esac

	out=$("$VENV/bin/python" t/decode_cmp.py "$BPEC" "$TOK2" "$BPEC_RANDOM" 2>&1)
	st=$?
	line=$(printf '%s' "$out" | tail -1)
	if [ "$st" -eq 0 ]; then
		ok "second family, $line"
	else
		nok "second family, $line"
		printf '%s\n' "$out" | head -20 | sed 's/^/        /'
	fi

	out=$("$VENV/bin/python" t/hostile.py "$BPEC" "$TOK2" 2>&1)
	st=$?
	line=$(printf '%s' "$out" | tail -1)
	if [ "$st" -eq 0 ]; then
		ok "second family, $line"
	else
		nok "second family, $line"
		printf '%s\n' "$out" | head -20 | sed 's/^/        /'
	fi
elif [ -x "$BPEC" ]; then
	skip "the second tokenizer family could not be fetched"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
