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
 */
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

static void usage(void)
{
	fputs("usage: bpec --alphabet\n", stderr);
}

int main(int argc, char **argv)
{
	build_byte_map();

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
