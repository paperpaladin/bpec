/* bpec: byte-level BPE tokenisation, matching the HuggingFace library
 * token for token.
 *
 * One C11 file, no dependencies. Tokenising should not need a Python
 * runtime and 200 MB of wheels.
 *
 * The claim is exactness, not approximation. A tokenizer that differs on
 * one input in ten thousand produces a model that works until it does
 * not, and the difference is invisible to every test that does not
 * compare ids directly.
 *
 * Usage: bpec --alphabet
 *        bpec --load TOKENIZER_JSON
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "classes.h"

/* Byte-level BPE does not feed raw bytes to the merge table. It maps
 * each of the 256 byte values to one printable codepoint first, so that
 * the merge table is over characters with no whitespace or control
 * characters in it, and so that every byte sequence is representable
 * including the ones that are not valid UTF-8.
 *
 * The mapping is not arbitrary and is not a table to be typed in. Bytes
 * that are already printable and unambiguous stand for themselves:
 * '!' through '~', then two Latin-1 runs. Every remaining byte, 68 of
 * them, is assigned the next codepoint from 0x100 upward in byte order.
 * Deriving it is a dozen lines; typing it is 256 chances to be wrong in
 * a way that only shows up on one input in a million.
 */
#define BYTE_ALPHABET 256

static int byte_to_cp[BYTE_ALPHABET];

static int is_self_representing(int b)
{
	return (b >= '!' && b <= '~') ||
	       (b >= 0xA1 && b <= 0xAC) ||
	       (b >= 0xAE && b <= 0xFF);
}

static void build_byte_map(void)
{
	int b, next = 0x100;

	for (b = 0; b < BYTE_ALPHABET; b++)
		byte_to_cp[b] = is_self_representing(b) ? b : -1;
	for (b = 0; b < BYTE_ALPHABET; b++)
		if (byte_to_cp[b] < 0)
			byte_to_cp[b] = next++;
}

/* The alphabet is the set of codepoints the map produces, which is the
 * thing the library exposes and so the thing that can be compared whole
 * rather than sampled. Printed one per line, unsorted: the harness
 * sorts both sides, because agreeing on the set is the claim and
 * agreeing on an order would be a claim about an implementation detail.
 */
static void print_alphabet(void)
{
	int b;

	for (b = 0; b < BYTE_ALPHABET; b++)
		printf("%d\n", byte_to_cp[b]);
}


/* ------------------------------------------------------------ JSON
 *
 * Enough JSON to read a tokenizer file and not a byte more. The file is
 * the input and most of the ways this goes wrong are in reading it, so
 * every construct it does not handle is refused by name rather than
 * skipped: a field quietly ignored produces wrong ids for every input
 * that would have touched it, and nothing fails until someone compares
 * against the reference.
 */
typedef struct {
	const char *p, *end;
	const char *err;
} Json;

static void jerr(Json *j, const char *m) { if (!j->err) j->err = m; }

static void jspace(Json *j)
{
	while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' ||
	                         *j->p == '\n' || *j->p == '\r'))
		j->p++;
}

static int jpeek(Json *j) { jspace(j); return j->p < j->end ? *j->p : -1; }

static int jtake(Json *j, char c)
{
	if (jpeek(j) != c) return 0;
	j->p++;
	return 1;
}

/* A string, unescaped into `out` (which must hold at least the source
 * length). Only \" and \\ are implemented. A \u escape is refused by
 * name: decoding it wrongly would corrupt exactly the non-ASCII
 * vocabulary entries that byte-level BPE depends on, and silently. */
static int jstring(Json *j, char *out, size_t cap, size_t *len)
{
	size_t n = 0;

	if (!jtake(j, '"')) { jerr(j, "expected a string"); return 0; }
	while (j->p < j->end && *j->p != '"') {
		char c = *j->p++;
		if (c == '\\') {
			if (j->p >= j->end) { jerr(j, "string ends inside an escape"); return 0; }
			c = *j->p++;
			if (c == 'u') {
				jerr(j, "\\u escapes are not implemented; this file needs them");
				return 0;
			}
			if (c != '"' && c != '\\') {
				jerr(j, "unsupported escape in a string");
				return 0;
			}
		}
		if (n + 1 >= cap) { jerr(j, "string longer than its buffer"); return 0; }
		out[n++] = c;
	}
	if (!jtake(j, '"')) { jerr(j, "unterminated string"); return 0; }
	out[n] = 0;
	if (len) *len = n;
	return 1;
}

static int jskip(Json *j);

static int jskip_container(Json *j, char open, char close)
{
	if (!jtake(j, open)) return 0;
	if (jtake(j, close)) return 1;
	for (;;) {
		if (open == '{') {
			char tmp[512];
			if (!jstring(j, tmp, sizeof tmp, NULL)) return 0;
			if (!jtake(j, ':')) { jerr(j, "expected ':' in an object"); return 0; }
		}
		if (!jskip(j)) return 0;
		if (jtake(j, ',')) continue;
		if (jtake(j, close)) return 1;
		jerr(j, "expected ',' or a closing bracket");
		return 0;
	}
}

static int jskip(Json *j)
{
	int c = jpeek(j);

	if (j->err) return 0;
	if (c == '{') return jskip_container(j, '{', '}');
	if (c == '[') return jskip_container(j, '[', ']');
	if (c == '"') { char t[4096]; return jstring(j, t, sizeof t, NULL); }
	if (c == -1) { jerr(j, "input ended where a value was expected"); return 0; }
	/* null, true, false, or a number: a run of value characters. */
	while (j->p < j->end && !strchr(",}] \t\n\r", *j->p)) j->p++;
	return 1;
}


/* --------------------------------------------------- the tokenizer file */

typedef struct {
	char *tok;       /* the vocabulary entry, byte-level encoded */
	int   id;
} Vocab;

typedef struct {
	char *left, *right;
	int   rank;      /* lower merges first; the index in the file */
} Merge;

static Vocab *vocab;
static size_t vocab_n;
static Merge *merges;
static int refused;   /* an unimplemented field was named and rejected */

/* Added tokens are matched against the raw input BEFORE the
 * pre-tokenizer sees it, and their id is emitted whole. They are not
 * reachable by merging: "<|endoftext|>" tokenises as ordinary text if
 * it is not matched here first, which is what the suite caught. */
typedef struct { char *content; int id; } Added;
static Added *added;
static size_t added_n;
static size_t merges_n;

static char *xdup(const char *s, size_t n)
{
	char *d = malloc(n + 1);
	if (!d) { fputs("bpec: out of memory\n", stderr); exit(1); }
	memcpy(d, s, n);
	d[n] = 0;
	return d;
}

/* A field this program does not implement must be refused by name. The
 * alternative is reading a Llama file, ignoring its normalizer, and
 * producing ids that are wrong for every input containing a character
 * the normalizer would have touched, with nothing failing anywhere. */
static int want_null(Json *j, const char *field)
{
	jspace(j);
	if (j->end - j->p >= 4 && !strncmp(j->p, "null", 4)) { j->p += 4; return 1; }
	fprintf(stderr, "bpec: this file sets %s, which is not implemented\n", field);
	refused = 1;
	return 0;
}

static int want_empty_string(Json *j, const char *field)
{
	char buf[256];
	size_t n;

	jspace(j);
	if (j->end - j->p >= 4 && !strncmp(j->p, "null", 4)) { j->p += 4; return 1; }
	if (!jstring(j, buf, sizeof buf, &n)) return 0;
	if (n == 0) return 1;
	fprintf(stderr, "bpec: this file sets %s to \"%s\", which is not implemented\n",
	        field, buf);
	refused = 1;
	return 0;
}

static int load_model(Json *j)
{
	char key[512], val[4096];
	size_t klen, vlen;

	if (!jtake(j, '{')) { jerr(j, "model is not an object"); return 0; }
	if (jtake(j, '}')) return 1;
	for (;;) {
		if (!jstring(j, key, sizeof key, &klen)) return 0;
		if (!jtake(j, ':')) { jerr(j, "expected ':' in model"); return 0; }

		if (!strcmp(key, "vocab")) {
			size_t cap = 4096;
			vocab = malloc(cap * sizeof *vocab);
			if (!vocab) { fputs("bpec: out of memory\n", stderr); exit(1); }
			if (!jtake(j, '{')) { jerr(j, "vocab is not an object"); return 0; }
			if (!jtake(j, '}')) for (;;) {
				if (!jstring(j, val, sizeof val, &vlen)) return 0;
				if (!jtake(j, ':')) { jerr(j, "expected ':' in vocab"); return 0; }
				jspace(j);
				if (vocab_n == cap) {
					cap *= 2;
					vocab = realloc(vocab, cap * sizeof *vocab);
					if (!vocab) { fputs("bpec: out of memory\n", stderr); exit(1); }
				}
				vocab[vocab_n].tok = xdup(val, vlen);
				vocab[vocab_n].id = (int)strtol(j->p, (char **)&j->p, 10);
				vocab_n++;
				if (jtake(j, ',')) continue;
				if (jtake(j, '}')) break;
				jerr(j, "expected ',' or '}' in vocab");
				return 0;
			}
		} else if (!strcmp(key, "merges")) {
			size_t cap = 4096;
			merges = malloc(cap * sizeof *merges);
			if (!merges) { fputs("bpec: out of memory\n", stderr); exit(1); }
			if (!jtake(j, '[')) { jerr(j, "merges is not an array"); return 0; }
			if (!jtake(j, ']')) for (;;) {
				char *sp;
				if (!jstring(j, val, sizeof val, &vlen)) return 0;
				/* "Ġ t": one space separates the pair, and a merge
				 * whose own halves contain a space would be
				 * ambiguous. The format does not produce those, and
				 * assuming so silently is how a parser becomes wrong
				 * on one vocabulary in ten. */
				sp = strchr(val, ' ');
				if (!sp || strchr(sp + 1, ' ')) {
					fprintf(stderr, "bpec: merge %zu is not a space-separated "
					        "pair: \"%s\"\n", merges_n, val);
					return 0;
				}
				if (merges_n == cap) {
					cap *= 2;
					merges = realloc(merges, cap * sizeof *merges);
					if (!merges) { fputs("bpec: out of memory\n", stderr); exit(1); }
				}
				merges[merges_n].left = xdup(val, (size_t)(sp - val));
				merges[merges_n].right = xdup(sp + 1, vlen - (size_t)(sp - val) - 1);
				merges[merges_n].rank = (int)merges_n;
				merges_n++;
				if (jtake(j, ',')) continue;
				if (jtake(j, ']')) break;
				jerr(j, "expected ',' or ']' in merges");
				return 0;
			}
		} else if (!strcmp(key, "dropout")) {
			if (!want_null(j, "model.dropout")) return 0;
		} else if (!strcmp(key, "unk_token")) {
			if (!want_null(j, "model.unk_token")) return 0;
		} else if (!strcmp(key, "continuing_subword_prefix")) {
			if (!want_empty_string(j, "model.continuing_subword_prefix")) return 0;
		} else if (!strcmp(key, "end_of_word_suffix")) {
			if (!want_empty_string(j, "model.end_of_word_suffix")) return 0;
		} else if (!strcmp(key, "fuse_unk")) {
			jspace(j);
			if (j->end - j->p >= 4 && !strncmp(j->p, "true", 4)) {
				fputs("bpec: this file sets model.fuse_unk, which is not implemented\n",
				      stderr);
				refused = 1;
				return 0;
			}
			if (!jskip(j)) return 0;
		} else if (!strcmp(key, "type")) {
			if (!jstring(j, val, sizeof val, &vlen)) return 0;
			if (strcmp(val, "BPE")) {
				fprintf(stderr, "bpec: model type is \"%s\"; only BPE is implemented\n",
				        val);
				refused = 1;
				return 0;
			}
		} else {
			if (!jskip(j)) return 0;
		}

		if (jtake(j, ',')) continue;
		if (jtake(j, '}')) return 1;
		jerr(j, "expected ',' or '}' in model");
		return 0;
	}
}

static int load_tokenizer(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long sz;
	Json j;
	char key[512];
	size_t klen, vlen;

	if (!f) { fprintf(stderr, "bpec: cannot open %s\n", path); return 0; }
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) { fclose(f); fprintf(stderr, "bpec: cannot size %s\n", path); return 0; }
	buf = malloc((size_t)sz + 1);
	if (!buf) { fclose(f); fputs("bpec: out of memory\n", stderr); exit(1); }
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		fclose(f); free(buf);
		fprintf(stderr, "bpec: short read on %s\n", path);
		return 0;
	}
	fclose(f);
	buf[sz] = 0;

	j.p = buf; j.end = buf + sz; j.err = NULL;
	if (!jtake(&j, '{')) { fputs("bpec: the file is not a JSON object\n", stderr); return 0; }
	if (!jtake(&j, '}')) for (;;) {
		if (!jstring(&j, key, sizeof key, &klen)) break;
		if (!jtake(&j, ':')) { jerr(&j, "expected ':' at the top level"); break; }

		if (!strcmp(key, "model")) {
			/* break, not return: a Json error set inside load_model
			 * has its message in j.err, and returning here threw it
			 * away. Every one of those came out as exit 1 with no
			 * output at all, which is the least useful thing a
			 * program can do. */
			if (!load_model(&j)) break;
		} else if (!strcmp(key, "added_tokens")) {
			char content[1024];
			int id = -1;
			size_t cap = 16;
			added = malloc(cap * sizeof *added);
			if (!added) { fputs("bpec: out of memory\n", stderr); exit(1); }
			if (!jtake(&j, '[')) { jerr(&j, "added_tokens is not an array"); break; }
			if (!jtake(&j, ']')) for (;;) {
				int bad_flag = 0;
				content[0] = 0; id = -1;
				if (!jtake(&j, '{')) { jerr(&j, "added token is not an object"); break; }
				if (!jtake(&j, '}')) for (;;) {
					if (!jstring(&j, key, sizeof key, &klen)) break;
					if (!jtake(&j, ':')) { jerr(&j, "expected ':'"); break; }
					if (!strcmp(key, "content")) {
						if (!jstring(&j, content, sizeof content, &vlen)) break;
					} else if (!strcmp(key, "id")) {
						jspace(&j);
						id = (int)strtol(j.p, (char **)&j.p, 10);
					} else if (!strcmp(key, "lstrip") || !strcmp(key, "rstrip") ||
					           !strcmp(key, "single_word")) {
						/* Each of these changes which text the token
						 * matches. GPT-2 sets all three false; a file
						 * that sets one is refused rather than matched
						 * as if it had not. */
						jspace(&j);
						if (j.end - j.p >= 4 && !strncmp(j.p, "true", 4))
							bad_flag = 1;
						if (!jskip(&j)) break;
					} else if (!jskip(&j)) break;
					if (jtake(&j, ',')) continue;
					if (jtake(&j, '}')) break;
					jerr(&j, "expected ',' or '}' in an added token");
					break;
				}
				if (bad_flag) {
					fprintf(stderr, "bpec: added token \"%s\" sets lstrip, "
					        "rstrip or single_word, which is not implemented\n",
					        content);
					refused = 1;
					return 0;
				}
				if (content[0] && id >= 0) {
					if (added_n == cap) {
						cap *= 2;
						added = realloc(added, cap * sizeof *added);
						if (!added) { fputs("bpec: out of memory\n", stderr); exit(1); }
					}
					added[added_n].content = xdup(content, strlen(content));
					added[added_n].id = id;
					added_n++;
				}
				if (jtake(&j, ',')) continue;
				if (jtake(&j, ']')) break;
				jerr(&j, "expected ',' or ']' in added_tokens");
				break;
			}
		} else if (!strcmp(key, "normalizer")) {
			if (!want_null(&j, "normalizer")) return 0;
		} else if (!strcmp(key, "pre_tokenizer")) {
			/* Only the type is checked here. add_prefix_space and
			 * trim_offsets change the answer and are item 04's
			 * business; refusing the whole file for them now would
			 * be refusing GPT-2. */
			const char *save = j.p;
			char t[256];
			int seen_type = 0;
			if (!jtake(&j, '{')) { jerr(&j, "pre_tokenizer is not an object"); break; }
			if (!jtake(&j, '}')) for (;;) {
				if (!jstring(&j, key, sizeof key, &klen)) return 0;
				if (!jtake(&j, ':')) { jerr(&j, "expected ':'"); return 0; }
				if (!strcmp(key, "type")) {
					if (!jstring(&j, t, sizeof t, &vlen)) return 0;
					seen_type = 1;
					if (strcmp(t, "ByteLevel")) {
						fprintf(stderr, "bpec: pre_tokenizer is \"%s\"; only "
						        "ByteLevel is implemented\n", t);
						refused = 1;
						return 0;
					}
				} else if (!jskip(&j)) return 0;
				if (jtake(&j, ',')) continue;
				if (jtake(&j, '}')) break;
				jerr(&j, "expected ',' or '}' in pre_tokenizer");
				return 0;
			}
			if (!seen_type) {
				fputs("bpec: pre_tokenizer has no type\n", stderr);
				return 0;
			}
			(void)save;
		} else {
			if (!jskip(&j)) break;
		}

		if (jtake(&j, ',')) continue;
		if (jtake(&j, '}')) break;
		jerr(&j, "expected ',' or '}' at the top level");
		break;
	}
	if (j.err) {
		fprintf(stderr, "bpec: %s (at byte %ld)\n", j.err, (long)(j.p - buf));
		free(buf);
		return 0;
	}
	/* Every string taken from this file was copied out with xdup, so
	 * the file itself is dead the moment parsing finishes. Holding
	 * 1.3 MB for the life of the process is not a design choice, it
	 * is just a leak, and LeakSanitizer said so. */
	free(buf);
	buf = NULL;

	/* A refusal is a refusal whenever it happened. The "type" field
	 * comes AFTER vocab and merges in a real file, so by the time it
	 * is rejected the vocabulary is already loaded, and gating only on
	 * "did we get a vocabulary" returned success while printing "only
	 * BPE is implemented" immediately above it. A refusal that does
	 * not refuse is worse than no check. */
	if (refused) return 0;

	/* A refusal printed its own message and left j.err clear. Anything
	 * else that got here without a vocabulary failed without
	 * explaining itself, and that is worth saying. */
	if (!vocab_n) {
		fputs("bpec: the file has no vocabulary\n", stderr);
		return 0;
	}
	return 1;
}


/* ------------------------------------------------- the pre-tokenizer
 *
 * GPT-2 splits text before any merging, with this pattern:
 *
 *   's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
 *
 * Alternation is first match wins, left to right, so the order below is
 * the pattern's order and changing it changes the answer. This is where
 * exactness is won or lost: get the split wrong and every id after it is
 * wrong, with the merge table working perfectly on the wrong pieces.
 *
 * Hand written rather than run through a regex engine. The pattern is
 * fixed and five of its six alternatives are a character class and a
 * plus, so a scanner is smaller than an engine and has no lookahead
 * machinery to get wrong. The one construct that needs care is
 * \s+(?!\S), and it gets its own comment below.
 */

static int in_ranges(unsigned cp, const CpRange *r, size_t n)
{
	size_t lo = 0, hi = n;

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (cp < r[mid].lo) hi = mid;
		else if (cp > r[mid].hi) lo = mid + 1;
		else return 1;
	}
	return 0;
}

static int is_letter(unsigned cp) { return in_ranges(cp, uni_letter, UNI_LETTER_N); }
static int is_number(unsigned cp) { return in_ranges(cp, uni_number, UNI_NUMBER_N); }
static int is_space(unsigned cp)  { return in_ranges(cp, uni_space,  UNI_SPACE_N); }

/* One UTF-8 codepoint. Returns its length, or 0 for anything malformed.
 * Malformed input is not an error here: the byte-level mapping is
 * defined for all 256 byte values, so a bad byte is a one-byte piece
 * that merges like any other. Refusing it would make bpec unable to
 * tokenise exactly the inputs that byte-level BPE exists to handle. */
static int utf8_next(const unsigned char *p, size_t n, unsigned *cp)
{
	if (n == 0) return 0;
	if (p[0] < 0x80) { *cp = p[0]; return 1; }
	if ((p[0] & 0xE0) == 0xC0 && n >= 2 && (p[1] & 0xC0) == 0x80) {
		*cp = (unsigned)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
		return *cp >= 0x80 ? 2 : 0;
	}
	if ((p[0] & 0xF0) == 0xE0 && n >= 3 &&
	    (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
		*cp = (unsigned)(p[0] & 0x0F) << 12 | (unsigned)(p[1] & 0x3F) << 6 |
		      (p[2] & 0x3F);
		return (*cp >= 0x800 && !(*cp >= 0xD800 && *cp <= 0xDFFF)) ? 3 : 0;
	}
	if ((p[0] & 0xF8) == 0xF0 && n >= 4 && (p[1] & 0xC0) == 0x80 &&
	    (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
		*cp = (unsigned)(p[0] & 0x07) << 18 | (unsigned)(p[1] & 0x3F) << 12 |
		      (unsigned)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
		return (*cp >= 0x10000 && *cp <= 0x10FFFF) ? 4 : 0;
	}
	return 0;
}

/* How many bytes a run of class `cls` occupies starting at p. A byte
 * that is not valid UTF-8 belongs to no Unicode class, so it ends every
 * run, which is what puts it in the "everything else" alternative. */
static size_t run_of(const unsigned char *p, size_t n, int (*cls)(unsigned))
{
	size_t i = 0;

	while (i < n) {
		unsigned cp;
		int len = utf8_next(p + i, n - i, &cp);
		if (len == 0 || !cls(cp)) break;
		i += (size_t)len;
	}
	return i;
}

static size_t run_of_other(const unsigned char *p, size_t n)
{
	size_t i = 0;

	while (i < n) {
		unsigned cp;
		int len = utf8_next(p + i, n - i, &cp);
		if (len == 0) { i += 1; continue; }   /* a stray byte is "other" */
		if (is_space(cp) || is_letter(cp) || is_number(cp)) break;
		i += (size_t)len;
	}
	return i;
}

static const char *const CONTRACTIONS[] = {
	"'s", "'t", "'re", "'ve", "'m", "'ll", "'d",
};

/* The length of the next piece, which is never zero for a non-empty
 * input. */
static size_t next_piece(const unsigned char *p, size_t n)
{
	size_t i, k, sp;
	unsigned cp;

	for (i = 0; i < sizeof CONTRACTIONS / sizeof *CONTRACTIONS; i++) {
		size_t len = strlen(CONTRACTIONS[i]);
		if (n >= len && !memcmp(p, CONTRACTIONS[i], len)) return len;
	}

	/* " ?" is one ASCII space, and only when something of the right
	 * class follows it. A space with nothing after it falls through to
	 * the whitespace alternatives, which is why "  two" splits as
	 * "Ġ" then "Ġtwo" and not as "ĠĠtwo". */
	sp = (n > 0 && p[0] == ' ') ? 1 : 0;

	if ((k = run_of(p + sp, n - sp, is_letter)) > 0) return sp + k;
	if ((k = run_of(p + sp, n - sp, is_number)) > 0) return sp + k;
	if ((k = run_of_other(p + sp, n - sp)) > 0)      return sp + k;

	k = run_of(p, n, is_space);
	if (k > 0) {
		/* Where the run's LAST codepoint starts. The lookahead
		 * backtracks by one character and this is a byte offset, so
		 * subtracting 1 splits a multi-byte space down the middle:
		 * U+00A0 came out as two pieces where the reference gives
		 * one. Found by the corpus, not by reading. */
		size_t last = 0, j = 0;
		while (j < k) {
			unsigned c2;
			int len = utf8_next(p + j, k - j, &c2);
			if (len == 0) break;
			last = j;
			j += (size_t)len;
		}
		/* \s+(?!\S) before plain \s+. The engine takes the longest
		 * run, finds the lookahead fails because a non-space
		 * follows, and backtracks one character at a time until it
		 * holds. The lookahead holds at end of input, or when the
		 * next character is itself whitespace, so the answer is the
		 * whole run when it reaches the end and one less otherwise.
		 * That single character is why "a  \n b" splits its gap as
		 * "ĠĠĊ" and leaves the final space to join "Ġb". */
		if (k == n) return k;
		if (last > 0) return last;
		return k;      /* a single character, then a non-space: plain \s+ */
	}

	/* Not valid UTF-8 and not matched above: one byte, so that the
	 * scanner always advances. */
	(void)cp;
	return 1;
}

/* One piece byte-level encoded into `out`: each byte becomes its mapped
 * codepoint, written as UTF-8. Returns the length, or 0 if it would not
 * fit. Into a buffer rather than a FILE so the merge loop can use it
 * without fmemopen, which is a glibc extension this does not need. */
static size_t encode_into(char *out, size_t cap, const unsigned char *p, size_t n)
{
	size_t i, k = 0;

	for (i = 0; i < n; i++) {
		unsigned cp = (unsigned)byte_to_cp[p[i]];
		if (k + 4 > cap) return 0;
		if (cp < 0x80) {
			out[k++] = (char)cp;
		} else if (cp < 0x800) {
			out[k++] = (char)(0xC0 | (cp >> 6));
			out[k++] = (char)(0x80 | (cp & 0x3F));
		} else {
			out[k++] = (char)(0xE0 | (cp >> 12));
			out[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[k++] = (char)(0x80 | (cp & 0x3F));
		}
	}
	out[k] = 0;
	return k;
}

static void put_encoded(const unsigned char *p, size_t n, FILE *out)
{
	char buf[4096];

	if (encode_into(buf, sizeof buf - 1, p, n))
		fwrite(buf, 1, strlen(buf), out);
}

static void split_stdin(void)
{
	static unsigned char buf[1 << 20];
	size_t n = fread(buf, 1, sizeof buf, stdin), i = 0;

	while (i < n) {
		size_t len = next_piece(buf + i, n - i);
		put_encoded(buf + i, len, stdout);
		fputc('\n', stdout);
		i += len;
	}
}


/* ------------------------------------------------------- lookup tables
 *
 * Two hash tables, both open addressed with linear probing and sized to
 * twice the entry count so the load factor stays under a half. FNV-1a
 * because the keys are short strings and the table is built once and
 * then only read.
 */
typedef struct { const char *key; int val; } Slot;

static Slot *vtab;        /* vocabulary entry -> id */
static size_t vtab_mask;
static Slot *mtab;        /* "left\x01right"  -> rank */
static size_t mtab_mask;
static char **mkeys;      /* the joined keys, owned here */

static size_t fnv1a(const char *s)
{
	size_t h = 1469598103934665603ULL;

	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 1099511628211ULL;
	}
	return h;
}

static void tab_put(Slot *t, size_t mask, const char *key, int val)
{
	size_t i = fnv1a(key) & mask;

	while (t[i].key) {
		if (!strcmp(t[i].key, key)) return;   /* first wins */
		i = (i + 1) & mask;
	}
	t[i].key = key;
	t[i].val = val;
}

static int tab_get(const Slot *t, size_t mask, const char *key, int *out)
{
	size_t i = fnv1a(key) & mask;

	while (t[i].key) {
		if (!strcmp(t[i].key, key)) { *out = t[i].val; return 1; }
		i = (i + 1) & mask;
	}
	return 0;
}

static size_t pow2_at_least(size_t n)
{
	size_t p = 16;

	while (p < n) p *= 2;
	return p;
}

static void build_tables(void)
{
	size_t i, n;

	n = pow2_at_least(vocab_n * 2);
	vtab = calloc(n, sizeof *vtab);
	if (!vtab) { fputs("bpec: out of memory\n", stderr); exit(1); }
	vtab_mask = n - 1;
	for (i = 0; i < vocab_n; i++)
		tab_put(vtab, vtab_mask, vocab[i].tok, vocab[i].id);

	n = pow2_at_least(merges_n * 2);
	mtab = calloc(n, sizeof *mtab);
	mkeys = calloc(merges_n, sizeof *mkeys);
	if (!mtab || !mkeys) { fputs("bpec: out of memory\n", stderr); exit(1); }
	mtab_mask = n - 1;
	for (i = 0; i < merges_n; i++) {
		size_t a = strlen(merges[i].left), b = strlen(merges[i].right);
		char *k = malloc(a + b + 2);
		if (!k) { fputs("bpec: out of memory\n", stderr); exit(1); }
		memcpy(k, merges[i].left, a);
		k[a] = 1;                       /* a separator no token contains */
		memcpy(k + a + 1, merges[i].right, b);
		k[a + b + 1] = 0;
		mkeys[i] = k;
		tab_put(mtab, mtab_mask, k, merges[i].rank);
	}
}

/* --------------------------------------------------------- the merge loop
 *
 * A piece starts as its individual characters and the lowest-ranked
 * adjacent pair is merged, repeatedly, until no pair has a rank. Rank
 * is position in the merges list, so lower merges first, and the order
 * is the whole algorithm: merging a higher-ranked pair first gives a
 * different and wrong answer that still looks like tokenisation.
 *
 * Quadratic in the number of symbols per piece. Pieces are words, so
 * the constant that matters is a handful of symbols, and a priority
 * queue here would be more code than it saves.
 * shortcut: O(n^2) per piece; upgrade to a linked list plus a heap of
 * candidate pairs if a profile ever says a long piece dominates.
 */
static int merge_piece(const char *piece, int *ids, int cap)
{
	/* Sized from the piece. A fixed 4096 entries silently dropped
	 * everything past the 4096th character of a longer piece, which
	 * is a wrong answer that looks like a right one: the ids it does
	 * emit are all correct. */
	static const char **sym;
	static int *len;
	static size_t sym_cap;
	char joined[1024];
	int n = 0, i, count = 0;
	size_t off = 0, plen = strlen(piece);

	if (plen + 1 > sym_cap) {
		sym_cap = plen + 1;
		sym = realloc(sym, sym_cap * sizeof *sym);
		len = realloc(len, sym_cap * sizeof *len);
		if (!sym || !len) { fputs("bpec: out of memory\n", stderr); exit(1); }
	}

	/* One symbol per codepoint, since the piece is byte-level encoded
	 * and every merge is over those characters. */
	while (off < plen) {
		unsigned cp;
		int l = utf8_next((const unsigned char *)piece + off, plen - off, &cp);
		if (l == 0) l = 1;
		sym[n] = piece + off;
		len[n] = l;
		n++;
		off += (size_t)l;
	}

	for (;;) {
		int best = -1, best_at = -1;

		for (i = 0; i + 1 < n; i++) {
			int rank;
			size_t a = (size_t)len[i], b = (size_t)len[i + 1];
			if (a + b + 2 > sizeof joined) continue;
			memcpy(joined, sym[i], a);
			joined[a] = 1;
			memcpy(joined + a + 1, sym[i + 1], b);
			joined[a + b + 1] = 0;
			if (!tab_get(mtab, mtab_mask, joined, &rank)) continue;
			if (best < 0 || rank < best) { best = rank; best_at = i; }
		}
		if (best_at < 0) break;

		/* The two symbols are adjacent in the original buffer, so the
		 * merged symbol is just the left one with a longer length. No
		 * copying, and the result keeps pointing into `piece`. */
		len[best_at] += len[best_at + 1];
		for (i = best_at + 1; i + 1 < n; i++) {
			sym[i] = sym[i + 1];
			len[i] = len[i + 1];
		}
		n--;
	}

	for (i = 0; i < n && count < cap; i++) {
		char tok[1024];
		int id;
		if ((size_t)len[i] + 1 > sizeof tok) return -1;
		memcpy(tok, sym[i], (size_t)len[i]);
		tok[len[i]] = 0;
		if (!tab_get(vtab, vtab_mask, tok, &id)) {
			/* Byte-level BPE puts all 256 single bytes in the
			 * vocabulary, so every symbol reduces to something
			 * present. A miss means the file and the algorithm
			 * disagree, and guessing an id would be worse than
			 * saying so. */
			fprintf(stderr, "bpec: no id for symbol \"%s\"\n", tok);
			return -1;
		}
		ids[count++] = id;
	}
	return count;
}

static int encode_stdin(void)
{
	/* The whole of stdin, however large. A fixed buffer here read the
	 * first megabyte and dropped the rest without a word: 1.2 MB of
	 * input produced 524289 ids and looked like a successful run.
	 * Silent truncation in a tokenizer is worse than a refusal,
	 * because the ids it did emit are all correct. */
	unsigned char *buf = NULL;
	size_t n = 0, cap = 0, i = 0, seg;
	static int ids[1 << 16];
	size_t next_added_at = 0, next_added_len = 0;
	int next_added_id = -1;
	/* Explicit, because "next_added_at < i" is false at i = 0 and the
	 * scan then never ran for the first piece. Everything still
	 * worked whenever the first piece did not reach the token, which
	 * is why every hand-written case passed and a line of this
	 * repository's own README did not. */
	int scan_valid = 0;
	char *piece = NULL;
	size_t piece_cap = 0;

	for (;;) {
		size_t got;
		if (n == cap) {
			cap = cap ? cap * 2 : (1 << 16);
			buf = realloc(buf, cap);
			if (!buf) { fputs("bpec: out of memory\n", stderr); return 1; }
		}
		got = fread(buf + n, 1, cap - n, stdin);
		n += got;
		if (got == 0) break;
	}

	build_tables();
	while (i < n) {

		size_t a;

		/* Where the next added token starts, found once and kept.
		 * The first version searched from the current position to
		 * the end of input for every piece, which is quadratic in
		 * the input: 1.2 MB of ordinary words did not finish at
		 * all. The scan runs only when the cached answer has been
		 * passed. */
		if (!scan_valid || next_added_at < i) {
			next_added_at = n;
			next_added_len = 0;
			next_added_id = -1;
			scan_valid = 1;
			for (seg = i; seg < n; seg++) {
				for (a = 0; a < added_n; a++) {
					size_t l = strlen(added[a].content);
					if (n - seg < l || memcmp(buf + seg, added[a].content, l))
						continue;
					if (l > next_added_len) {
						next_added_at = seg;
						next_added_len = l;
						next_added_id = added[a].id;
					}
				}
				if (next_added_id >= 0) break;
			}
		}

		if (next_added_id >= 0 && next_added_at == i) {
			printf("%d\n", next_added_id);
			i += next_added_len;
			scan_valid = 0;         /* force a rescan past this one */
			continue;
		}
		{
		/* Stop the ordinary path where the added token begins. */
		size_t limit = (next_added_id >= 0) ? next_added_at : n;
		size_t plen = next_piece(buf + i, limit - i);
		int got, k;

		/* Three bytes of UTF-8 per input byte at most, so the piece
		 * buffer is sized from the piece rather than fixed. A fixed
		 * 4096 refused a 5000-byte run of one character, which the
		 * reference tokenises without complaint. */
		if (plen * 3 + 1 > piece_cap) {
			piece_cap = plen * 3 + 8;
			piece = realloc(piece, piece_cap);
			if (!piece) { fputs("bpec: out of memory\n", stderr); return 1; }
		}
		if (!encode_into(piece, piece_cap - 1, buf + i, plen)) {
			fputs("bpec: a piece did not fit its own buffer\n", stderr);
			return 1;
		}

		got = merge_piece(piece, ids, (int)(sizeof ids / sizeof *ids));
		if (got < 0) return 1;
		for (k = 0; k < got; k++) printf("%d\n", ids[k]);
		i += plen;
		}
	}
	return 0;
}

static void usage(void)
{
	fputs("usage: bpec --alphabet\n"
	      "       bpec --load TOKENIZER_JSON\n"
	      "       bpec --split   (text on stdin, one piece per line)\n"
	      "       bpec --encode TOKENIZER_JSON   (text on stdin, one id per line)\n", stderr);
}

int main(int argc, char **argv)
{
	build_byte_map();

	if (argc == 3 && !strcmp(argv[1], "--encode")) {
		if (!load_tokenizer(argv[2])) return 1;
		return encode_stdin();
	}
	if (argc == 3 && !strcmp(argv[1], "--load")) {
		if (!load_tokenizer(argv[2])) return 1;
		printf("vocab %zu\nmerges %zu\n", vocab_n, merges_n);
		return 0;
	}
	if (argc != 2) {
		usage();
		return 2;
	}
	if (!strcmp(argv[1], "--alphabet")) {
		print_alphabet();
		return 0;
	}
	if (!strcmp(argv[1], "--split")) {
		split_stdin();
		return 0;
	}
	usage();
	return 2;
}
