/*
 * Quick & dirty crypto testing module.
 *
 * This will only exist until we have a better testing mechanism
 * (e.g. a char device).
 *
 * Copyright (c) 2002 James Morris <jmorris@intercode.com.au>
 * Copyright (c) 2002 Jean-Francois Dive <jef@linuxbe.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * 2004-08-09 Added cipher speed tests (Reyk Floeter <reyk@vantronix.net>)
 * 2003-09-14 Rewritten by Kartikey Mahendra Bhatt
 *
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/string.h>
#include <linux/crypto.h>
#include <linux/highmem.h>
#include <linux/moduleparam.h>
#include <linux/jiffies.h>
#include <linux/timex.h>
#include <linux/interrupt.h>
#include "tcrypt.h"

/*
 * Need to kmalloc() memory for testing kmap().
 */
#define TVMEMSIZE	16384
#define XBUFSIZE	32768

/*
 * Indexes into the xbuf to simulate cross-page access.
 */
#define IDX1		37
#define IDX2		32400
#define IDX3		1
#define IDX4		8193
#define IDX5		22222
#define IDX6		17101
#define IDX7		27333
#define IDX8		3000

/*
* Used by test_cipher()
*/
#define ENCRYPT 1
#define DECRYPT 0

static unsigned int IDX[8] = { IDX1, IDX2, IDX3, IDX4, IDX5, IDX6, IDX7, IDX8 };

/*
 * Used by test_cipher_speed()
 */
static unsigned int sec;

static int mode;
static char *xbuf;
static char *tvmem;

static char *check[] = {
	"des", "md5", "des3_ede", "rot13", "sha1", "sha256", "blowfish",
	"twofish", "serpent", "sha384", "sha512", "md4", "aes", "cast6",
	"arc4", "michael_mic", "deflate", "crc32c", "tea", "xtea",
	"khazad", "wp512", "wp384", "wp256", "tnepres", "xeta", NULL
};

static void hexdump(unsigned char *buf, unsigned int len)
{
	while (len--)
		printk("%02x", *buf++);

	printk("\n");
}

static void test_hash(char *algo, struct hash_testvec *template,
		      unsigned int tcount)
{
	unsigned int i, j, k, temp;
	struct scatterlist sg[8];
	char result[64];
	struct crypto_tfm *tfm;
	struct hash_testvec *hash_tv;
	unsigned int tsize;

	printk("\ntesting %s\n", algo);

	tsize = sizeof(struct hash_testvec);
	tsize *= tcount;

	if (tsize > TVMEMSIZE) {
		printk("template (%u) too big for tvmem (%u)\n", tsize, TVMEMSIZE);
		return;
	}

	memcpy(tvmem, template, tsize);
	hash_tv = (void *)tvmem;
	tfm = crypto_alloc_tfm(algo, 0);
	if (tfm == NULL) {
		printk("failed to load transform for %s\n", algo);
		return;
	}

	for (i = 0; i < tcount; i++) {
		printk("test %u:\n", i + 1);
		memset(result, 0, 64);

		sg_set_buf(&sg[0], hash_tv[i].plaintext, hash_tv[i].psize);

		crypto_digest_init(tfm);
		crypto_digest_setkey(tfm, hash_tv[i].key, hash_tv[i].ksize);
		crypto_digest_update(tfm, sg, 1);
		crypto_digest_final(tfm, result);

		hexdump(result, crypto_tfm_alg_digestsize(tfm));
		printk("%s\n",
		       memcmp(result, hash_tv[i].digest,
			      crypto_tfm_alg_digestsize(tfm)) ?
		       "fail" : "pass");
	}

	printk("testing %s across pages\n", algo);

	/* setup the dummy buffer first */
	memset(xbuf, 0, XBUFSIZE);

	j = 0;
	for (i = 0; i < tcount; i++) {
		if (hash_tv[i].np) {
			j++;
			printk("test %u:\n", j);
			memset(result, 0, 64);

			temp = 0;
			for (k = 0; k < hash_tv[i].np; k++) {
				memcpy(&xbuf[IDX[k]],
				       hash_tv[i].plaintext + temp,
				       hash_tv[i].tap[k]);
				temp += hash_tv[i].tap[k];
				sg_set_buf(&sg[k], &xbuf[IDX[k]],
					    hash_tv[i].tap[k]);
			}

			crypto_digest_digest(tfm, sg, hash_tv[i].np, result);

			hexdump(result, crypto_tfm_alg_digestsize(tfm));
			printk("%s\n",
			       memcmp(result, hash_tv[i].digest,
				      crypto_tfm_alg_digestsize(tfm)) ?
			       "fail" : "pass");
		}
	}

	crypto_free_tfm(tfm);
}


#ifdef CONFIG_CRYPTO_HMAC

static void test_hmac(char *algo, struct hmac_testvec *template,
		      unsigned int tcount)
{
	unsigned int i, j, k, temp;
	struct scatterlist sg[8];
	char result[64];
	struct crypto_tfm *tfm;
	struct hmac_testvec *hmac_tv;
	unsigned int tsize, klen;

	tfm = crypto_alloc_tfm(algo, 0);
	if (tfm == NULL) {
		printk("failed to load transform for %s\n", algo);
		return;
	}

	printk("\ntesting hmac_%s\n", algo);

	tsize = sizeof(struct hmac_testvec);
	tsize *= tcount;
	if (tsize > TVMEMSIZE) {
		printk("template (%u) too big for tvmem (%u)\n", tsize,
		       TVMEMSIZE);
		goto out;
	}

	memcpy(tvmem, template, tsize);
	hmac_tv = (void *)tvmem;

	for (i = 0; i < tcount; i++) {
		printk("test %u:\n", i + 1);
		memset(result, 0, sizeof (result));

		klen = hmac_tv[i].ksize;
		sg_set_buf(&sg[0], hmac_tv[i].plaintext, hmac_tv[i].psize);

		crypto_hmac(tfm, hmac_tv[i].key, &klen, sg, 1, result);

		hexdump(result, crypto_tfm_alg_digestsize(tfm));
		printk("%s\n",
		       memcmp(result, hmac_tv[i].digest,
			      crypto_tfm_alg_digestsize(tfm)) ? "fail" :
		       "pass");
	}

	printk("\ntesting hmac_%s across pages\n", algo);

	memset(xbuf, 0, XBUFSIZE);

	j = 0;
	for (i = 0; i < tcount; i++) {
		if (hmac_tv[i].np) {
			j++;
			printk("test %u:\n",j);
			memset(result, 0, 64);

			temp = 0;
			klen = hmac_tv[i].ksize;
			for (k = 0; k < hmac_tv[i].np; k++) {
				memcpy(&xbuf[IDX[k]],
				       hmac_tv[i].plaintext + temp,
				       hmac_tv[i].tap[k]);
				temp += hmac_tv[i].tap[k];
				sg_set_buf(&sg[k], &xbuf[IDX[k]],
					    hmac_tv[i].tap[k]);
			}

			crypto_hmac(tfm, hmac_tv[i].key, &klen, sg,
				    hmac_tv[i].np, result);
			hexdump(result, crypto_tfm_alg_digestsize(tfm));

			printk("%s\n",
			       memcmp(result, hmac_tv[i].digest,
				      crypto_tfm_alg_digestsize(tfm)) ?
			       "fail" : "pass");
		}
	}
out:
	crypto_free_tfm(tfm);
}

#endif	/* CONFIG_CRYPTO_HMAC */

static void test_cipher(char *algo, int enc,
			struct cipher_testvec *template, unsigned int tcount)
{
	unsigned int ret, i, j, k, temp;
	unsigned int tsize;
	unsigned int iv_len;
	unsigned int len;
	char *q;
	struct crypto_blkcipher *tfm;
	char *key;
	struct cipher_testvec *cipher_tv;
	struct blkcipher_desc desc;
	struct scatterlist sg[8];
	const char *e;

	if (enc == ENCRYPT)
	        e = "encryption";
	else
		e = "decryption";

	printk("\ntesting %s %s\n", algo, e);

	tsize = sizeof (struct cipher_testvec);
	tsize *= tcount;

	if (tsize > TVMEMSIZE) {
		printk("template (%u) too big for tvmem (%u)\n", tsize,
		       TVMEMSIZE);
		return;
	}

	memcpy(tvmem, template, tsize);
	cipher_tv = (void *)tvmem;

	tfm = crypto_alloc_blkcipher(algo, 0, CRYPTO_ALG_ASYNC);

	if (IS_ERR(tfm)) {
		printk("failed to load transform for %s: %ld\n", algo,
		       PTR_ERR(tfm));
		return;
	}
	desc.tfm = tfm;
	desc.flags = 0;

	j = 0;
	for (i = 0; i < tcount; i++) {
		if (!(cipher_tv[i].np)) {
			j++;
			printk("test %u (%d bit key):\n",
			j, cipher_tv[i].klen * 8);

			crypto_blkcipher_clear_flags(tfm, ~0);
			if (cipher_tv[i].wk)
				crypto_blkcipher_set_flags(
					tfm, CRYPTO_TFM_REQ_WEAK_KEY);
			key = cipher_tv[i].key;

			ret = crypto_blkcipher_setkey(tfm, key,
						      cipher_tv[i].klen);
			if (ret) {
				printk("setkey() failed flags=%x\n",
				       crypto_blkcipher_get_flags(tfm));

				if (!cipher_tv[i].fail)
					goto out;
			}

			sg_set_buf(&sg[0], cipher_tv[i].input,
				   cipher_tv[i].ilen);

			iv_len = crypto_blkcipher_ivsize(tfm);
			if (iv_len)
				crypto_blkcipher_set_iv(tfm, cipher_tv[i].iv,
							iv_len);

			len = cipher_tv[i].ilen;
			ret = enc ?
				crypto_blkcipher_encrypt(&desc, sg, sg, len) :
				crypto_blkcipher_decrypt(&desc, sg, sg, len);

			if (ret) {
				printk("%s () failed flags=%x\n", e,
				       desc.flags);
				goto out;
			}

			q = kmap(sg[0].page) + sg[0].offset;
			hexdump(q, cipher_tv[i].rlen);

			printk("%s\n",
			       memcmp(q, cipher_tv[i].result,
				      cipher_tv[i].rlen) ? "fail" : "pass");
		}
	}

	printk("\ntesting %s %s across pages (chunking)\n", algo, e);
	memset(xbuf, 0, XBUFSIZE);

	j = 0;
	for (i = 0; i < tcount; i++) {
		if (cipher_tv[i].np) {
			j++;
			printk("test %u (%d bit key):\n",
			j, cipher_tv[i].klen * 8);

			crypto_blkcipher_clear_flags(tfm, ~0);
			if (cipher_tv[i].wk)
				crypto_blkcipher_set_flags(
					tfm, CRYPTO_TFM_REQ_WEAK_KEY);
			key = cipher_tv[i].key;

			ret = crypto_blkcipher_setkey(tfm, key,
						      cipher_tv[i].klen);
			if (ret) {
				printk("setkey() failed flags=%x\n",
				       crypto_blkcipher_get_flags(tfm));

				if (!cipher_tv[i].fail)
					goto out;
			}

			temp = 0;
			for (k = 0; k < cipher_tv[i].np; k++) {
				memcpy(&xbuf[IDX[k]],
				       cipher_tv[i].input + temp,
				       cipher_tv[i].tap[k]);
				temp += cipher_tv[i].tap[k];
				sg_set_buf(&sg[k], &xbuf[IDX[k]],
					   cipher_tv[i].tap[k]);
			}

			iv_len = crypto_blkcipher_ivsize(tfm);
			if (iv_len)
				crypto_blkcipher_set_iv(tfm, cipher_tv[i].iv,
							iv_len);

			len = cipher_tv[i].ilen;
			ret = enc ?
				crypto_blkcipher_encrypt(&desc, sg, sg, len) :
				crypto_blkcipher_decrypt(&desc, sg, sg, len);

			if (ret) {
				printk("%s () failed flags=%x\n", e,
				       desc.flags);
				goto out;
			}

			temp = 0;
			for (k = 0; k < cipher_tv[i].np; k++) {
				printk("page %u\n", k);
				q = kmap(sg[k].page) + sg[k].offset;
				hexdump(q, cipher_tv[i].tap[k]);
				printk("%s\n",
					memcmp(q, cipher_tv[i].result + temp,
						cipher_tv[i].tap[k]) ? "fail" :
					"pass");
				temp += cipher_tv[i].tap[k];
			}
		}
	}

out:
	crypto_free_blkcipher(tfm);
}

static int test_cipher_jiffies(struct blkcipher_desc *desc, int enc, char *p,
			       int blen, int sec)
{
	struct scatterlist sg[1];
	unsigned long start, end;
	int bcount;
	int ret;

	sg_set_buf(sg, p, blen);

	for (start = jiffies, end = start + sec * HZ, bcount = 0;
	     time_before(jiffies, end); bcount++) {
		if (enc)
			ret = crypto_blkcipher_encrypt(desc, sg, sg, blen);
		else
			ret = crypto_blkcipher_decrypt(desc, sg, sg, blen);

		if (ret)
			return ret;
	}

	printk("%d operations in %d seconds (%ld bytes)\n",
	       bcount, sec, (long)bcount * blen);
	return 0;
}

static int test_cipher_cycles(struct blkcipher_desc *desc, int enc, char *p,
			      int blen)
{
	struct scatterlist sg[1];
	unsigned long cycles = 0;
	int ret = 0;
	int i;

	sg_set_buf(sg, p, blen);

	local_bh_disable();
	local_irq_disable();

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		if (enc)
			ret = crypto_blkcipher_encrypt(desc, sg, sg, blen);
		else
			ret = crypto_blkcipher_decrypt(desc, sg, sg, blen);

		if (ret)
			goto out;
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		start = get_cycles();
		if (enc)
			ret = crypto_blkcipher_encrypt(desc, sg, sg, blen);
		else
			ret = crypto_blkcipher_decrypt(desc, sg, sg, blen);
		end = get_cycles();

		if (ret)
			goto out;

		cycles += end - start;
	}

out:
	local_irq_enable();
	local_bh_enable();

	if (ret == 0)
		printk("1 operation in %lu cycles (%d bytes)\n",
		       (cycles + 4) / 8, blen);

	return ret;
}

static void test_cipher_speed(char *algo, int enc, unsigned int sec,
			      struct cipher_testvec *template,
			      unsigned int tcount, struct cipher_speed *speed)
{
	unsigned int ret, i, j, iv_len;
	unsigned char *key, *p, iv[128];
	struct crypto_blkcipher *tfm;
	struct blkcipher_desc desc;
	const char *e;

	if (enc == ENCRYPT)
	        e = "encryption";
	else
		e = "decryption";

	printk("\ntesting speed of %s %s\n", algo, e);

	tfm = crypto_alloc_blkcipher(algo, 0, CRYPTO_ALG_ASYNC);

	if (IS_ERR(tfm)) {
		printk("failed to load transform for %s: %ld\n", algo,
		       PTR_ERR(tfm));
		return;
	}
	desc.tfm = tfm;
	desc.flags = 0;

	for (i = 0; speed[i].klen != 0; i++) {
		if ((speed[i].blen + speed[i].klen) > TVMEMSIZE) {
			printk("template (%u) too big for tvmem (%u)\n",
			       speed[i].blen + speed[i].klen, TVMEMSIZE);
			goto out;
		}

		printk("test %u (%d bit key, %d byte blocks): ", i,
		       speed[i].klen * 8, speed[i].blen);

		memset(tvmem, 0xff, speed[i].klen + speed[i].blen);

		/* set key, plain text and IV */
		key = (unsigned char *)tvmem;
		for (j = 0; j < tcount; j++) {
			if (template[j].klen == speed[i].klen) {
				key = template[j].key;
				break;
			}
		}
		p = (unsigned char *)tvmem + speed[i].klen;

		ret = crypto_blkcipher_setkey(tfm, key, speed[i].klen);
		if (ret) {
			printk("setkey() failed flags=%x\n",
			       crypto_blkcipher_get_flags(tfm));
			goto out;
		}

		iv_len = crypto_blkcipher_ivsize(tfm);
		if (iv_len) {
			memset(&iv, 0xff, iv_len);
			crypto_blkcipher_set_iv(tfm, iv, iv_len);
		}

		if (sec)
			ret = test_cipher_jiffies(&desc, enc, p, speed[i].blen,
						  sec);
		else
			ret = test_cipher_cycles(&desc, enc, p, speed[i].blen);

		if (ret) {
			printk("%s() failed flags=%x\n", e, desc.flags);
			break;
		}
	}

out:
	crypto_free_blkcipher(tfm);
}

static void test_digest_jiffies(struct crypto_tfm *tfm, char *p, int blen,
				int plen, char *out, int sec)
{
	struct scatterlist sg[1];
	unsigned long start, end;
	int bcount, pcount;

	for (start = jiffies, end = start + sec * HZ, bcount = 0;
	     time_before(jiffies, end); bcount++) {
		crypto_digest_init(tfm);
		for (pcount = 0; pcount < blen; pcount += plen) {
			sg_set_buf(sg, p + pcount, plen);
			crypto_digest_update(tfm, sg, 1);
		}
		/* we assume there is enough space in 'out' for the result */
		crypto_digest_final(tfm, out);
	}

	printk("%6u opers/sec, %9lu bytes/sec\n",
	       bcount / sec, ((long)bcount * blen) / sec);

	return;
}

static void test_digest_cycles(struct crypto_tfm *tfm, char *p, int blen,
			       int plen, char *out)
{
	struct scatterlist sg[1];
	unsigned long cycles = 0;
	int i, pcount;

	local_bh_disable();
	local_irq_disable();

	/* Warm-up run. */
	for (i = 0; i < 4; i++) {
		crypto_digest_init(tfm);
		for (pcount = 0; pcount < blen; pcount += plen) {
			sg_set_buf(sg, p + pcount, plen);
			crypto_digest_update(tfm, sg, 1);
		}
		crypto_digest_final(tfm, out);
	}

	/* The real thing. */
	for (i = 0; i < 8; i++) {
		cycles_t start, end;

		crypto_digest_init(tfm);

		start = get_cycles();

		for (pcount = 0; pcount < blen; pcount += plen) {
			sg_set_buf(sg, p + pcount, plen);
			crypto_digest_update(tfm, sg, 1);
		}
		crypto_digest_final(tfm, out);

		end = get_cycles();

		cycles += end - start;
	}

	local_irq_enable();
	local_bh_enable();

	printk("%6lu cycles/operation, %4lu cycles/byte\n",
	       cycles / 8, cycles / (8 * blen));

	return;
}

static void test_digest_speed(char *algo, unsigned int sec,
			      struct digest_speed *speed)
{
	struct crypto_tfm *tfm;
	char output[1024];
	int i;

	printk("\ntesting speed of %s\n", algo);

	tfm = crypto_alloc_tfm(algo, 0);

	if (tfm == NULL) {
		printk("failed to load transform for %s\n", algo);
		return;
	}

	if (crypto_tfm_alg_digestsize(tfm) > sizeof(output)) {
		printk("digestsize(%u) > outputbuffer(%zu)\n",
		       crypto_tfm_alg_digestsize(tfm), sizeof(output));
		goto out;
	}

	for (i = 0; speed[i].blen != 0; i++) {
		if (speed[i].blen > TVMEMSIZE) {
			printk("template (%u) too big for tvmem (%u)\n",
			       speed[i].blen, TVMEMSIZE);
			goto out;
		}

		printk("test%3u (%5u byte blocks,%5u bytes per update,%4u updates): ",
		       i, speed[i].blen, speed[i].plen, speed[i].blen / speed[i].plen);

		memset(tvmem, 0xff, speed[i].blen);

		if (sec)
			test_digest_jiffies(tfm, tvmem, speed[i].blen, speed[i].plen, output, sec);
		else
			test_digest_cycles(tfm, tvmem, speed[i].blen, speed[i].plen, output);
	}

out:
	crypto_free_tfm(tfm);
}

static void test_deflate(void)
{
	unsigned int i;
	char result[COMP_BUF_SIZE];
	struct crypto_tfm *tfm;
	struct comp_testvec *tv;
	unsigned int tsize;

	printk("\ntesting deflate compression\n");

	tsize = sizeof (deflate_comp_tv_template);
	if (tsize > TVMEMSIZE) {
		printk("template (%u) too big for tvmem (%u)\n", tsize,
		       TVMEMSIZE);
		return;
	}

	memcpy(tvmem, deflate_comp_tv_template, tsize);
	tv = (void *)tvmem;

	tfm = crypto_alloc_tfm("deflate", 0);
	if (tfm == NULL) {
		printk("failed to load transform for deflate\n");
		return;
	}

	for (i = 0; i < DEFLATE_COMP_TEST_VECTORS; i++) {
		int ilen, ret, dlen = COMP_BUF_SIZE;

		printk("test %u:\n", i + 1);
		memset(result, 0, sizeof (result));

		ilen = tv[i].inlen;
		ret = crypto_comp_compress(tfm, tv[i].input,
		                           ilen, result, &dlen);
		if (ret) {
			printk("fail: ret=%d\n", ret);
			continue;
		}
		hexdump(result, dlen);
		printk("%s (ratio %d:%d)\n",
		       memcmp(result, tv[i].output, dlen) ? "fail" : "pass",
		       ilen, dlen);
	}

	printk("\ntesting deflate decompression\n");

	tsize = sizeof (deflate_decomp_tv_template);
	if (tsize > TVMEMSIZE) {
		printk("template (%u) too big for tvmem (%u)\n", tsize,
		       TVMEMSIZE);
		goto out;
	}

	memcpy(tvmem, deflate_decomp_tv_template, tsize);
	tv = (void *)tvmem;

	for (i = 0; i < DEFLATE_DECOMP_TEST_VECTORS; i++) {
		int ilen, ret, dlen = COMP_BUF_SIZE;

		printk("test %u:\n", i + 1);
		memset(result, 0, sizeof (result));

		ilen = tv[i].inlen;
		ret = crypto_comp_decompress(tfm, tv[i].input,
		                             ilen, result, &dlen);
		if (ret) {
			printk("fail: ret=%d\n", ret);
			continue;
		}
		hexdump(result, dlen);
		printk("%s (ratio %d:%d)\n",
		       memcmp(result, tv[i].output, dlen) ? "fail" : "pass",
		       ilen, dlen);
	}
out:
	crypto_free_tfm(tfm);
}

static void test_available(void)
{
	char **name = check;

	while (*name) {
		printk("alg %s ", *name);
		printk((crypto_alg_available(*name, 0)) ?
			"found\n" : "not found\n");
		name++;
	}
}

static void do_test(void)
{
	switch (mode) {

	case 0:
		test_hash("md5", md5_tv_template, MD5_TEST_VECTORS);

		test_hash("sha1", sha1_tv_template, SHA1_TEST_VECTORS);

		//DES
		test_cipher("ecb(des)", ENCRYPT, des_enc_tv_template,
			    DES_ENC_TEST_VECTORS);
		test_cipher("ecb(des)", DECRYPT, des_dec_tv_template,
			    DES_DEC_TEST_VECTORS);
		test_cipher("cbc(des)", ENCRYPT, des_cbc_enc_tv_template,
			    DES_CBC_ENC_TEST_VECTORS);
		test_cipher("cbc(des)", DECRYPT, des_cbc_dec_tv_template,
			    DES_CBC_DEC_TEST_VECTORS);

		//DES3_EDE
		test_cipher("ecb(des3_ede)", ENCRYPT, des3_ede_enc_tv_template,
			    DES3_EDE_ENC_TEST_VECTORS);
		test_cipher("ecb(des3_ede)", DECRYPT, des3_ede_dec_tv_template,
			    DES3_EDE_DEC_TEST_VECTORS);

		test_hash("md4", md4_tv_template, MD4_TEST_VECTORS);

		test_hash("sha256", sha256_tv_template, SHA256_TEST_VECTORS);

		//BLOWFISH
		test_cipher("ecb(blowfish)", ENCRYPT, bf_enc_tv_template,
			    BF_ENC_TEST_VECTORS);
		test_cipher("ecb(blowfish)", DECRYPT, bf_dec_tv_template,
			    BF_DEC_TEST_VECTORS);
		test_cipher("cbc(blowfish)", ENCRYPT, bf_cbc_enc_tv_template,
			    BF_CBC_ENC_TEST_VECTORS);
		test_cipher("cbc(blowfish)", DECRYPT, bf_cbc_dec_tv_template,
			    BF_CBC_DEC_TEST_VECTORS);

		//TWOFISH
		test_cipher("ecb(twofish)", ENCRYPT, tf_enc_tv_template,
			    TF_ENC_TEST_VECTORS);
		test_cipher("ecb(twofish)", DECRYPT, tf_dec_tv_template,
			    TF_DEC_TEST_VECTORS);
		test_cipher("cbc(twofish)", ENCRYPT, tf_cbc_enc_tv_template,
			    TF_CBC_ENC_TEST_VECTORS);
		test_cipher("cbc(twofish)", DECRYPT, tf_cbc_dec_tv_template,
			    TF_CBC_DEC_TEST_VECTORS);

		//SERPENT
		test_cipher("ecb(serpent)", ENCRYPT, serpent_enc_tv_template,
			    SERPENT_ENC_TEST_VECTORS);
		test_cipher("ecb(serpent)", DECRYPT, serpent_dec_tv_template,
			    SERPENT_DEC_TEST_VECTORS);

		//TNEPRES
		test_cipher("ecb(tnepres)", ENCRYPT, tnepres_enc_tv_template,
			    TNEPRES_ENC_TEST_VECTORS);
		test_cipher("ecb(tnepres)", DECRYPT, tnepres_dec_tv_template,
			    TNEPRES_DEC_TEST_VECTORS);

		//AES
		test_cipher("ecb(aes)", ENCRYPT, aes_enc_tv_template,
			    AES_ENC_TEST_VECTORS);
		test_cipher("ecb(aes)", DECRYPT, aes_dec_tv_template,
			    AES_DEC_TEST_VECTORS);
		test_cipher("cbc(aes)", ENCRYPT, aes_cbc_enc_tv_template,
			    AES_CBC_ENC_TEST_VECTORS);
		test_cipher("cbc(aes)", DECRYPT, aes_cbc_dec_tv_template,
			    AES_CBC_DEC_TEST_VECTORS);

		//CAST5
		test_cipher("ecb(cast5)", ENCRYPT, cast5_enc_tv_template,
			    CAST5_ENC_TEST_VECTORS);
		test_cipher("ecb(cast5)", DECRYPT, cast5_dec_tv_template,
			    CAST5_DEC_TEST_VECTORS);

		//CAST6
		test_cipher("ecb(cast6)", ENCRYPT, cast6_enc_tv_template,
			    CAST6_ENC_TEST_VECTORS);
		test_cipher("ecb(cast6)", DECRYPT, cast6_dec_tv_template,
			    CAST6_DEC_TEST_VECTORS);

		//ARC4
		test_cipher("ecb(arc4)", ENCRYPT, arc4_enc_tv_template,
			    ARC4_ENC_TEST_VECTORS);
		test_cipher("ecb(arc4)", DECRYPT, arc4_dec_tv_template,
			    ARC4_DEC_TEST_VECTORS);

		//TEA
		test_cipher("ecb(tea)", ENCRYPT, tea_enc_tv_template,
			    TEA_ENC_TEST_VECTORS);
		test_cipher("ecb(tea)", DECRYPT, tea_dec_tv_template,
			    TEA_DEC_TEST_VECTORS);


		//XTEA
		test_cipher("ecb(xtea)", ENCRYPT, xtea_enc_tv_template,
			    XTEA_ENC_TEST_VECTORS);
		test_cipher("ecb(xtea)", DECRYPT, xtea_dec_tv_template,
			    XTEA_DEC_TEST_VECTORS);

		//KHAZAD
		test_cipher("ecb(khazad)", ENCRYPT, khazad_enc_tv_template,
			    KHAZAD_ENC_TEST_VECTORS);
		test_cipher("ecb(khazad)", DECRYPT, khazad_dec_tv_template,
			    KHAZAD_DEC_TEST_VECTORS);

		//ANUBIS
		test_cipher("ecb(anubis)", ENCRYPT, anubis_enc_tv_template,
			    ANUBIS_ENC_TEST_VECTORS);
		test_cipher("ecb(anubis)", DECRYPT, anubis_dec_tv_template,
			    ANUBIS_DEC_TEST_VECTORS);
		test_cipher("cbc(anubis)", ENCRYPT, anubis_cbc_enc_tv_template,
			    ANUBIS_CBC_ENC_TEST_VECTORS);
		test_cipher("cbc(anubis)", DECRYPT, anubis_cbc_dec_tv_template,
			    ANUBIS_CBC_ENC_TEST_VECTORS);

		//XETA
		test_cipher("ecb(xeta)", ENCRYPT, xeta_enc_tv_template,
			    XETA_ENC_TEST_VECTORS);
		test_cipher("ecb(xeta)", DECRYPT, xeta_dec_tv_template,
			    XETA_DEC_TEST_VECTORS);

		test_hash("sha384", sha384_tv_template, SHA384_TEST_VECTORS);
		test_hash("sha512", sha512_tv_template, SHA512_TEST_VECTORS);
		test_hash("wp512", wp512_tv_template, WP512_TEST_VECTORS);
		test_hash("wp384", wp384_tv_template, WP384_TEST_VECTORS);
		test_hash("wp256", wp256_tv_template, WP256_TEST_VECTORS);
		test_hash("tgr192", tgr192_tv_template, TGR192_TEST_VECTORS);
		test_hash("tgr160", tgr160_tv_template, TGR160_TEST_VECTORS);
		test_hash("tgr128", tgr128_tv_template, TGR128_TEST_VECTORS);
		test_deflate();
		test_hash("crc32c", crc32c_tv_template, CRC32C_TEST_VECTORS);
#ifdef CONFIG_CRYPTO_HMAC
		test_hmac("md5", hmac_md5_tv_template, HMAC_MD5_TEST_VECTORS);
		test_hmac("sha1", hmac_sha1_tv_template, HMAC_SHA1_TEST_VECTORS);
		test_hmac("sha256", hmac_sha256_tv_template, HMAC_SHA256_TEST_VECTORS);
#endif

		test_hash("michael_mic", michael_mic_tv_template, MICHAEL_MIC_TEST_VECTORS);
		break;

	case 1:
		test_hash("md5", md5_tv_template, MD5_TEST_VECTORS);
		break;

	case 2:
		test_hash("sha1", sha1_tv_template, SHA1_TEST_VECTORS);
		break;

	case 3:
		test_cipher("ecb(des)", ENCRYPT, des_enc_tv_template,
			    DES_ENC_TEST_VECTORS);
		test_cipher("ecb(des)", DECRYPT, des_dec_tv_template,
			    DES_DEC_TEST_VECTORS);
		test_cipher("cbc(des)", ENCRYPT, des_cbc_enc_tv_template,
			    DES_CBC_ENC_TEST_VECTORS);
		test_cipher("cbc(des)", DECRYPT, des_cbc_dec_tv_template,
			    DES_CBC_DEC_TEST_VECTORS);
		break;

	case 4:
		test_cipher("ecb(des3_ede)", ENCRYPT, des3_ede_enc_tv_template,
			    DES3_EDE_ENC_TEST_VECTORS);
		test_cipher("ecb(des3_ede)", DECRYPT, des3_ede_dec_tv_template,
			    DES3_EDE_DEC_TEST_VECTORS);
		break;

	case 5:
		test_hash("md4", md4_tv_template, MD4_TEST_VECTORS);
		break;

	case 6:
		test_hash("sha256", sha256_tv_template, SHA256_TEST_VECTORS);
		break;

	case 7:
		test_cipher("ecb(blowfish)", ENCRYPT, bf_enc_tv_template,
			    BF_ENC_TEST_VECTORS);
		test_cipher("ecb(blowfish)", DECRYPT, bf_dec_tv_template,
			    BF_DEC_TEST_VECTORS);
		test_cipher("cbc(blowfish)", ENCRYPT, bf_cbc_enc_tv_template,
			    BF_CBC_ENC_TEST_VECTORS);
		test_cipher("cbc(blowfish)", DECRYPT, bf_cbc_dec_tv_template,
			    BF_CBC_DEC_TEST_VECTORS);
		break;

	case 8:
		test_cipher("ecb(twofish)", ENCRYPT, tf_enc_tv_template,
			    TF_ENC_TEST_VECTORS);
		test_cipher("ecb(twofish)", DECRYPT, tf_dec_tv_template,
			    TF_DEC_TEST_VECTORS);
		test_cipher("cbc(twofish)", ENCRYPT, tf_cbc_enc_tv_template,
			    TF_CBC_ENC_TEST_VECTORS);
		test_cipher("cbc(twofish)", DECRYPT, tf_cbc_dec_tv_template,
			    TF_CBC_DEC_TEST_VECTORS);
		break;

	case 9:
		test_cipher("ecb(serpent)", ENCRYPT, serpent_enc_tv_template,
			    SERPENT_ENC_TEST_VECTORS);
		test_cipher("ecb(serpent)", DECRYPT, serpent_dec_tv_template,
			    SERPENT_DEC_TEST_VECTORS);
		break;

	case 10:
		test_cipher("ecb(aes)", ENCRYPT, aes_enc_tv_template,
			    AES_ENC_TEST_VECTORS);
		test_cipher("ecb(aes)", DECRYPT, aes_dec_tv_template,
			    AES_DEC_TEST_VECTORS);
		test_cipher("cbc(aes)", ENCRYPT, aes_cbc_enc_tv_template,
			    AES_CBC_ENC_TEST_VECTORS);
		test_cipher("cbc(aes)", DECRYPT, aes_cbc_dec_tv_template,
			    AES_CBC_DEC_TEST_VECTORS);
		break;

	case 11:
		test_hash("sha384", sha384_tv_template, SHA384_TEST_VECTORS);
		break;

	case 12:
		test_hash("sha512", sha512_tv_template, SHA512_TEST_VECTORS);
		break;

	case 13:
		test_deflate();
		break;

	case 14:
		test_cipher("ecb(cast5)", ENCRYPT, cast5_enc_tv_template,
			    CAST5_ENC_TEST_VECTORS);
		test_cipher("ecb(cast5)", DECRYPT, cast5_dec_tv_template,
			    CAST5_DEC_TEST_VECTORS);
		break;

	case 15:
		test_cipher("ecb(cast6)", ENCRYPT, cast6_enc_tv_template,
			    CAST6_ENC_TEST_VECTORS);
		test_cipher("ecb(cast6)", DECRYPT, cast6_dec_tv_template,
			    CAST6_DEC_TEST_VECTORS);
		break;

	case 16:
		test_cipher("ecb(arc4)", ENCRYPT, arc4_enc_tv_template,
			    ARC4_ENC_TEST_VECTORS);
		test_cipher("ecb(arc4)", DECRYPT, arc4_dec_tv_template,
			    ARC4_DEC_TEST_VECTORS);
		break;

	case 17:
		test_hash("michael_mic", michael_mic_tv_template, MICHAEL_MIC_TEST_VECTORS);
		break;

	case 18:
		test_hash("crc32c", crc32c_tv_template, CRC32C_TEST_VECTORS);
		break;

	case 19:
		test_cipher("ecb(tea)", ENCRYPT, tea_enc_tv_template,
			    TEA_ENC_TEST_VECTORS);
		test_cipher("ecb(tea)", DECRYPT, tea_dec_tv_template,
			    TEA_DEC_TEST_VECTORS);
		break;

	case 20:
		test_cipher("ecb(xtea)", ENCRYPT, xtea_enc_tv_template,
			    XTEA_ENC_TEST_VECTORS);
		test_cipher("ecb(xtea)", DECRYPT, xtea_dec_tv_template,
			    XTEA_DEC_TEST_VECTORS);
		break;

	case 21:
		test_cipher("ecb(khazad)", ENCRYPT, khazad_enc_tv_template,
			    KHAZAD_ENC_TEST_VECTORS);
		test_cipher("ecb(khazad)", DECRYPT, khazad_dec_tv_template,
			    KHAZAD_DEC_TEST_VECTORS);
		break;

	case 22:
		test_hash("wp512", wp512_tv_template, WP512_TEST_VECTORS);
		break;

	case 23:
		test_hash("wp384", wp384_tv_template, WP384_TEST_VECTORS);
		break;

	case 24:
		test_hash("wp256", wp256_tv_template, WP256_TEST_VECTORS);
		break;

	case 25:
		test_cipher("ecb(tnepres)", ENCRYPT, tnepres_enc_tv_template,
			    TNEPRES_ENC_TEST_VECTORS);
		test_cipher("ecb(tnepres)", DECRYPT, tnepres_dec_tv_template,
			    TNEPRES_DEC_TEST_VECTORS);
		break;

	case 26:
		test_cipher("ecb(anubis)", ENCRYPT, anubis_enc_tv_template,
			    ANUBIS_ENC_TEST_VECTORS);
		test_cipher("ecb(anubis)", DECRYPT, anubis_dec_tv_template,
			    ANUBIS_DEC_TEST_VECTORS);
		test_cipher("cbc(anubis)", ENCRYPT, anubis_cbc_enc_tv_template,
			    ANUBIS_CBC_ENC_TEST_VECTORS);
		test_cipher("cbc(anubis)", DECRYPT, anubis_cbc_dec_tv_template,
			    ANUBIS_CBC_ENC_TEST_VECTORS);
		break;

	case 27:
		test_hash("tgr192", tgr192_tv_template, TGR192_TEST_VECTORS);
		break;

	case 28:

		test_hash("tgr160", tgr160_tv_template, TGR160_TEST_VECTORS);
		break;

	case 29:
		test_hash("tgr128", tgr128_tv_template, TGR128_TEST_VECTORS);
		break;
		
	case 30:
		test_cipher("ecb(xeta)", ENCRYPT, xeta_enc_tv_template,
			    XETA_ENC_TEST_VECTORS);
		test_cipher("ecb(xeta)", DECRYPT, xeta_dec_tv_template,
			    XETA_DEC_TEST_VECTORS);
		break;

#ifdef CONFIG_CRYPTO_HMAC
	case 100:
		test_hmac("md5", hmac_md5_tv_template, HMAC_MD5_TEST_VECTORS);
		break;

	case 101:
		test_hmac("sha1", hmac_sha1_tv_template, HMAC_SHA1_TEST_VECTORS);
		break;

	case 102:
		test_hmac("sha256", hmac_sha256_tv_template, HMAC_SHA256_TEST_VECTORS);
		break;

#endif

	case 200:
		test_cipher_speed("ecb(aes)", ENCRYPT, sec, NULL, 0,
				  aes_speed_template);
		test_cipher_speed("ecb(aes)", DECRYPT, sec, NULL, 0,
				  aes_speed_template);
		test_cipher_speed("cbc(aes)", ENCRYPT, sec, NULL, 0,
				  aes_speed_template);
		test_cipher_speed("cbc(aes)", DECRYPT, sec, NULL, 0,
				  aes_speed_template);
		break;

	case 201:
		test_cipher_speed("ecb(des3_ede)", ENCRYPT, sec,
				  des3_ede_enc_tv_template,
				  DES3_EDE_ENC_TEST_VECTORS,
				  des3_ede_speed_template);
		test_cipher_speed("ecb(des3_ede)", DECRYPT, sec,
				  des3_ede_dec_tv_template,
				  DES3_EDE_DEC_TEST_VECTORS,
				  des3_ede_speed_template);
		test_cipher_speed("cbc(des3_ede)", ENCRYPT, sec,
				  des3_ede_enc_tv_template,
				  DES3_EDE_ENC_TEST_VECTORS,
				  des3_ede_speed_template);
		test_cipher_speed("cbc(des3_ede)", DECRYPT, sec,
				  des3_ede_dec_tv_template,
				  DES3_EDE_DEC_TEST_VECTORS,
				  des3_ede_speed_template);
		break;

	case 202:
		test_cipher_speed("ecb(twofish)", ENCRYPT, sec, NULL, 0,
				  twofish_speed_template);
		test_cipher_speed("ecb(twofish)", DECRYPT, sec, NULL, 0,
				  twofish_speed_template);
		test_cipher_speed("cbc(twofish)", ENCRYPT, sec, NULL, 0,
				  twofish_speed_template);
		test_cipher_speed("cbc(twofish)", DECRYPT, sec, NULL, 0,
				  twofish_speed_template);
		break;

	case 203:
		test_cipher_speed("ecb(blowfish)", ENCRYPT, sec, NULL, 0,
				  blowfish_speed_template);
		test_cipher_speed("ecb(blowfish)", DECRYPT, sec, NULL, 0,
				  blowfish_speed_template);
		test_cipher_speed("cbc(blowfish)", ENCRYPT, sec, NULL, 0,
				  blowfish_speed_template);
		test_cipher_speed("cbc(blowfish)", DECRYPT, sec, NULL, 0,
				  blowfish_speed_template);
		break;

	case 204:
		test_cipher_speed("ecb(des)", ENCRYPT, sec, NULL, 0,
				  des_speed_template);
		test_cipher_speed("ecb(des)", DECRYPT, sec, NULL, 0,
				  des_speed_template);
		test_cipher_speed("cbc(des)", ENCRYPT, sec, NULL, 0,
				  des_speed_template);
		test_cipher_speed("cbc(des)", DECRYPT, sec, NULL, 0,
				  des_speed_template);
		break;

	case 300:
		/* fall through */

	case 301:
		test_digest_speed("md4", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 302:
		test_digest_speed("md5", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 303:
		test_digest_speed("sha1", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 304:
		test_digest_speed("sha256", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 305:
		test_digest_speed("sha384", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 306:
		test_digest_speed("sha512", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 307:
		test_digest_speed("wp256", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 308:
		test_digest_speed("wp384", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 309:
		test_digest_speed("wp512", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 310:
		test_digest_speed("tgr128", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 311:
		test_digest_speed("tgr160", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 312:
		test_digest_speed("tgr192", sec, generic_digest_speed_template);
		if (mode > 300 && mode < 400) break;

	case 399:
		break;

	case 1000:
		test_available();
		break;

	default:
		/* useful for debugging */
		printk("not testing anything\n");
		break;
	}
}

static int __init init(void)
{
	tvmem = kmalloc(TVMEMSIZE, GFP_KERNEL);
	if (tvmem == NULL)
		return -ENOMEM;

	xbuf = kmalloc(XBUFSIZE, GFP_KERNEL);
	if (xbuf == NULL) {
		kfree(tvmem);
		return -ENOMEM;
	}

	do_test();

	kfree(xbuf);
	kfree(tvmem);

	/* We intentionaly return -EAGAIN to prevent keeping
	 * the module. It does all its work from init()
	 * and doesn't offer any runtime functionality 
	 * => we don't need it in the memory, do we?
	 *                                        -- mludvig
	 */
	return -EAGAIN;
}

/*
 * If an init function is provided, an exit function must also be provided
 * to allow module unload.
 */
static void __exit fini(void) { }

module_init(init);
module_exit(fini);

module_param(mode, int, 0);
module_param(sec, uint, 0);
MODULE_PARM_DESC(sec, "Length in seconds of speed tests "
		      "(defaults to zero which uses CPU cycles instead)");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Quick & dirty crypto testing module");
MODULE_AUTHOR("James Morris <jmorris@intercode.com.au>");
