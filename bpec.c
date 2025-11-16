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
		return 0;
	}
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

static void usage(void)
{
	fputs("usage: bpec --alphabet\n"
	      "       bpec --load TOKENIZER_JSON\n", stderr);
}

int main(int argc, char **argv)
{
	build_byte_map();

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
	usage();
	return 2;
}
