/*
 * hmac_ossl.c
 *
 * Implementation of hmac srtp_auth_type_t that leverages OpenSSL
 *
 * John A. Foley
 * Cisco Systems, Inc.
 */
/*
 *
 * Copyright(c) 2013, Cisco Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials provided
 *   with the distribution.
 *
 *   Neither the name of the Cisco Systems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include "hmac.h"
#include "alloc.h"
#include <openssl/evp.h>

#define HMAC_KEYLEN_MAX		20

/* the debug module for authentiation */

srtp_debug_module_t srtp_mod_hmac = {
    0,                /* debugging is off by default */
    "hmac sha-1 openssl"   /* printable name for module   */
};


static srtp_err_status_t srtp_hmac_alloc (srtp_auth_t **a, int key_len, int out_len)
{
    extern srtp_auth_type_t srtp_hmac;
    uint8_t *pointer;
    srtp_hmac_ctx_t *new_hmac_ctx;

    debug_print(srtp_mod_hmac, "allocating auth func with key length %d", key_len);
    debug_print(srtp_mod_hmac, "                          tag length %d", out_len);

    /*
     * check key length - note that we don't support keys larger
     * than 20 bytes yet
     */
    if (key_len > HMAC_KEYLEN_MAX) {
        return srtp_err_status_bad_param;
    }

    /* check output length - should be less than 20 bytes */
    if (out_len > HMAC_KEYLEN_MAX) {
        return srtp_err_status_bad_param;
    }

    /* allocate memory for auth and srtp_hmac_ctx_t structures */
    pointer = (uint8_t*)srtp_crypto_alloc(sizeof(srtp_hmac_ctx_t) + sizeof(srtp_auth_t));
    if (pointer == NULL) {
        return srtp_err_status_alloc_fail;
    }

    /* set pointers */
    *a = (srtp_auth_t*)pointer;
    (*a)->type = &srtp_hmac;
    (*a)->state = pointer + sizeof(srtp_auth_t);
    (*a)->out_len = out_len;
    (*a)->key_len = key_len;
    (*a)->prefix_len = 0;
    new_hmac_ctx = (srtp_hmac_ctx_t*)((*a)->state);
    memset(new_hmac_ctx, 0, sizeof(srtp_hmac_ctx_t));

    return srtp_err_status_ok;
}

static srtp_err_status_t srtp_hmac_dealloc (srtp_auth_t *a)
{
    srtp_hmac_ctx_t *hmac_ctx;

    hmac_ctx = (srtp_hmac_ctx_t*)a->state;
    if (hmac_ctx->ctx_initialized) {
        EVP_MD_CTX_cleanup(&hmac_ctx->ctx);
    }
    if (hmac_ctx->init_ctx_initialized) {
        EVP_MD_CTX_cleanup(&hmac_ctx->init_ctx);
    }

    /* zeroize entire state*/
    octet_string_set_to_zero((uint8_t*)a,
                             sizeof(srtp_hmac_ctx_t) + sizeof(srtp_auth_t));

    /* free memory */
    srtp_crypto_free(a);

    return srtp_err_status_ok;
}

static srtp_err_status_t srtp_hmac_start (srtp_hmac_ctx_t *state)
{
    if (state->ctx_initialized) {
        EVP_MD_CTX_cleanup(&state->ctx);
    }
    if (!EVP_MD_CTX_copy(&state->ctx, &state->init_ctx)) {
        return srtp_err_status_auth_fail;
    } else {
        state->ctx_initialized = 1;
        return srtp_err_status_ok;
    }
}

static srtp_err_status_t srtp_hmac_init (srtp_hmac_ctx_t *state, const uint8_t *key, int key_len)
{
    int i;
    uint8_t ipad[64];

    /*
     * check key length - note that we don't support keys larger
     * than 20 bytes yet
     */
    if (key_len > HMAC_KEYLEN_MAX) {
        return srtp_err_status_bad_param;
    }

    /*
     * set values of ipad and opad by exoring the key into the
     * appropriate constant values
     */
    for (i = 0; i < key_len; i++) {
        ipad[i] = key[i] ^ 0x36;
        state->opad[i] = key[i] ^ 0x5c;
    }
    /* set the rest of ipad, opad to constant values */
    for (; i < sizeof(ipad); i++) {
        ipad[i] = 0x36;
        ((uint8_t*)state->opad)[i] = 0x5c;
    }

    debug_print(srtp_mod_hmac, "ipad: %s", srtp_octet_string_hex_string(ipad, sizeof(ipad)));

    /* initialize sha1 context */
    srtp_sha1_init(&state->init_ctx);
    state->init_ctx_initialized = 1;

    /* hash ipad ^ key */
    srtp_sha1_update(&state->init_ctx, ipad, sizeof(ipad));
    return (srtp_hmac_start(state));
}

static srtp_err_status_t srtp_hmac_update (srtp_hmac_ctx_t *state, const uint8_t *message, int msg_octets)
{
    debug_print(srtp_mod_hmac, "input: %s",
                srtp_octet_string_hex_string(message, msg_octets));

    /* hash message into sha1 context */
    srtp_sha1_update(&state->ctx, message, msg_octets);

    return srtp_err_status_ok;
}

static srtp_err_status_t srtp_hmac_compute (srtp_hmac_ctx_t *state, const void *message,
              int msg_octets, int tag_len, uint8_t *result)
{
    uint32_t hash_value[5];
    uint32_t H[5];
    int i;

    /* check tag length, return error if we can't provide the value expected */
    if (tag_len > HMAC_KEYLEN_MAX) {
        return srtp_err_status_bad_param;
    }

    /* hash message, copy output into H */
    srtp_sha1_update(&state->ctx, message, msg_octets);
    srtp_sha1_final(&state->ctx, H);

    /*
     * note that we don't need to debug_print() the input, since the
     * function hmac_update() already did that for us
     */
    debug_print(srtp_mod_hmac, "intermediate state: %s",
                srtp_octet_string_hex_string((uint8_t*)H, sizeof(H)));

    /* re-initialize hash context */
    srtp_sha1_init(&state->ctx);

    /* hash opad ^ key  */
    srtp_sha1_update(&state->ctx, (uint8_t*)state->opad, sizeof(state->opad));

    /* hash the result of the inner hash */
    srtp_sha1_update(&state->ctx, (uint8_t*)H, sizeof(H));

    /* the result is returned in the array hash_value[] */
    srtp_sha1_final(&state->ctx, hash_value);

    /* copy hash_value to *result */
    for (i = 0; i < tag_len; i++) {
        result[i] = ((uint8_t*)hash_value)[i];
    }

    debug_print(srtp_mod_hmac, "output: %s",
                srtp_octet_string_hex_string((uint8_t*)hash_value, tag_len));

    return srtp_err_status_ok;
}


/* begin test case 0 */

static uint8_t srtp_hmac_test_case_0_key[HMAC_KEYLEN_MAX] = {
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    0x0b, 0x0b, 0x0b, 0x0b
};

static uint8_t srtp_hmac_test_case_0_data[8] = {
    0x48, 0x69, 0x20, 0x54, 0x68, 0x65, 0x72, 0x65 /* "Hi There" */
};

static uint8_t srtp_hmac_test_case_0_tag[HMAC_KEYLEN_MAX] = {
    0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64,
    0xe2, 0x8b, 0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e,
    0xf1, 0x46, 0xbe, 0x00
};

static srtp_auth_test_case_t srtp_hmac_test_case_0 = {
    sizeof(srtp_hmac_test_case_0_key),    /* octets in key            */
    srtp_hmac_test_case_0_key,            /* key                      */
    sizeof(srtp_hmac_test_case_0_data),   /* octets in data           */
    srtp_hmac_test_case_0_data,           /* data                     */
    sizeof(srtp_hmac_test_case_0_tag),    /* octets in tag            */
    srtp_hmac_test_case_0_tag,            /* tag                      */
    NULL                             /* pointer to next testcase */
};

/* end test case 0 */

static char srtp_hmac_description[] = "hmac sha-1 authentication function";

/*
 * srtp_auth_type_t hmac is the hmac metaobject
 */

srtp_auth_type_t srtp_hmac  = {
    (auth_alloc_func)	srtp_hmac_alloc,
    (auth_dealloc_func)	srtp_hmac_dealloc,
    (auth_init_func)	srtp_hmac_init,
    (auth_compute_func)	srtp_hmac_compute,
    (auth_update_func)	srtp_hmac_update,
    (auth_start_func)	srtp_hmac_start,
    (char*)		srtp_hmac_description,
    (srtp_auth_test_case_t*)	&srtp_hmac_test_case_0,
    (srtp_debug_module_t*)	&srtp_mod_hmac,
    (srtp_auth_type_id_t) SRTP_HMAC_SHA1
};

