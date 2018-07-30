/*
 * key management facility for FS encryption support.
 *
 * Copyright (C) 2015, Google, Inc.
 *
 * This contains encryption key functions.
 *
 * Written by Michael Halcrow, Ildar Muslukhov, and Uday Savagaonkar, 2015.
 */

#include <keys/user-type.h>
#include <linux/scatterlist.h>
#include <linux/ratelimit.h>
#include <crypto/aes.h>
#include <crypto/sha.h>
#include <crypto/skcipher.h>
#include "fscrypt_private.h"
#include <crypto/fmp.h>

#ifdef CONFIG_FSCRYPT_SDP
static int derive_fek(struct inode *inode,
		const struct fscrypt_context *ctx,
		struct fscrypt_info *crypt_info,
		u8 *fek, u32 fek_len);
#endif

static struct crypto_shash *essiv_hash_tfm;

/*
 * Key derivation function.  This generates the derived key by encrypting the
 * master key with AES-128-ECB using the inode's nonce as the AES key.
 *
 * The master key must be at least as long as the derived key.  If the master
 * key is longer, then only the first 'derived_keysize' bytes are used.
 */
static int derive_key_aes(const u8 *master_key,
			  const struct fscrypt_context *ctx,
			  u8 *derived_key, unsigned int derived_keysize)
{
	int res = 0;
	struct skcipher_request *req = NULL;
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist src_sg, dst_sg;
	struct crypto_skcipher *tfm = crypto_alloc_skcipher("ecb(aes)", 0, 0);

	if (IS_ERR(tfm)) {
		res = PTR_ERR(tfm);
		tfm = NULL;
		goto out;
	}
	crypto_skcipher_set_flags(tfm, CRYPTO_TFM_REQ_WEAK_KEY);
	req = skcipher_request_alloc(tfm, GFP_NOFS);
	if (!req) {
		res = -ENOMEM;
		goto out;
	}
	skcipher_request_set_callback(req,
			CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
			crypto_req_done, &wait);
	res = crypto_skcipher_setkey(tfm, ctx->nonce, sizeof(ctx->nonce));
	if (res < 0)
		goto out;

	sg_init_one(&src_sg, master_key, derived_keysize);
	sg_init_one(&dst_sg, derived_key, derived_keysize);
	skcipher_request_set_crypt(req, &src_sg, &dst_sg, derived_keysize,
				   NULL);
	res = crypto_wait_req(crypto_skcipher_encrypt(req), &wait);
out:
	skcipher_request_free(req);
	crypto_free_skcipher(tfm);
	return res;
}

static inline int get_fe_key(char *nonce, const struct fscrypt_key *source_key, char *fe_key)
{
#ifdef CONFIG_FS_CRYPTO_SEC_EXTENSION
	return fscrypt_sec_get_key_aes(nonce, source_key->raw, fe_key);
#else
	return derive_key_aes(nonce, source_key, fe_key);
#endif /* CONFIG FS_CRYPTO_SEC_EXTENSION */
}

/*
 * Search the current task's subscribed keyrings for a "logon" key with
 * description prefix:descriptor, and if found acquire a read lock on it and
 * return a pointer to its validated payload in *payload_ret.
 */
static struct key *
find_and_lock_process_key(const char *prefix,
			  const u8 descriptor[FS_KEY_DESCRIPTOR_SIZE],
			  unsigned int min_keysize,
			  const struct fscrypt_key **payload_ret)
{
	char *description;
	struct key *key;
	const struct user_key_payload *ukp;
	const struct fscrypt_key *payload;

	description = kasprintf(GFP_NOFS, "%s%*phN", prefix,
				FS_KEY_DESCRIPTOR_SIZE, descriptor);
	if (!description)
		return ERR_PTR(-ENOMEM);

	key = request_key(&key_type_logon, description, NULL);
	kfree(description);
	if (IS_ERR(key))
		return key;

	down_read(&key->sem);
	ukp = user_key_payload_locked(key);

	if (!ukp) /* was the key revoked before we acquired its semaphore? */
		goto invalid;

	payload = (const struct fscrypt_key *)ukp->data;

	if (ukp->datalen != sizeof(struct fscrypt_key) ||
	    payload->size < 1 || payload->size > FS_MAX_KEY_SIZE) {
		fscrypt_warn(NULL,
			     "key with description '%s' has invalid payload",
			     key->description);
		goto invalid;
	}

	if (payload->size < min_keysize) {
		fscrypt_warn(NULL,
			     "key with description '%s' is too short (got %u bytes, need %u+ bytes)",
			     key->description, payload->size, min_keysize);
		goto invalid;
	}

	*payload_ret = payload;
	return key;

invalid:
	up_read(&key->sem);
	key_put(key);
	return ERR_PTR(-ENOKEY);
}

/* Find the master key, then derive the inode's actual encryption key */
static int find_and_derive_key(const struct inode *inode,
			       const struct fscrypt_context *ctx,
			       u8 *derived_key, unsigned int derived_keysize)
{
	struct key *key;
	const struct fscrypt_key *payload;
	int err;

	key = find_and_lock_process_key(FS_KEY_DESC_PREFIX,
					ctx->master_key_descriptor,
					derived_keysize, &payload);
	if (key == ERR_PTR(-ENOKEY) && inode->i_sb->s_cop->key_prefix) {
		key = find_and_lock_process_key(inode->i_sb->s_cop->key_prefix,
						ctx->master_key_descriptor,
						derived_keysize, &payload);
	}
	if (IS_ERR(key))
		return PTR_ERR(key);
	err = derive_key_aes(payload->raw, ctx, derived_key, derived_keysize);
	up_read(&key->sem);
	key_put(key);
	return err;
}

static struct fscrypt_mode {
	const char *friendly_name;
	const char *cipher_str;
	int keysize;
	bool logged_impl_name;
} available_modes[] = {
	[FS_ENCRYPTION_MODE_AES_256_XTS] = {
		.friendly_name = "AES-256-XTS",
		.cipher_str = "xts(aes)",
		.keysize = 64,
	},
	[FS_ENCRYPTION_MODE_AES_256_CTS] = {
		.friendly_name = "AES-256-CTS-CBC",
		.cipher_str = "cts(cbc(aes))",
		.keysize = 32,
	},
	[FS_ENCRYPTION_MODE_AES_128_CBC] = {
		.friendly_name = "AES-128-CBC",
		.cipher_str = "cbc(aes)",
		.keysize = 16,
	},
	[FS_ENCRYPTION_MODE_AES_128_CTS] = {
		.friendly_name = "AES-128-CTS-CBC",
		.cipher_str = "cts(cbc(aes))",
		.keysize = 16,
	},
	[FS_ENCRYPTION_MODE_SPECK128_256_XTS] = {
		.friendly_name = "Speck128/256-XTS",
		.cipher_str = "xts(speck128)",
		.keysize = 64,
	},
	[FS_ENCRYPTION_MODE_SPECK128_256_CTS] = {
		.friendly_name = "Speck128/256-CTS-CBC",
		.cipher_str = "cts(cbc(speck128))",
		.keysize = 32,
	},
};

static struct fscrypt_mode *
select_encryption_mode(const struct fscrypt_info *ci, const struct inode *inode)
{
	if (!fscrypt_valid_enc_modes(ci->ci_data_mode, ci->ci_filename_mode)) {
		fscrypt_warn(inode->i_sb,
			     "inode %lu uses unsupported encryption modes (contents mode %d, filenames mode %d)",
			     inode->i_ino, ci->ci_data_mode,
			     ci->ci_filename_mode);
		return ERR_PTR(-EINVAL);
	}

	if (S_ISREG(inode->i_mode))
		return &available_modes[ci->ci_data_mode];

	if (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode))
		return &available_modes[ci->ci_filename_mode];

	WARN_ONCE(1, "fscrypt: filesystem tried to load encryption info for inode %lu, which is not encryptable (file type %d)\n",
		  inode->i_ino, (inode->i_mode & S_IFMT));
	return ERR_PTR(-EINVAL);
}

static void put_crypt_info(struct fscrypt_info *ci)
{
	if (!ci)
		return;
#ifdef CONFIG_FSCRYPT_SDP
	fscrypt_sdp_put_sdp_info(ci->ci_sdp_info);
#endif
	crypto_free_skcipher(ci->ci_ctfm);
	crypto_free_cipher(ci->ci_essiv_tfm);
	kmem_cache_free(fscrypt_info_cachep, ci);
}

static int derive_essiv_salt(const u8 *key, int keysize, u8 *salt)
{
	struct crypto_shash *tfm = READ_ONCE(essiv_hash_tfm);

	/* init hash transform on demand */
	if (unlikely(!tfm)) {
		struct crypto_shash *prev_tfm;

		tfm = crypto_alloc_shash("sha256", 0, 0);
		if (IS_ERR(tfm)) {
			fscrypt_warn(NULL,
				     "error allocating SHA-256 transform: %ld",
				     PTR_ERR(tfm));
			return PTR_ERR(tfm);
		}
		prev_tfm = cmpxchg(&essiv_hash_tfm, NULL, tfm);
		if (prev_tfm) {
			crypto_free_shash(tfm);
			tfm = prev_tfm;
		}
	}

	{
		SHASH_DESC_ON_STACK(desc, tfm);
		desc->tfm = tfm;
		desc->flags = 0;

		return crypto_shash_digest(desc, key, keysize, salt);
	}
}

static int init_essiv_generator(struct fscrypt_info *ci, const u8 *raw_key,
				int keysize)
{
	int err;
	struct crypto_cipher *essiv_tfm;
	u8 salt[SHA256_DIGEST_SIZE];

	essiv_tfm = crypto_alloc_cipher("aes", 0, 0);
	if (IS_ERR(essiv_tfm))
		return PTR_ERR(essiv_tfm);

	ci->ci_essiv_tfm = essiv_tfm;

	err = derive_essiv_salt(raw_key, keysize, salt);
	if (err)
		goto out;

	/*
	 * Using SHA256 to derive the salt/key will result in AES-256 being
	 * used for IV generation. File contents encryption will still use the
	 * configured keysize (AES-128) nevertheless.
	 */
	err = crypto_cipher_setkey(essiv_tfm, salt, sizeof(salt));
	if (err)
		goto out;

out:
	memzero_explicit(salt, sizeof(salt));
	return err;
}

void __exit fscrypt_essiv_cleanup(void)
{
	crypto_free_shash(essiv_hash_tfm);
}

int fscrypt_get_encryption_info(struct inode *inode)
{
	struct fscrypt_info *crypt_info;
	struct fscrypt_context ctx;
	struct crypto_skcipher *ctfm;
	struct fscrypt_mode *mode;
	u8 *raw_key = NULL;
	int res;

	if (inode->i_crypt_info)
		return 0;

	res = fscrypt_initialize(inode->i_sb->s_cop->flags);
	if (res)
		return res;

	res = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (res < 0) {
		if (!fscrypt_dummy_context_enabled(inode) ||
		    IS_ENCRYPTED(inode))
			return res;
		/* Fake up a context for an unencrypted directory */
		memset(&ctx, 0, sizeof(ctx));
		ctx.format = FS_ENCRYPTION_CONTEXT_FORMAT_V1;
#ifdef CONFIG_FS_PRIVATE_ENCRYPTION
		ctx.contents_encryption_mode = FS_PRIVATE_ENCRYPTION_MODE_AES_256_XTS;
#else
		ctx.contents_encryption_mode = FS_ENCRYPTION_MODE_AES_256_XTS;
#endif
		ctx.filenames_encryption_mode = FS_ENCRYPTION_MODE_AES_256_CTS;
		memset(ctx.master_key_descriptor, 0x42, FS_KEY_DESCRIPTOR_SIZE);
	} else if (res != sizeof(ctx)) {
		return -EINVAL;
	}

	if (ctx.format != FS_ENCRYPTION_CONTEXT_FORMAT_V1)
		return -EINVAL;

	if (ctx.flags & ~FS_POLICY_FLAGS_VALID)
		return -EINVAL;

	crypt_info = kmem_cache_alloc(fscrypt_info_cachep, GFP_NOFS);
	if (!crypt_info)
		return -ENOMEM;

	crypt_info->ci_flags = ctx.flags;
	crypt_info->ci_data_mode = ctx.contents_encryption_mode;
	crypt_info->ci_filename_mode = ctx.filenames_encryption_mode;
#ifdef CONFIG_FS_PRIVATE_ENCRYPTION
    if (ctx.filenames_encryption_mode == FS_PRIVATE_ENCRYPTION_MODE_AES_256_XTS ||
		ctx.filenames_encryption_mode == FS_PRIVATE_ENCRYPTION_MODE_AES_256_CBC) {
            printk(KERN_WARNING "Doesn't support filename encryption mode.\n");
	printk(KERN_WARNING "Forcely, change it to AES_256_CTS mode.\n");
            ctx.filenames_encryption_mode = FS_ENCRYPTION_MODE_AES_256_CTS;
    }
#endif
	crypt_info->ci_ctfm = NULL;
	crypt_info->ci_essiv_tfm = NULL;
	memcpy(crypt_info->ci_master_key, ctx.master_key_descriptor,
				sizeof(crypt_info->ci_master_key));
#ifdef CONFIG_FSCRYPT_SDP
	crypt_info->ci_sdp_info = NULL;
#endif

	mode = select_encryption_mode(crypt_info, inode);
	if (IS_ERR(mode)) {
		res = PTR_ERR(mode);
		goto out;
	}

	/*
	 * This cannot be a stack buffer because it is passed to the scatterlist
	 * crypto API as part of key derivation.
	 */
	res = -ENOMEM;
	raw_key = kmalloc(mode->keysize, GFP_NOFS);
	if (!raw_key)
		goto out;

	res = find_and_derive_key(inode, &ctx, raw_key, mode->keysize);
	if (res)
		goto out;

	ctfm = crypto_alloc_skcipher(mode->cipher_str, 0, 0);
	if (IS_ERR(ctfm)) {
		res = PTR_ERR(ctfm);
		fscrypt_warn(inode->i_sb,
			     "error allocating '%s' transform for inode %lu: %d",
			     mode->cipher_str, inode->i_ino, res);
		goto out;
	}
	if (unlikely(!mode->logged_impl_name)) {
		/*
		 * fscrypt performance can vary greatly depending on which
		 * crypto algorithm implementation is used.  Help people debug
		 * performance problems by logging the ->cra_driver_name the
		 * first time a mode is used.  Note that multiple threads can
		 * race here, but it doesn't really matter.
		 */
		mode->logged_impl_name = true;
		pr_info("fscrypt: %s using implementation \"%s\"\n",
			mode->friendly_name,
			crypto_skcipher_alg(ctfm)->base.cra_driver_name);
	}
	crypt_info->ci_ctfm = ctfm;
	crypto_skcipher_set_flags(ctfm, CRYPTO_TFM_REQ_WEAK_KEY);
	res = crypto_skcipher_setkey(ctfm, raw_key, mode->keysize);
	if (res)
		goto out;

	if (S_ISREG(inode->i_mode) &&
	    crypt_info->ci_data_mode == FS_ENCRYPTION_MODE_AES_128_CBC) {
		res = init_essiv_generator(crypt_info, raw_key, mode->keysize);
		if (res) {
			fscrypt_warn(inode->i_sb,
				     "error initializing ESSIV generator for inode %lu: %d",
				     inode->i_ino, res);
			goto out;
		}
	}
	if (cmpxchg(&inode->i_crypt_info, NULL, crypt_info) == NULL)
		crypt_info = NULL;
#ifdef CONFIG_FSCRYPT_SDP
	if (crypt_info == NULL) //Call only when i_crypt_info is loaded initially
		fscrypt_sdp_finalize_tasks(inode, raw_key, keysize);
#endif
out:
	if (res == -ENOKEY)
		res = 0;
	put_crypt_info(crypt_info);
	kzfree(raw_key);
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_info);

void fscrypt_put_encryption_info(struct inode *inode)
{
#ifdef CONFIG_FSCRYPT_SDP
	fscrypt_sdp_cache_remove_inode_num(inode);
#endif
	put_crypt_info(inode->i_crypt_info);
	inode->i_crypt_info = NULL;
}
EXPORT_SYMBOL(fscrypt_put_encryption_info);
#ifdef CONFIG_FSCRYPT_SDP
static inline int __find_and_derive_fskey2(const struct inode *inode,
						const struct fscrypt_context *ctx,
						struct fscrypt_key *fskey, unsigned int min_keysize,
						const u8* prefix)
{
	char *description;
	struct key *keyring_key;
	struct fscrypt_key *master_key;
	const struct user_key_payload *ukp;
	int res;

	description = kasprintf(GFP_NOFS, "%s%*phN", prefix,
				FS_KEY_DESCRIPTOR_SIZE,
				ctx->master_key_descriptor);
	if (!description)
		return -ENOMEM;

	keyring_key = request_key(&key_type_logon, description, NULL);
	kfree(description);
	if (IS_ERR(keyring_key))
		return PTR_ERR(keyring_key);
	down_read(&keyring_key->sem);

	if (keyring_key->type != &key_type_logon) {
		printk_once(KERN_WARNING
				"%s: key type must be logon\n", __func__);
		res = -ENOKEY;
		goto out;
	}
	ukp = user_key_payload_locked(keyring_key);
	if (!ukp) {
		/* key was revoked before we acquired its semaphore */
		res = -EKEYREVOKED;
		goto out;
	}
	if (ukp->datalen != sizeof(struct fscrypt_key)) {
		res = -EINVAL;
		goto out;
	}
	master_key = (struct fscrypt_key *)ukp->data;
	BUILD_BUG_ON(FS_AES_128_ECB_KEY_SIZE != FS_KEY_DERIVATION_NONCE_SIZE);

	if (master_key->size < min_keysize || master_key->size > FS_MAX_KEY_SIZE
	    || master_key->size % AES_BLOCK_SIZE != 0) {
		printk_once(KERN_WARNING
				"%s: key size incorrect: %d\n",
				__func__, master_key->size);
		res = -ENOKEY;
		goto out;
	}
	memcpy(fskey, master_key, sizeof(struct fscrypt_key));
	res = 0;
out:
	up_read(&keyring_key->sem);
	key_put(keyring_key);
	return res;
}

static inline int __find_and_derive_fskey(struct inode *inode,
						const struct fscrypt_context *ctx,
						struct fscrypt_key *fskey, unsigned int min_keysize)
{
	int res;
	res = __find_and_derive_fskey2(inode, ctx, fskey, min_keysize,
									FS_KEY_DESC_PREFIX);
	if (res && inode->i_sb->s_cop->key_prefix) {
		int res2 = __find_and_derive_fskey2(inode, ctx, fskey, min_keysize,
				inode->i_sb->s_cop->key_prefix);
		if (res2) {
			if (res2 == -ENOKEY)
				res = -ENOKEY;
			goto out;
		}
		res = 0;
	} else if (res) {
		goto out;
	}

out:
	return res;
}

/* The function is only for regular files */
static int derive_fek(struct inode *inode,
						const struct fscrypt_context *ctx,
						struct fscrypt_info *crypt_info,
						u8 *fek, u32 fek_len)
{
	int res = 0;
	/*
	 * 1. [ Native / Uninitialized / To_sensitive ]  --> Plain fek
	 * 2. [ Native / Uninitialized / Non_sensitive ] --> Plain fek
	 */
	if (fscrypt_sdp_is_uninitialized(crypt_info))
	{
		res = fscrypt_sdp_derive_uninitialized_dek(crypt_info, fek, fek_len);
	}
	/*
	 * 3. [ Native / Initialized / Sensitive ]     --> { fek }_SDPK
	 * 4. [ Non_native / Initialized / Sensitive ] --> { fek }_SDPK
	 */
	else if (fscrypt_sdp_is_sensitive(crypt_info))
	{
		res = fscrypt_sdp_derive_dek(crypt_info, fek, fek_len);
	}
	/*
	 * 5. [ Native / Initialized / Non_sensitive ] --> { fek }_cekey
	 */
	else if (fscrypt_sdp_is_native(crypt_info))
	{
		res = fscrypt_sdp_derive_fek(inode, crypt_info, fek, fek_len);
	}
	/*
	 * else { N/A }
	 *
	 * Not classified file.
	 * 6. [ Non_native / Initialized / Non_sensitive ]
	 * 7. [ Non_native / Initialized / To_sensitive ]
	 */
	return res;
}

int fscrypt_get_encryption_key(struct inode *inode, struct fscrypt_key *key)
{
	struct fscrypt_info *crypt_info;
	struct fscrypt_context ctx;
	const char *cipher_str;
	int keysize;
	int res;
	//int fname = 0;
	u8 *raw_key = NULL;
	if (!inode->i_crypt_info)
		return -EINVAL;
	crypt_info = inode->i_crypt_info;

	res = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (res < 0) {
		return res;
	} else if (res != sizeof(ctx)) {
		return -EINVAL;
	}

	if (ctx.format != FS_ENCRYPTION_CONTEXT_FORMAT_V1)
		return -EINVAL;

	if (ctx.flags & ~FS_POLICY_FLAGS_VALID)
		return -EINVAL;

	res = determine_cipher_type(crypt_info, inode, &cipher_str, &keysize);
	if (res)
		goto out;

	raw_key = kmalloc(FS_MAX_KEY_SIZE, GFP_NOFS);
	if (!raw_key)
		goto out;

	/*
	 * This cannot be a stack buffer because it is passed to the scatterlist
	 * crypto API as part of key derivation.
	 */
	 
	res = -ENOMEM;
	res = validate_user_key(crypt_info, &ctx, raw_key, FS_KEY_DESC_PREFIX,
			keysize);
	if (res && inode->i_sb->s_cop->key_prefix) {
		int res2 = validate_user_key(crypt_info, &ctx, raw_key,
				inode->i_sb->s_cop->key_prefix,
				keysize);
		if (res2) {
			if (res2 == -ENOKEY)
				res = -ENOKEY;
			goto out;
		}
		res = 0;
	} else if (res) {
		goto out;
	}
	memcpy(key->raw, raw_key, keysize);
	key->size = keysize;

out:
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_key);

int fscrypt_get_encryption_kek(struct inode *inode,
							struct fscrypt_info *crypt_info,
							struct fscrypt_key *kek)
{
	struct fscrypt_context ctx;
	const char *cipher_str;
	int keysize;
	int res;

	if (!crypt_info)
		return -EINVAL;

	res = inode->i_sb->s_cop->get_context(inode, &ctx, sizeof(ctx));
	if (res < 0) {
		return res;
	} else if (res != sizeof(ctx)) {
		return -EINVAL;
	}

	if (ctx.format != FS_ENCRYPTION_CONTEXT_FORMAT_V1)
		return -EINVAL;

	if (ctx.flags & ~FS_POLICY_FLAGS_VALID)
		return -EINVAL;

	res = determine_cipher_type(crypt_info, inode, &cipher_str, &keysize);
	if (res)
		goto out;

	/*
	 * This cannot be a stack buffer because it is passed to the scatterlist
	 * crypto API as part of key derivation.
	 */
	res = -ENOMEM;
	res = __find_and_derive_fskey(inode, &ctx, kek, keysize);

out:
	return res;
}
EXPORT_SYMBOL(fscrypt_get_encryption_kek);
#endif
