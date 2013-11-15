/*
 * Copyright (c) 2013 Damien Miller <djm@mindrot.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/time.h> /* needed for misc.h */
#include <stdarg.h> /* needed for log.h */
#include <string.h>
#include <stdio.h>  /* needed for misc.h */

#include "log.h"
#include "misc.h"
#include "chacha20poly1305aead.h"

void cp_aead_init(struct chacha_poly_aead_ctx *ctx,
    const u_char *key, u_int keylen)
{
	if (keylen != (32 + 32)) /* 2 x 256 bit keys */
		fatal("%s: invalid keylen %u", __func__, keylen);
	chacha_keysetup(&ctx->main_ctx, key, 256);
	chacha_keysetup(&ctx->header_ctx, key + 32, 256);
}

/*
 * cp_aead_crypt() operates as following:
 * Copy 'aadlen' bytes (without en/decryption) from 'src' to 'dest'.
 * Theses bytes are treated as additional authenticated data.
 * En/Decrypt 'len' bytes at offset 'aadlen' from 'src' to 'dest'.
 * Use POLY1305_TAGLEN bytes at offset 'len'+'aadlen' as the
 * authentication tag.
 * This tag is written on encryption and verified on decryption.
 * Both 'aadlen' and 'authlen' can be set to 0.
 */
int
cp_aead_crypt(struct chacha_poly_aead_ctx *ctx, u_int seqnr, u_char *dest,
    const u_char *src, u_int len, u_int aadlen, u_int authlen, int do_encrypt)
{
	u_char seqbuf[8];
	u_char one[8] = { 1, 0, 0, 0, 0, 0, 0, 0 }; /* NB. little-endian */
	u_char expected_tag[POLY1305_TAGLEN], poly_key[POLY1305_KEYLEN];
	int r = -1;

	/*
	 * Run ChaCha20 once to generate the Poly1305 key. The IV is the
	 * packet sequence number.
	 */
	bzero(poly_key, sizeof(poly_key));
	put_u64(seqbuf, seqnr);
	chacha_ivsetup(&ctx->main_ctx, seqbuf, NULL);
	chacha_encrypt_bytes(&ctx->main_ctx,
	    poly_key, poly_key, sizeof(poly_key));
	/* Set Chacha's block counter to 1 */
	chacha_ivsetup(&ctx->main_ctx, seqbuf, one);

	/* If decrypting, check tag before anything else */
	if (!do_encrypt) {
		const u_char *tag = src + aadlen + len;

		poly1305_auth(expected_tag, src, aadlen + len, poly_key);
		if (!timingsafe_bcmp(expected_tag, tag, POLY1305_TAGLEN))
			goto out;
	}
	/* Crypt additional data */
        if (aadlen) {
		chacha_ivsetup(&ctx->header_ctx, seqbuf, NULL);
		chacha_encrypt_bytes(&ctx->header_ctx, src, dest, aadlen);
	}
	chacha_encrypt_bytes(&ctx->main_ctx, src + aadlen,
	    dest + aadlen, len);

	/* If encrypting, calculate and append tag */
	if (do_encrypt) {
		poly1305_auth(dest + aadlen + len, dest, aadlen + len,
		    poly_key);
	}
	r = 0;

 out:
	bzero(expected_tag, sizeof(expected_tag));
	bzero(seqbuf, sizeof(seqbuf));
	bzero(poly_key, sizeof(poly_key));
	return r;
}

int
cp_aead_get_length(struct chacha_poly_aead_ctx *ctx,
    u_int *plenp, u_int seqnr, const u_char *cp, u_int len)
{
	u_char buf[4], seqbuf[8];

	if (len < 4)
		return -1; /* Insufficient length */
	put_u64(seqbuf, seqnr);
	chacha_ivsetup(&ctx->header_ctx, seqbuf, NULL);
	chacha_encrypt_bytes(&ctx->header_ctx, cp, buf, 4);
	*plenp = get_u32(buf);
	return 0;
}

