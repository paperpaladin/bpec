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
VENV=scratch/venv
LIB_VERSION=0.23.3

pass=0
fail=0
ok()   { pass=$((pass + 1)); printf 'ok    %s\n' "$1"; }
nok()  { fail=$((fail + 1)); printf 'FAIL  %s\n' "$1"
         [ $# -gt 1 ] && printf '        %s\n' "$2"; return 0; }
skip() { printf 'SKIP  %s\n' "$1"; }
eq()   { if [ "$2" = "$3" ]; then ok "$1"; else nok "$1" "want [$2] got [$3]"; fi; }

mkdir -p scratch

# ---------------------------------------------------------- the tokenizer
if [ ! -f "$TOK" ]; then
	command -v curl >/dev/null 2>&1 || {
		echo "SKIP: curl is not installed, so the oracle cannot be fetched"
		exit 0; }
	curl -sSL --max-time 180 -o "$TOK.part" "$TOK_URL" 2>/dev/null || {
		rm -f "$TOK.part"
		echo "SKIP: cannot reach huggingface.co, so there is no oracle to check against"
		exit 0; }
	mv "$TOK.part" "$TOK"
fi
got=$(sha256sum "$TOK" | cut -d' ' -f1)
if [ "$got" != "$TOK_SHA" ]; then
	echo "FAIL: tokenizer.json sha256 is $got, expected $TOK_SHA" >&2
	echo "  the pin exists so a result cannot shift under a changed vocabulary." >&2
	rm -f "$TOK"
	exit 1
fi

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

if [ -x ./bpec ]; then
	ours=$(./bpec --alphabet | sort -n | uniq | md5sum | cut -d' ' -f1)
	theirs=$(sort -n scratch/alphabet.txt | uniq | md5sum | cut -d' ' -f1)
	eq "our byte-level alphabet is the library's, character for character" \
	   "$theirs" "$ours"
else
	skip "bpec is not built yet, so only the oracle side ran"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
