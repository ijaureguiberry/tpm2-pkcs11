/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2018, Intel Corporation
 * All rights reserved.
 */
/* config can control how other headers behave, include first */
#include "config.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include <arpa/inet.h>

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include <tss2/tss2_esys.h>
#include <tss2/tss2_mu.h>
#include <tss2/tss2_tctildr.h>

#include "attrs.h"
#include "checks.h"
#include "digest.h"
#include "openssl_compat.h"
#include "pkcs11.h"
#include "log.h"
#include "mutex.h"
#include "tpm.h"

/**
 * Maps 4-byte manufacturer identifier to manufacturer name.
 */
static const char *TPM2_MANUFACTURER_MAP[][2] = {
    {"ATML", "Atmel"},
    {"INTC", "Intel"},
    {"IFX ", "Infineon"},
    {"IBM ", "IBM"},
    {"NTC ", "Nuvoton"},
    {"STM ", "STMicro"}
};

static TPMS_CAPABILITY_DATA *tpms_fixed_property_cache;
static TPMS_CAPABILITY_DATA *tpms_alg_cache;

struct tpm_ctx {
    TSS2_TCTI_CONTEXT *tcti_ctx;
    ESYS_CONTEXT *esys_ctx;
    bool esapi_manage_session_flags;
    ESYS_TR hmac_session;
    TPMA_SESSION old_flags;
    TPMA_SESSION original_flags;
};

#define TPM2B_INIT(xsize) { .size = xsize, }
#define TPM2B_EMPTY_INIT TPM2B_INIT(0)

#define TPM2_ERROR_TSS2_RC_ERROR_MASK 0xFFFF

static inline UINT16 tpm2_error_get(TSS2_RC rc) {
    return ((rc & TPM2_ERROR_TSS2_RC_ERROR_MASK));
}

#define TSS2_RETRY_EXP(expression)                         \
    ({                                                     \
        TSS2_RC __result = 0;                              \
        do {                                               \
            __result = (expression);                       \
        } while (tpm2_error_get(__result) == TPM2_RC_RETRY); \
        __result;                                          \
    })

#define TSS2L_SYS_AUTH_COMMAND_INIT(cnt, array) { \
        .count = cnt, \
        .auths = array, \
    }

#define TPMS_AUTH_COMMAND_INIT(session_handle) \
        TPMS_AUTH_COMMAND_INIT_ATTRS(session_handle, TPMA_SESSION_CONTINUESESSION)

#define TPMS_AUTH_COMMAND_INIT_ATTRS(session_handle, attrs) { \
        .sessionHandle = session_handle,\
        .nonce = TPM2B_EMPTY_INIT, \
        .sessionAttributes = attrs, \
        .hmac = TPM2B_EMPTY_INIT \
    }

typedef struct tpm2_hierarchy_pdata tpm2_hierarchy_pdata;
struct tpm2_hierarchy_pdata {
    struct {
        TPMI_RH_HIERARCHY hierarchy;
        TPM2B_SENSITIVE_CREATE sensitive;
        TPM2B_PUBLIC public;
        TPM2B_DATA outside_info;
        TPML_PCR_SELECTION creation_pcr;
        TPM2_HANDLE object_handle;
    } in;
    struct {
        TPM2_HANDLE handle;
        TPM2B_PUBLIC public;
        TPM2B_DIGEST hash;
        struct {
            TPM2B_CREATION_DATA data;
            TPMT_TK_CREATION ticket;
        } creation;
        TPM2B_NAME name;
    } out;
};

//|decrypt|sign"
#define _PUBLIC_AREA_TPMA_OBJECT_DEFAULT_INIT { \
    .publicArea = { \
        .nameAlg = TPM2_ALG_SHA256, \
        .type = TPM2_ALG_RSA, \
        .objectAttributes = \
              TPMA_OBJECT_FIXEDTPM \
            | TPMA_OBJECT_FIXEDPARENT \
            | TPMA_OBJECT_SENSITIVEDATAORIGIN \
            | TPMA_OBJECT_USERWITHAUTH \
            | TPMA_OBJECT_DECRYPT \
            | TPMA_OBJECT_SIGN_ENCRYPT, \
        .parameters = { \
            .rsaDetail = { \
                .exponent = 0, \
                .symmetric = { \
                    .algorithm = TPM2_ALG_NULL, \
                 }, \
            .scheme = { .scheme = TPM2_ALG_NULL }, \
            .keyBits = 2048 \
            }, \
        }, \
            .unique = { .rsa = { .size = 0 } } \
    }, \
}

#define TPM2B_SENSITIVE_CREATE_EMPTY_INIT { \
           .sensitive = { \
                .data = {   \
                    .size = 0 \
                }, \
                .userAuth = {   \
                    .size = 0 \
                } \
            } \
    }

#define TPM2_HIERARCHY_DATA_INIT { \
    .in = { \
        .public = _PUBLIC_AREA_TPMA_OBJECT_DEFAULT_INIT, \
        .sensitive = TPM2B_SENSITIVE_CREATE_EMPTY_INIT, \
        .hierarchy = TPM2_RH_OWNER \
    }, \
}

#define SUPPORTED_ABI_VERSION \
{ \
    .tssCreator = 1, \
    .tssFamily = 2, \
    .tssLevel = 1, \
    .tssVersion = 108, \
}

static ESYS_CONTEXT* esys_ctx_init(TSS2_TCTI_CONTEXT *tcti_ctx) {

    TSS2_ABI_VERSION abi_version = SUPPORTED_ABI_VERSION;

    ESYS_CONTEXT *esys_ctx = NULL;
    TSS2_RC rval = Esys_Initialize(&esys_ctx, tcti_ctx, &abi_version);
    if (rval != TPM2_RC_SUCCESS) {
        LOGE("Esys_Initialize: 0x%x", rval);
        return NULL;
    }

    return esys_ctx;
}

static void flags_turndown(tpm_ctx *ctx, TPMA_SESSION flags) {

    if (ctx->esapi_manage_session_flags) {
        return;
    }

    TSS2_RC rc = Esys_TRSess_GetAttributes(ctx->esys_ctx, ctx->hmac_session, &ctx->old_flags);
    assert(rc == TPM2_RC_SUCCESS);
    if (rc != TSS2_RC_SUCCESS) {
        LOGW("Esys_TRSess_SetAttributes: 0x%x", rc);
        return;
    }

    assert(ctx->old_flags == ctx->original_flags);

    TPMA_SESSION new_flags = (ctx->old_flags & (~flags));
    rc = Esys_TRSess_SetAttributes(ctx->esys_ctx, ctx->hmac_session, new_flags, 0xff);
    assert(rc == TSS2_RC_SUCCESS);
    if (rc != TSS2_RC_SUCCESS) {
        LOGW("Esys_TRSess_SetAttributes: 0x%x", rc);
    }
}

static void flags_restore(tpm_ctx *ctx) {

    if (ctx->esapi_manage_session_flags) {
        return;
    }

    assert(ctx->old_flags == ctx->original_flags);

    TSS2_RC rc = Esys_TRSess_SetAttributes(ctx->esys_ctx, ctx->hmac_session, ctx->old_flags, 0xff);
    assert(rc == TSS2_RC_SUCCESS);
    if (rc != TSS2_RC_SUCCESS) {
        LOGW("Esys_TRSess_SetAttributes: 0x%x", rc);
    }
}

void tpm_ctx_free(tpm_ctx *ctx) {

    if (!ctx) {
        return;
    }

    Esys_Finalize(&ctx->esys_ctx);
    Tss2_Tcti_Finalize(ctx->tcti_ctx);
    free(ctx->tcti_ctx);
    free(ctx);
}

static bool set_esys_auth(ESYS_CONTEXT *esys_ctx, ESYS_TR handle, twist auth) {

    TPM2B_AUTH tpm_auth = TPM2B_EMPTY_INIT;

    if (auth) {
        size_t auth_len = twist_len(auth);
        if (auth_len > sizeof(tpm_auth.buffer)) {
            LOGE("Auth value too large, got %zu expected < %zu",
                    auth_len, sizeof(tpm_auth.buffer));
            return false;
        }

        tpm_auth.size = auth_len;
        memcpy(tpm_auth.buffer, auth, auth_len);
    }

    TSS2_RC rval = Esys_TR_SetAuth(esys_ctx, handle, &tpm_auth);
    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Esys_TR_SetAuth: 0x%x:", rval);
        return false;
    }

    return true;
}

CK_RV tpm_session_start(tpm_ctx *ctx, twist auth, uint32_t handle) {

    assert(!ctx->hmac_session);

    bool res = set_esys_auth(ctx->esys_ctx, handle, auth);
    if (!res) {
        return CKR_GENERAL_ERROR;
    }

    TPMT_SYM_DEF symmetric = {
        .algorithm = TPM2_ALG_AES,
        .keyBits = { .aes = 128 },
        .mode = { .aes = TPM2_ALG_CFB }
    };

    TPMA_SESSION session_attrs =
        TPMA_SESSION_CONTINUESESSION
      | TPMA_SESSION_DECRYPT
      | TPMA_SESSION_ENCRYPT;

    ESYS_TR session = ESYS_TR_NONE;
    TSS2_RC rc = Esys_StartAuthSession(ctx->esys_ctx,
            handle, //tpmkey
            handle, //bind
            ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
            NULL,
            TPM2_SE_HMAC, &symmetric, TPM2_ALG_SHA256,
            &session);
    if (rc != TSS2_RC_SUCCESS) {
        LOGE("Esys_StartAuthSession: 0x%x", rc);
        return CKR_GENERAL_ERROR;
    }

    rc = Esys_TRSess_SetAttributes(ctx->esys_ctx, session, session_attrs,
                                      0xff);
    if (rc != TSS2_RC_SUCCESS) {
        LOGE("Esys_TRSess_SetAttributes: 0x%x", rc);
        rc = Esys_FlushContext(ctx->esys_ctx,
                session);
        if (rc != TSS2_RC_SUCCESS) {
            LOGW("Esys_FlushContext: 0x%x", rc);
        }
        return CKR_GENERAL_ERROR;
    }

    ctx->original_flags = session_attrs;

    ctx->hmac_session = session;

    return CKR_OK;
}

CK_RV tpm_session_stop(tpm_ctx *ctx) {

    TSS2_RC rc = Esys_FlushContext(ctx->esys_ctx,
            ctx->hmac_session);
    if (rc != TSS2_RC_SUCCESS) {
        LOGE("Esys_FlushContext: 0x%x", rc);
        return CKR_GENERAL_ERROR;
    }

    ctx->hmac_session = 0;

    return CKR_OK;
}

#ifndef ESAPI_MANAGE_FLAGS
#define ESAPI_MANAGE_FLAGS 0
#endif

CK_RV tpm_ctx_new(const char *config, tpm_ctx **tctx) {

    ESYS_CONTEXT *esys = NULL;
    TSS2_TCTI_CONTEXT *tcti = NULL;

    /* no specific config, try environment */
    if (!config) {
        config = getenv(TPM2_PKCS11_TCTI);
    }

    LOGV("tcti=%s", config ? config : "(null)");
    TSS2_RC rc = Tss2_TctiLdr_Initialize(config, &tcti);
    if (rc != TSS2_RC_SUCCESS) {
        return CKR_GENERAL_ERROR;
    }

    tpm_ctx *t = calloc(1, sizeof(*t));
    if (!t) {
        return CKR_HOST_MEMORY;
    }

    esys = esys_ctx_init(tcti);
    if (!esys) {
        goto error;
    }

    /* populate */
    t->esys_ctx = esys;
    t->tcti_ctx = tcti;

    /*
     * allow TPM2_PKCS11_ESAPI_MANAGE_FLAGS to override the configure time default on whether or
     * not ESAPI should manage the flags or if the TPM code should do it.
     */
    const char *c = getenv("TPM2_PKCS11_ESAPI_MANAGE_FLAGS");
    t->esapi_manage_session_flags = c ? true : !!ESAPI_MANAGE_FLAGS;

    /* assign back (return via pointer) */
    *tctx = t;

    return CKR_OK;

error:
    tpm_ctx_free(t);
    return CKR_GENERAL_ERROR;
}

static CK_RV tpm_get_properties(tpm_ctx *ctx, TPMS_CAPABILITY_DATA **d) {

    if (tpms_fixed_property_cache) {
        *d = tpms_fixed_property_cache;
        return CKR_OK;
    }

    assert(!tpms_fixed_property_cache);

    TPM2_CAP capability = TPM2_CAP_TPM_PROPERTIES;
    UINT32 property = TPM2_PT_FIXED;
    UINT32 propertyCount = TPM2_MAX_TPM_PROPERTIES;
    TPMS_CAPABILITY_DATA *capabilityData;
    TPMI_YES_NO moreData;

    TSS2_RC rval = Esys_GetCapability(ctx->esys_ctx,
        ESYS_TR_NONE,
        ESYS_TR_NONE,
        ESYS_TR_NONE,
        capability,
        property, propertyCount, &moreData, &capabilityData);
    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Esys_GetCapability: 0x%x:", rval);
        return CKR_GENERAL_ERROR;
    }

    if (!capabilityData ||
        capabilityData->data.tpmProperties.count < TPM2_PT_VENDOR_STRING_4 - TPM2_PT_FIXED + 1) {
        LOGE("TPM did not reply with correct amount of capabilities");
        Esys_Free(capabilityData);
        return CKR_GENERAL_ERROR;
    }

    *d = tpms_fixed_property_cache = capabilityData;
    return CKR_OK;
}

static CK_RV tpm_get_algs(tpm_ctx *ctx, TPMS_CAPABILITY_DATA **d) {

    if (tpms_alg_cache) {
        *d = tpms_alg_cache;
        return CKR_OK;
    }

    assert(!tpms_alg_cache);

    TPM2_CAP capability = TPM2_CAP_ALGS;
    UINT32 property = TPM2_ALG_FIRST;
    UINT32 propertyCount = TPM2_MAX_CAP_ALGS;
    TPMS_CAPABILITY_DATA *capabilityData;
    TPMI_YES_NO moreData;

    TSS2_RC rval = Esys_GetCapability(ctx->esys_ctx,
        ESYS_TR_NONE,
        ESYS_TR_NONE,
        ESYS_TR_NONE,
        capability,
        property, propertyCount, &moreData, &capabilityData);
    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Esys_GetCapability: 0x%x:", rval);
        return CKR_GENERAL_ERROR;
    }

    if (!capabilityData ||
        capabilityData->data.tpmProperties.count < TPM2_PT_VENDOR_STRING_4 - TPM2_PT_FIXED + 1) {
        LOGE("TPM did not reply with correct amount of capabilities");
        Esys_Free(capabilityData);
        return CKR_GENERAL_ERROR;
    }

    *d = tpms_alg_cache = capabilityData;
    return CKR_OK;
}

static CK_RV find_fixed_cap(TPMS_CAPABILITY_DATA *d, TPM2_PT property, CK_ULONG_PTR value) {

    TPML_TAGGED_TPM_PROPERTY *t = &d->data.tpmProperties;
    TPMS_TAGGED_PROPERTY *p = t->tpmProperty;

    CK_ULONG i;
    for (i=0; i < t->count; i++) {
            if (property == p[i].property) {
                *value = p[i].value;
                return CKR_OK;
            }
    }

    return CKR_MECHANISM_INVALID;
}

static CK_RV find_alg(TPMS_CAPABILITY_DATA *d, TPM2_ALG_ID alg) {

    TPML_ALG_PROPERTY *t = &d->data.algorithms;
    TPMS_ALG_PROPERTY *p = t->algProperties;

    CK_ULONG i;
    for (i=0; i < t->count; i++) {
            if (alg == p[i].alg) {
                return CKR_OK;
            }
    }

    return CKR_MECHANISM_INVALID;
}

static TPM2_ALG_ID mech_to_alg(CK_MECHANISM_TYPE mech) {

    switch(mech) {
        case CKM_AES_CBC:
            return TPM2_ALG_CBC;
        case CKM_AES_CFB1:
            return TPM2_ALG_CFB;
        case CKM_AES_ECB:
            return TPM2_ALG_ECB;
        case CKM_AES_KEY_GEN:
            return TPM2_ALG_AES;
        case CKM_RSA_PKCS_KEY_PAIR_GEN:
            /* falls thru */
        case CKM_RSA_PKCS:
            /* falls thru */
        case CKM_RSA_X_509:
            /* falls thru */
        case CKM_RSA_PKCS_OAEP:
            /* falls thru */
        case CKM_SHA1_RSA_PKCS:
            /* falls thru */
        case CKM_SHA256_RSA_PKCS:
            /* falls thru */
        case CKM_SHA384_RSA_PKCS:
            /* falls thru */
        case CKM_SHA512_RSA_PKCS:
            return TPM2_ALG_RSA;
        case CKM_SHA_1:
            return TPM2_ALG_SHA1;
        case CKM_SHA256:
            return TPM2_ALG_SHA256;
        case CKM_SHA384:
            return TPM2_ALG_SHA384;
        case CKM_SHA512:
            return TPM2_ALG_SHA512;
        case CKM_EC_KEY_PAIR_GEN:
            return TPM2_ALG_ECC;
        case CKM_ECDSA:
            /* falls thru */
        case CKM_ECDSA_SHA1:
            return TPM2_ALG_ECDSA;
        default:
            LOGE("Cannot map mechanism 0x%x onto TPM2 algorithm", mech);
            return TPM2_ALG_ERROR;
    }
}

static CK_RV tpm_find_max_rsa_keysize(tpm_ctx *tctx, CK_ULONG_PTR max) {

    TPMT_PUBLIC_PARMS input = { 0 };

    input.type = TPM2_ALG_RSA;
    input.parameters.rsaDetail.exponent = 0;
    input.parameters.rsaDetail.scheme.scheme = TPM2_ALG_NULL;
    input.parameters.rsaDetail.symmetric.algorithm = TPM2_ALG_NULL;

    CK_ULONG max_found = 0;

    TPM2_KEY_BITS i;
    for(i=2; i < 5; i++) {
        input.parameters.rsaDetail.keyBits = 1024 * i; /* 2048, 3072, 4096... */
        TSS2_RC rval = Esys_TestParms(tctx->esys_ctx,
                ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &input);
        if (rval != TSS2_RC_SUCCESS) {
            if ((rval & (TPM2_RC_P | TPM2_RC_1)) == (TPM2_RC_P | TPM2_RC_1)) {
                rval &= ~(TPM2_RC_P | TPM2_RC_1);
                if (rval == TPM2_RC_KEY_SIZE || rval == TPM2_RC_VALUE) {
                    continue;
                } else {
                    return CKR_MECHANISM_INVALID;
                }
            }
            return CKR_GENERAL_ERROR;
        }

        /* key size was good */
        if (max_found < input.parameters.rsaDetail.keyBits) {
            max_found = input.parameters.rsaDetail.keyBits;
        }
    }

    *max = max_found;

    return CKR_OK;
}

static CK_RV tpm_find_ecc_keysizes(tpm_ctx *tctx, CK_ULONG_PTR min, CK_ULONG_PTR max) {

    TPMT_PUBLIC_PARMS input = { 0 };

    input.type = TPM2_ALG_ECC;
    input.parameters.eccDetail.kdf.scheme = TPM2_ALG_NULL;
    input.parameters.eccDetail.scheme.scheme = TPM2_ALG_NULL;
    input.parameters.eccDetail.symmetric.algorithm = TPM2_ALG_NULL;

    struct {
        TPM2_ALG_ID alg;
        unsigned size;
    } tests[] = {
        { TPM2_ECC_NIST_P192, 192 * 8 },
        { TPM2_ECC_NIST_P224, 224 * 8 },
        { TPM2_ECC_NIST_P256, 256 * 8 },
        { TPM2_ECC_NIST_P384, 384 * 8 },
        { TPM2_ECC_NIST_P521, 521 * 8 },
        { TPM2_ECC_BN_P256,   256 * 8 },
        { TPM2_ECC_BN_P638,   638 * 8 },
        { TPM2_ECC_SM2_P256,  256 * 8 },
    };

    CK_ULONG found_max = 0;
    CK_ULONG found_min = ~0;

    TPM2_ALG_ID i;
    for(i=0; i < ARRAY_LEN(tests); i++) {
        input.parameters.eccDetail.curveID = tests[i].alg;
        TSS2_RC rval = Esys_TestParms(tctx->esys_ctx,
                ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &input);
        if (rval != TSS2_RC_SUCCESS) {
            if ((rval & (TPM2_RC_P | TPM2_RC_1)) == (TPM2_RC_P | TPM2_RC_1)) {
                rval &= ~(TPM2_RC_P | TPM2_RC_1);
                if (rval == TPM2_RC_CURVE) {
                    continue;
                } else {
                    return CKR_MECHANISM_INVALID;
                }
            }
            return CKR_GENERAL_ERROR;
        }

        if (tests[i].size > found_max) {
            found_max = tests[i].size;
        }

        if (tests[i].size < found_min) {
            found_min = tests[i].size;
        }

    }

    *max = found_max;
    *min = found_min;

    return CKR_OK;
}

static CK_RV handle_aes_mech_info(CK_MECHANISM_TYPE t,
        TPMS_CAPABILITY_DATA *p, TPMS_CAPABILITY_DATA *a,
        CK_MECHANISM_INFO_PTR info, CK_FLAGS flag) {

    /* map pkcs11 alg onto tpm2 alg */
    TPM2_ALG_ID alg = mech_to_alg(t);
    if (alg == TPM2_ALG_ERROR) {
        return CKR_MECHANISM_INVALID;
    }

    /* is tpm2 alg supported? */
    CK_RV rv = find_alg(a, alg);
    if (rv != CKR_OK) {
        return rv;
    }

    /* ok it's supported, what are the key sizes? */
    CK_ULONG max;

    rv = find_fixed_cap(p, TPM2_PT_CONTEXT_SYM_SIZE, &max);
    if (rv != CKR_OK) {
        return rv;
    }

    /* if it's supported 128bites is min by spec IIUC */
    /* convert to bytes for return values */
    info->ulMinKeySize = 128/8;
    info->ulMaxKeySize = max/8;
    info->flags = flag;

    return rv;
}

static CK_RV handle_rsa_mech_info(tpm_ctx *tctx, CK_MECHANISM_TYPE t,
        TPMS_CAPABILITY_DATA *a,
        CK_MECHANISM_INFO_PTR info, CK_FLAGS flag) {

    /* map pkcs11 alg onto tpm2 alg */
    TPM2_ALG_ID alg = mech_to_alg(t);
    if (alg == TPM2_ALG_ERROR) {
        return CKR_MECHANISM_INVALID;
    }

    /* is tpm2 alg supported? */
    CK_RV rv = find_alg(a, alg);
    if (rv != CKR_OK) {
        return rv;
    }

    /* ok it's supported, what are the key sizes? */
    CK_ULONG max;
    rv = tpm_find_max_rsa_keysize(tctx, &max);
    if (rv != CKR_OK) {
        return rv;
    }

    /* if it's supported 1024 is min by spec IIUC */
    info->ulMinKeySize = 1024;
    info->ulMaxKeySize = max;
    info->flags = flag;

    return rv;
}

static CK_RV handle_ecc_mech_info(tpm_ctx *tctx, CK_MECHANISM_TYPE t,
        TPMS_CAPABILITY_DATA *a,
        CK_MECHANISM_INFO_PTR info, CK_FLAGS flag) {

    /* map pkcs11 alg onto tpm2 alg */
    TPM2_ALG_ID alg = mech_to_alg(t);
    if (alg == TPM2_ALG_ERROR) {
        return CKR_MECHANISM_INVALID;
    }

    /* is tpm2 alg supported? */
    CK_RV rv = find_alg(a, alg);
    if (rv != CKR_OK) {
        return rv;
    }

    /* ok it's supported, what are the key sizes? */
    CK_ULONG max, min;
    rv = tpm_find_ecc_keysizes(tctx, &min, &max);
    if (rv != CKR_OK) {
        return rv;
    }

    /* TPM reports bits, pkcs11 expects bytes */
    info->ulMinKeySize = min/8;
    info->ulMaxKeySize = max/8;
    info->flags = flag;

    return rv;
}

static CK_RV handle_hash_mech_info(CK_MECHANISM_TYPE t, CK_MECHANISM_INFO_PTR info) {

    /*
     * TODO this should be moved out of TPM layer perhaps? or all hashing should
     * be brought into TPM layer for consistency
     */
    bool is_supported = digest_is_supported(t);
    if (!is_supported) {
        return CKR_MECHANISM_INVALID;
    }

    info->ulMinKeySize = 0;
    info->ulMaxKeySize = 0;
    info->flags = CKF_DIGEST;

    return CKR_OK;
}

CK_RV tpm_get_mech_info(tpm_ctx *ctx, CK_MECHANISM_TYPE t, CK_MECHANISM_INFO_PTR info) {

    check_pointer(ctx);
    check_pointer(info);

    TPMS_CAPABILITY_DATA *fixed_property_data = NULL;
    CK_RV rv = tpm_get_properties(ctx, &fixed_property_data);
    if (rv != CKR_OK) {
        return rv;
    }

    TPMS_CAPABILITY_DATA *alg_property_data = NULL;
    rv = tpm_get_algs(ctx, &alg_property_data);
    if (rv != CKR_OK) {
        return rv;
    }

    CK_FLAGS flags = 0;

    switch(t) {
    /* AES based crypto */
    case CKM_AES_KEY_GEN:
        flags = CKF_GENERATE;
        /* fall-thru */
    case CKM_AES_CBC:
    case CKM_AES_CFB1:
    case CKM_AES_ECB:
        flags |= CKF_HW;
        return handle_aes_mech_info(t, fixed_property_data, alg_property_data, info, flags);
    /* RSA based crypto */
    case CKM_RSA_PKCS_KEY_PAIR_GEN:
        flags = CKF_HW | CKF_GENERATE_KEY_PAIR;
        return handle_rsa_mech_info(ctx, t, alg_property_data, info, flags);
    case CKM_RSA_PKCS:
    case CKM_RSA_X_509:
        flags = CKF_HW | CKF_ENCRYPT | CKF_DECRYPT | CKF_SIGN | CKF_VERIFY;
        return handle_rsa_mech_info(ctx, t, alg_property_data, info, flags);
    case CKM_RSA_PKCS_OAEP:
        flags = CKF_HW | CKF_ENCRYPT| CKF_DECRYPT;
        return handle_rsa_mech_info(ctx, t, alg_property_data, info, flags);
    case CKM_SHA1_RSA_PKCS:
    case CKM_SHA256_RSA_PKCS:
    case CKM_SHA384_RSA_PKCS:
    case CKM_SHA512_RSA_PKCS:
        flags = CKF_HW | CKF_SIGN | CKF_VERIFY;
        return handle_rsa_mech_info(ctx, t, alg_property_data, info, flags);
    case CKM_EC_KEY_PAIR_GEN:
        flags = CKF_HW | CKF_GENERATE_KEY_PAIR;
        return handle_ecc_mech_info(ctx, t, alg_property_data, info, flags);
    case CKM_ECDSA:
    case CKM_ECDSA_SHA1:
        flags = CKF_HW | CKF_SIGN | CKF_VERIFY;
        return handle_ecc_mech_info(ctx, t, alg_property_data, info, flags);
    /* Hashes */
    case CKM_SHA_1:
    case CKM_SHA256:
    case CKM_SHA384:
    case CKM_SHA512:
        return handle_hash_mech_info(t, info);
        /* no default */
    }

    return CKR_MECHANISM_INVALID;
}

CK_RV tpm_get_token_info (tpm_ctx *ctx, CK_TOKEN_INFO *info) {

    check_pointer(ctx);
    check_pointer(info);

    TPMS_CAPABILITY_DATA *capabilityData = NULL;

    CK_RV rv = tpm_get_properties(ctx, &capabilityData);
    if (rv !=CKR_OK) {
        return rv;
    }

    TPMS_TAGGED_PROPERTY *tpmProperties = capabilityData->data.tpmProperties.tpmProperty;

    // Use Spec revision as HW Version
    UINT32 revision = tpmProperties[TPM2_PT_REVISION - TPM2_PT_FIXED].value;
    info->hardwareVersion.major = revision / 100;
    info->hardwareVersion.minor = revision % 100;

    // Use Firmware Version as FW Version
    UINT32 version = tpmProperties[TPM2_PT_FIRMWARE_VERSION_1 - TPM2_PT_FIXED].value;
    // Most vendors seem to use 00MM.00mm as format for TPM2_PT_FIRMWARE_VERSION_1
    // Unfortunately we only have 1 byte for major and minor each.
    info->firmwareVersion.major = (version >> 16) & 0xFF;
    info->firmwareVersion.minor = version  & 0xFF;

    // Use Vendor ID as Manufacturer ID
    unsigned char manufacturerID[sizeof(UINT32)+1] = {0}; // 4 bytes + '\0' as temp storage
    UINT32 manufacturer = ntohl(tpmProperties[TPM2_PT_MANUFACTURER - TPM2_PT_FIXED].value);
    memcpy(manufacturerID, (unsigned char*) &manufacturer, sizeof(uint32_t));
    str_padded_copy(info->manufacturerID, manufacturerID, sizeof(info->manufacturerID));

    // Map human readable Manufacturer String, if available,
    // otherwise 4 byte ID was already padded and will be used.
    for (unsigned int i=0; i < ARRAY_LEN(TPM2_MANUFACTURER_MAP); i++){
        if (!strncasecmp((char *)info->manufacturerID, TPM2_MANUFACTURER_MAP[i][0], 4)) {
            str_padded_copy(info->manufacturerID,
                            (unsigned char *)TPM2_MANUFACTURER_MAP[i][1],
                            sizeof(info->manufacturerID));
        }
    }

    // Use Vendor String as Model description
    UINT32 vendor[4];
    vendor[0] = ntohl(tpmProperties[TPM2_PT_VENDOR_STRING_1 - TPM2_PT_FIXED].value);
    vendor[1] = ntohl(tpmProperties[TPM2_PT_VENDOR_STRING_2 - TPM2_PT_FIXED].value);
    vendor[2] = ntohl(tpmProperties[TPM2_PT_VENDOR_STRING_3 - TPM2_PT_FIXED].value);
    vendor[3] = ntohl(tpmProperties[TPM2_PT_VENDOR_STRING_4 - TPM2_PT_FIXED].value);
    str_padded_copy(info->model, (unsigned char*) &vendor, sizeof(info->model));

    return CKR_OK;
}

bool tpm_getrandom(tpm_ctx *ctx, BYTE *data, size_t size) {

    size_t offset = 0;

    bool result = false;

    /*
     * This will get re-used once allocated by esys
     */
    TPM2B_DIGEST *rand_bytes = NULL;

    while (size) {

        UINT16 request_size = size > sizeof(rand_bytes->buffer) ?
                sizeof(rand_bytes->buffer) : size;

        TSS2_RC rval = Esys_GetRandom(
            ctx->esys_ctx,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            request_size,
            &rand_bytes);
        if (rval != TSS2_RC_SUCCESS) {
            LOGE("Esys_GetRandom: 0x%x:", rval);
            goto out;
        }

        memcpy(&data[offset], rand_bytes->buffer, rand_bytes->size);

        offset += rand_bytes->size;
        size -= rand_bytes->size;
    }

    result = true;

out:
    free(rand_bytes);

    return result;
}

CK_RV tpm_stirrandom(tpm_ctx *ctx, CK_BYTE_PTR seed, CK_ULONG seed_len) {

    TSS2_RC rc = TSS2_TCTI_RC_GENERAL_FAILURE;;

    size_t offset = 0;
    while(offset < seed_len) {
        TPM2B_SENSITIVE_DATA stir;

        size_t left = seed_len - offset;
        size_t chunk = left > sizeof(stir.buffer) ? sizeof(stir.buffer) : left;

        stir.size = chunk;
        memcpy(stir.buffer, &seed[offset], chunk);

        rc = Esys_StirRandom(
            ctx->esys_ctx,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            &stir);
        if (rc != TSS2_RC_SUCCESS) {
            LOGE("Esys_StirRandom: 0x%x:", rc);
            return CKR_GENERAL_ERROR;
        }

        offset += seed_len;
    }

    return CKR_OK;
}

bool tpm_deserialize_handle(tpm_ctx *ctx, twist handle_blob, uint32_t *handle) {

    TSS2_RC rval = Esys_TR_Deserialize(ctx->esys_ctx,
                        (uint8_t *)handle_blob,
                        twist_len(handle_blob), handle);
    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Esys_TR_Deserialize: 0x%x:", rval);
        return false;
    }

    return true;
}

static bool tpm_load(tpm_ctx *ctx,
        uint32_t phandle,
        TPM2B_PUBLIC *pub, twist priv_data,
        uint32_t *handle) {

    TPM2B_PRIVATE priv = { .size = 0 };
    size_t len = twist_len(priv_data);

    size_t offset = 0;
    TSS2_RC rval = Tss2_MU_TPM2B_PRIVATE_Unmarshal((uint8_t *)priv_data, len, &offset, &priv);
    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Tss2_MU_TPM2B_PRIVATE_Unmarshal: 0x%x:", rval);
        return false;
    }

    rval = Esys_Load(
           ctx->esys_ctx,
           phandle,
           ctx->hmac_session,
           ESYS_TR_NONE,
           ESYS_TR_NONE,
           &priv,
           pub,
           handle);
    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Esys_Load: 0x%x:", rval);
        return false;
    }

    return true;
}

static bool tpm_loadexternal(tpm_ctx *ctx,
        TPM2B_PUBLIC *pub,
        uint32_t *handle) {

    TSS2_RC rval = Esys_LoadExternal(
           ctx->esys_ctx,
           ESYS_TR_NONE,
           ESYS_TR_NONE,
           ESYS_TR_NONE,
           NULL,
           pub,
           TPM2_RH_NULL,
           handle);
    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Esys_LoadExternal: 0x%x:", rval);
        return false;
    }

    return true;
}

bool tpm_loadobj(
        tpm_ctx *ctx,
        uint32_t phandle, twist auth,
        twist pub_data, twist priv_data,
        uint32_t *handle) {

    TPM2B_PUBLIC pub = { .size = 0 };
    size_t len = twist_len(pub_data);

    size_t offset = 0;
    TSS2_RC rval = Tss2_MU_TPM2B_PUBLIC_Unmarshal((uint8_t *)pub_data, len, &offset, &pub);
    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Tss2_MU_TPM2B_PRIVATE_Unmarshal: 0x%x:", rval);
        return false;
    }

    bool tmp_rc = set_esys_auth(ctx->esys_ctx, phandle, auth);
    if (!tmp_rc) {
        return false;
    }

    if (priv_data) {
        return tpm_load(ctx, phandle, &pub, priv_data, handle);
    }

    return tpm_loadexternal(ctx, &pub, handle);
}

bool tpm_flushcontext(tpm_ctx *ctx, uint32_t handle) {

    TSS2_RC rval = Esys_FlushContext(
                ctx->esys_ctx,
                handle);
    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Esys_FlushContext: 0x%x", rval);
        return false;
    }

    return true;
}

TPMI_ALG_HASH hashlen_to_alg_guess(CK_ULONG datalen) {
    switch (datalen) {
        case SHA_DIGEST_LENGTH:
            return TPM2_ALG_SHA1;
        case SHA256_DIGEST_LENGTH:
            return TPM2_ALG_SHA256;
        case SHA384_DIGEST_LENGTH:
            return TPM2_ALG_SHA384;
        case SHA512_DIGEST_LENGTH:
            return TPM2_ALG_SHA512;
        default:
            LOGE("unkown digest length");
            return TPM2_ALG_ERROR;
    }
}

TPMI_ALG_HASH mech_to_hash_alg_ex(CK_MECHANISM_TYPE mode, CK_ULONG datalen) {

    switch (mode) {
    case CKM_RSA_PKCS:
        return TPM2_ALG_NULL;

    case CKM_SHA1_RSA_PKCS:
    case CKM_SHA_1:
        return TPM2_ALG_SHA1;

    case CKM_SHA256_RSA_PKCS:
    case CKM_SHA256:
        return TPM2_ALG_SHA256;

    case CKM_SHA384_RSA_PKCS:
    case CKM_SHA384:
        return TPM2_ALG_SHA384;

    case CKM_SHA512_RSA_PKCS:
    case CKM_SHA512:
        return TPM2_ALG_SHA512;

    case CKM_ECDSA_SHA1:
        return TPM2_ALG_SHA1;
    case CKM_ECDSA:
        // ECDSA is NOT using a hash.
        // It needs a length (determined by the name of an hash alg) anyway.
        // The length/hash_alg with correct length will be specified later.
        return datalen ? hashlen_to_alg_guess(datalen) : TPM2_ALG_ERROR;

    default:
        return TPM2_ALG_ERROR;
    }
}

TPMI_ALG_HASH mech_to_hash_alg(CK_MECHANISM_TYPE mode) {

    return mech_to_hash_alg_ex(mode, 0);
}


TPM2_ALG_ID mech_to_sig_scheme(CK_MECHANISM_TYPE mode) {

    switch (mode) {
    case CKM_SHA1_RSA_PKCS:
    case CKM_SHA256_RSA_PKCS:
    case CKM_SHA384_RSA_PKCS:
    case CKM_SHA512_RSA_PKCS:
        return TPM2_ALG_RSASSA;
    case CKM_ECDSA:
    case CKM_ECDSA_SHA1:
        return TPM2_ALG_ECDSA;
    default:
        return TPM2_ALG_ERROR;
    }
}

bool get_signature_scheme(CK_MECHANISM_TYPE mech, CK_ULONG datalen, TPMT_SIG_SCHEME *scheme) {

    TPM2_ALG_ID sig_scheme = mech_to_sig_scheme(mech);
    if (sig_scheme == TPM2_ALG_ERROR) {
        LOGE("Connot convert mechanism to signature scheme, got: 0x%lx", mech);
        return false;
    }

    TPMI_ALG_HASH halg = mech_to_hash_alg_ex(mech, datalen);
    if (halg == TPM2_ALG_ERROR) {
        LOGE("Connot convert mechanism to hash algorithm, got: 0x%lx", mech);
        return false;
    }

    scheme->scheme = sig_scheme;
    scheme->details.any.hashAlg = halg;

    return true;
}

twist tpm_unseal(tpm_ctx *ctx, uint32_t handle, twist objauth) {

    twist t = NULL;

    bool result = set_esys_auth(ctx->esys_ctx, handle, objauth);
    if (!result) {
        return false;
    }

    TPM2B_SENSITIVE_DATA *unsealed_data = NULL;

    flags_turndown(ctx, TPMA_SESSION_DECRYPT);

    TSS2_RC rc = Esys_Unseal(
            ctx->esys_ctx,
            handle,
            ctx->hmac_session,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            &unsealed_data);
    if (rc != TPM2_RC_SUCCESS) {
        LOGE("Tss2_Sys_Unseal: 0x%X", rc);
        goto out;
    }

    t = twistbin_new(unsealed_data->buffer, unsealed_data->size);

    free(unsealed_data);
out:

    flags_restore(ctx);
    return t;
}

static CK_RV flatten_rsassa(TPMS_SIGNATURE_RSASSA *rsassa, CK_BYTE_PTR sig, CK_ULONG_PTR siglen) {

    if (sig && *siglen < rsassa->sig.size) {
        *siglen = rsassa->sig.size;
        return CKR_BUFFER_TOO_SMALL;
    }

    *siglen = rsassa->sig.size;

    if (sig) {
        memcpy(sig, rsassa->sig.buffer, *siglen);
    }

    return CKR_OK;
}

static CK_RV flatten_ecdsa(TPMS_SIGNATURE_ECDSA *ecdsa, CK_BYTE_PTR sig, CK_ULONG_PTR siglen) {

    TPM2B_ECC_PARAMETER *R = &ecdsa->signatureR;
    TPM2B_ECC_PARAMETER *S = &ecdsa->signatureS;

    /*
     * From PKCS#11 Current Mechanisms Specification:
     *
     * "For the purposes of these mechanisms, an ECDSA signature is an
     * octet string of even length which is at most two times nLen
     * octets, where nLen is the length in octets of the base point
     * order n. The signature octets correspond to the concatenation
     * of the ECDSA values r and s, both represented as an octet
     * string of equal length of at most nLen with the most
     * significant byte first."
     */

    /*
     * From TCG TPM 2.0, Part 1: Architecture, Appendix C.8:
     *
     * "When ECC parameters are returned by the TPM as output
     * parameters in a response, they must be padded with zeros to the
     * length of the respective curve (e.g., 32 bytes for NIST
     * P-256)."
     */

    if (R->size != S->size) {
        LOGE("TPM returned ECC signature with inconsistent padding");
        return CKR_DEVICE_ERROR;
    }

    if (!sig) {
        *siglen = R->size + S->size;
        return CKR_OK;
    }

    if (R->size + S->size > *siglen) {
        *siglen = R->size + S->size;
        return CKR_BUFFER_TOO_SMALL;
    }

    memcpy(sig, R->buffer, R->size);
    memcpy(sig + R->size, S->buffer, S->size);
    *siglen = R->size + S->size;
    return CKR_OK;
}

static CK_RV sig_flatten(TPMT_SIGNATURE *signature, TPMT_SIG_SCHEME *scheme, CK_BYTE_PTR sig, CK_ULONG_PTR siglen) {

    switch(scheme->scheme) {
    case TPM2_ALG_RSASSA:
        return flatten_rsassa(&signature->signature.rsassa, sig, siglen);
    case TPM2_ALG_ECDSA:
        return flatten_ecdsa(&signature->signature.ecdsa, sig, siglen);
        /* no default */
    }

    return false;
}

CK_RV tpm_sign(tpm_ctx *ctx, tobject *tobj, CK_MECHANISM_TYPE mech, CK_BYTE_PTR data, CK_ULONG datalen, CK_BYTE_PTR sig, CK_ULONG_PTR siglen) {

    twist auth = tobj->unsealed_auth;
    TPMI_DH_OBJECT handle = tobj->handle;

    TPM2B_DIGEST tdigest;
    if (sizeof(tdigest.buffer) < datalen) {
        return CKR_DATA_LEN_RANGE;
    }
    memcpy(tdigest.buffer, data, datalen);
    tdigest.size = datalen;

    bool result = set_esys_auth(ctx->esys_ctx, handle, auth);
    if (!result) {
        return CKR_GENERAL_ERROR;
    }

    TPMT_SIG_SCHEME in_scheme;
    result = get_signature_scheme(mech, datalen, &in_scheme);
    assert(result);
    if (!result) {
        /*
         * do not return unsupported here
         * this should be done in C_SignInit()
         * In theory this cannot fail
         */
        return CKR_GENERAL_ERROR;
    }

    TPMT_TK_HASHCHECK validation = {
        .tag = TPM2_ST_HASHCHECK,
        .hierarchy = TPM2_RH_NULL,
        .digest = TPM2B_EMPTY_INIT
    };

    flags_turndown(ctx, TPMA_SESSION_ENCRYPT);

    TPMT_SIGNATURE *signature = NULL;
    TSS2_RC rval = Esys_Sign(
            ctx->esys_ctx,
            handle,
            ctx->hmac_session,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            &tdigest,
            &in_scheme,
            &validation,
            &signature);
    flags_restore(ctx);
    if (rval != TPM2_RC_SUCCESS) {
        LOGE("Esys_Sign: 0x%0x", rval);
        return CKR_GENERAL_ERROR;
    }

    CK_RV rv = sig_flatten(signature, &in_scheme, sig, siglen);

    free(signature);

    return rv;
}

static CK_RV init_rsassa_sig(CK_BYTE_PTR sig, CK_ULONG siglen, TPMS_SIGNATURE_RSASSA *rsassa) {

    if (siglen > sizeof(rsassa->sig.buffer)) {
        return CKR_SIGNATURE_LEN_RANGE;
    }

    rsassa->sig.size = siglen;
    memcpy(rsassa->sig.buffer, sig, siglen);

    return CKR_OK;
}

static CK_RV init_ecdsa_sig(CK_BYTE_PTR sig, CK_ULONG siglen, TPMS_SIGNATURE_ECDSA *ecdsa) {

    TPM2B_ECC_PARAMETER *R = &ecdsa->signatureR;
    TPM2B_ECC_PARAMETER *S = &ecdsa->signatureS;

    // See comments in flatten_ecdsa about signature format.

    if ((siglen % 2) != 0) {
        return CKR_SIGNATURE_LEN_RANGE;
    }

    CK_ULONG coordlen = siglen / 2;
    if (coordlen > sizeof(R->buffer)) {
        return CKR_SIGNATURE_LEN_RANGE;
    }

    R->size = coordlen;
    memcpy(R->buffer, sig, coordlen);

    S->size = coordlen;
    memcpy(S->buffer, sig + coordlen, coordlen);

    return CKR_OK;
}

static CK_RV init_sig_from_mech(CK_MECHANISM_TYPE mech, CK_ULONG datalen, CK_BYTE_PTR sig, CK_ULONG siglen, TPMT_SIGNATURE *tpmsig) {

    /*
     * VerifyInit should be verifying that the mech and sig is supported, so
     * we can't return that error code here as PKCS11 doesn't support it,
     * so just return general error.
     */
    tpmsig->sigAlg = mech_to_sig_scheme(mech);
    if (tpmsig->sigAlg == TPM2_ALG_ERROR) {
        return CKR_GENERAL_ERROR;
    }

    tpmsig->signature.any.hashAlg = mech_to_hash_alg_ex(mech, datalen);
    if (tpmsig->signature.any.hashAlg == TPM2_ALG_ERROR) {
        return CKR_GENERAL_ERROR;
    }

    switch(tpmsig->sigAlg) {
    case TPM2_ALG_RSASSA:
        return init_rsassa_sig(sig, siglen, &tpmsig->signature.rsassa);
    case TPM2_ALG_ECDSA:
        return init_ecdsa_sig(sig, siglen, &tpmsig->signature.ecdsa);
    default:
        LOGE("Unsupported verification algorithm, got: 0x%lx", mech);
        return CKR_GENERAL_ERROR;
    }
}

CK_RV tpm_verify(tpm_ctx *ctx, tobject *tobj, CK_MECHANISM_TYPE mech, CK_BYTE_PTR data, CK_ULONG datalen, CK_BYTE_PTR sig, CK_ULONG siglen) {

    TPMI_DH_OBJECT handle = tobj->handle;

    // Copy the data into the digest block
    TPM2B_DIGEST msgdigest;
    if (sizeof(msgdigest.buffer) < datalen) {
        return CKR_DATA_LEN_RANGE;
    }
    memcpy(msgdigest.buffer, data, datalen);
    msgdigest.size = datalen;

    TPMT_SIGNATURE tpmsig;
    CK_RV rv = init_sig_from_mech(mech, datalen, sig, siglen, &tpmsig);
    if (rv != CKR_OK) {
        return rv;
    }

    TPMT_TK_VERIFIED *validation = NULL;
    TSS2_RC rval = Esys_VerifySignature(
            ctx->esys_ctx,
            handle,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            &msgdigest,
            &tpmsig,
            &validation);
    if (rval != TPM2_RC_SUCCESS) {
        if (rval != TPM2_RC_SIGNATURE) {
            LOGE("Esys_VerifySignature: 0x%x", rval);
            return CKR_GENERAL_ERROR;
        }
        return CKR_SIGNATURE_INVALID;
    }

    free(validation);
    return CKR_OK;
}

CK_RV tpm_readpub(tpm_ctx *ctx,
        uint32_t handle,

        TPM2B_PUBLIC **public,
        TPM2B_NAME **name,
        TPM2B_NAME **qualified_name) {

    TSS2_RC rval = TSS2_RETRY_EXP(Esys_ReadPublic(ctx->esys_ctx, handle, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
            public, name, qualified_name));
    if (rval != TPM2_RC_SUCCESS) {
        LOGE("Esys_ReadPublic: 0x%x", rval);
        return CKR_GENERAL_ERROR;
    }

    return CKR_OK;
}

struct tpm_encrypt_data {
    tpm_ctx *ctx;

    uint32_t handle;
    twist auth;

    bool is_rsa;

    union {
        struct {
            TPMT_RSA_DECRYPT scheme;
            TPM2B_DATA label;
        } rsa;
        struct {
            TPMI_ALG_SYM_MODE mode;
            TPM2B_IV iv;
        } sym;
    };
};

static CK_RV mech_to_sym(CK_MECHANISM_PTR mech, tpm_encrypt_data *tpm_enc_data) {

    switch(mech->mechanism) {
    case CKM_AES_CBC:
        tpm_enc_data->sym.mode = TPM2_ALG_CBC;
        break;
    case CKM_AES_ECB:
        tpm_enc_data->sym.mode = TPM2_ALG_ECB;
        break;
    case CKM_AES_CFB1:
        tpm_enc_data->sym.mode = TPM2_ALG_CFB;
        break;
    default:
        LOGE("Unsupported mechanism: 0x%lx", mech->mechanism);
        return CKR_MECHANISM_INVALID;
    }

    if (mech->ulParameterLen > 0) {

        if (mech->ulParameterLen > sizeof(tpm_enc_data->sym.iv.buffer)) {
            return CKR_MECHANISM_PARAM_INVALID;
        }

        tpm_enc_data->sym.iv.size = mech->ulParameterLen;
        memcpy(tpm_enc_data->sym.iv.buffer, mech->pParameter, mech->ulParameterLen);
    } else {
        /* initialize to 16 zeros if IV not specified */
        tpm_enc_data->sym.iv.size = sizeof(tpm_enc_data->sym.iv.buffer);
        memset(tpm_enc_data->sym.iv.buffer, 0, sizeof(tpm_enc_data->sym.iv.buffer));
    }

    return CKR_OK;
}

static CK_RV mech_to_rsa_raw(CK_MECHANISM_PTR mech, tpm_encrypt_data *encdata) {
    UNUSED(mech);

    encdata->rsa.scheme.scheme = TPM2_ALG_NULL;

    encdata->rsa.label.size = 0;

    return CKR_OK;
}

static CK_RV get_oaep_mgf1_alg(tpm_ctx *tpm, uint32_t handle, CK_RSA_PKCS_MGF_TYPE_PTR mgf) {

    TPM2B_PUBLIC *public = NULL;
    TPM2B_NAME *name = NULL;
    TPM2B_NAME *qualified_name = NULL;

    CK_RV rv = tpm_readpub(tpm, handle, &public, &name, &qualified_name);
    if (rv != CKR_OK) {
        return rv;
    }

    switch(public->publicArea.nameAlg) {
    case TPM2_ALG_SHA1:
        *mgf = CKG_MGF1_SHA1;
        break;
    case TPM2_ALG_SHA256:
        *mgf = CKG_MGF1_SHA256;
        break;
    case TPM2_ALG_SHA384:
        *mgf = CKG_MGF1_SHA384;
        break;
    case TPM2_ALG_SHA512:
        *mgf = CKG_MGF1_SHA512;
        break;
    default:
        rv = CKR_GENERAL_ERROR;
    }

    free(public);
    free(name);
    free(qualified_name);

    return rv;
}

static CK_RV mech_to_rsa_oaep(tpm_ctx *tpm, CK_MECHANISM_PTR mech, tpm_encrypt_data *encdata) {

    encdata->rsa.scheme.scheme = TPM2_ALG_OAEP;

    if (mech->ulParameterLen != sizeof(CK_RSA_PKCS_OAEP_PARAMS)) {
        return CKR_MECHANISM_PARAM_INVALID;
    }

    CK_RSA_PKCS_OAEP_PARAMS_PTR params = (CK_RSA_PKCS_OAEP_PARAMS_PTR)mech->pParameter;

    if (params->source != CKZ_DATA_SPECIFIED
            && params->pSourceData != NULL
            && params->ulSourceDataLen != 0) {
        return CKR_MECHANISM_PARAM_INVALID;
    }

    /*
     * TPM is hardcoded to MGF1 + <name alg> in the TPM, make sure what is requested is supported
     */
    CK_RSA_PKCS_MGF_TYPE supported_mgf;
    CK_RV rv = get_oaep_mgf1_alg(tpm, encdata->handle, &supported_mgf);
    if (rv != CKR_OK) {
        return rv;
    }
    /*  TODO revisit - why does it return not supported here. It works though
    if (params->mgf != supported_mgf) {
        return CKR_MECHANISM_PARAM_INVALID;
    }
    */

    encdata->rsa.scheme.details.oaep.hashAlg = mech_to_hash_alg(params->hashAlg);
    if (encdata->rsa.scheme.details.oaep.hashAlg == TPM2_ALG_ERROR) {
        return CKR_MECHANISM_PARAM_INVALID;
    }

    if (params->ulSourceDataLen > sizeof(encdata->rsa.label.buffer)) {
        return CKR_MECHANISM_PARAM_INVALID;
    }

    encdata->rsa.label.size = params->ulSourceDataLen;
    if (params->ulSourceDataLen) {
        if (!params->pSourceData) {
            return CKR_MECHANISM_PARAM_INVALID;
        }
        memcpy(encdata->rsa.label.buffer, params->pSourceData, params->ulSourceDataLen);
    }

    return CKR_OK;
}

CK_RV tpm_encrypt_data_init(tpm_ctx *ctx, uint32_t handle, twist auth, CK_MECHANISM_PTR mech, tpm_encrypt_data **encdata) {

    CK_RV rv = CKR_MECHANISM_INVALID;

    tpm_encrypt_data *tpm_enc_data = calloc(1, sizeof(*tpm_enc_data));
    if (!tpm_enc_data) {
        return CKR_HOST_MEMORY;
    }

    tpm_enc_data->ctx = ctx;
    tpm_enc_data->handle = handle;
    tpm_enc_data->auth = auth;

    switch(mech->mechanism) {
        case CKM_RSA_X_509:
        case CKM_RSA_PKCS:
        case CKM_SHA1_RSA_PKCS:
        case CKM_SHA256_RSA_PKCS:
        case CKM_SHA384_RSA_PKCS:
        case CKM_SHA512_RSA_PKCS:
            tpm_enc_data->is_rsa = true;
            rv = mech_to_rsa_raw(mech, tpm_enc_data);
            break;

        case CKM_RSA_PKCS_OAEP:
            tpm_enc_data->is_rsa = true;
            rv = mech_to_rsa_oaep(ctx, mech, tpm_enc_data);
            break;

        case CKM_AES_CBC:
        case CKM_AES_ECB:
        case CKM_AES_CFB1:
            rv = mech_to_sym(mech, tpm_enc_data);
            break;
        /* no default */
    }

    if (rv == CKR_OK) {
        *encdata = tpm_enc_data;
    } else {
        free(tpm_enc_data);
    }

    return rv;
}

void tpm_encrypt_data_free(tpm_encrypt_data *encdata) {

    free(encdata);
}

CK_RV tpm_rsa_decrypt(tpm_encrypt_data *tpm_enc_data,
        CK_BYTE_PTR ctext, CK_ULONG ctextlen,
        CK_BYTE_PTR ptext, CK_ULONG_PTR ptextlen) {

    CK_RV rv = CKR_GENERAL_ERROR;

    tpm_ctx *ctx = tpm_enc_data->ctx;

    TPMT_RSA_DECRYPT *scheme = &tpm_enc_data->rsa.scheme;
    TPM2B_DATA *label = &tpm_enc_data->rsa.label;

    /*
     * Validate that the data to perform the operation on, typically
     * ciphertext on RSA decrypt, fits in the buffer for the TPM and
     * populate it.
     */
    TPM2B_PUBLIC_KEY_RSA tpm_ctext = { .size = ctextlen };
    if (ctextlen > sizeof(tpm_ctext.buffer)) {
        return CKR_ARGUMENTS_BAD;
    }
    memcpy(tpm_ctext.buffer, ctext, ctextlen);

    twist auth = tpm_enc_data->auth;
    ESYS_TR handle = tpm_enc_data->handle;
    bool result = set_esys_auth(ctx->esys_ctx, handle, auth);
    if (!result) {
        return CKR_GENERAL_ERROR;
    }

    TPM2B_PUBLIC_KEY_RSA *tpm_ptext;

    TSS2_RC rc = Esys_RSA_Decrypt(
            ctx->esys_ctx,
            handle,
            ctx->hmac_session,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            &tpm_ctext,
            scheme,
            label,
            &tpm_ptext);
    if (rc != TPM2_RC_SUCCESS) {
        LOGE("Esys_RSA_Decrypt: 0x%x", rc);
        return CKR_GENERAL_ERROR;
    }

    if (!ptext) {
        *ptextlen = tpm_ctext.size;
        rv = CKR_OK;
        goto out;
    }

    if (*ptextlen < tpm_ctext.size) {
        *ptextlen = tpm_ctext.size;
        rv = CKR_BUFFER_TOO_SMALL;
        goto out;
    }

    *ptextlen = tpm_ptext->size;
    memcpy(ptext, tpm_ptext->buffer, tpm_ptext->size);

    rv = CKR_OK;

out:
    free(tpm_ptext);

    return rv;
}

CK_RV tpm_rsa_encrypt(tpm_encrypt_data *tpm_enc_data,
        CK_BYTE_PTR pptext, CK_ULONG pptextlen,
        CK_BYTE_PTR cctext, CK_ULONG_PTR cctextlen) {

    CK_RV rv = CKR_GENERAL_ERROR;

    tpm_ctx *ctx = tpm_enc_data->ctx;

    TPMT_RSA_DECRYPT *scheme = &tpm_enc_data->rsa.scheme;
    TPM2B_DATA *label = &tpm_enc_data->rsa.label;

    /*
     * Validate that plaintext data fits in a message buffer.
     * Do this first since it requires no trip to the TPM
     * to verify or memory allocation.
     */
    TPM2B_PUBLIC_KEY_RSA message = { .size = pptextlen };
    if (pptextlen > sizeof(message.buffer)) {
        return CKR_ARGUMENTS_BAD;
    }
    memcpy(message.buffer, pptext, pptextlen);

    ESYS_TR handle = tpm_enc_data->handle;

    TPM2B_PUBLIC_KEY_RSA *ctext;

    TSS2_RC rc = Esys_RSA_Encrypt(
            ctx->esys_ctx,
            handle,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            &message,
            scheme,
            label,
            &ctext);
    if (rc != TPM2_RC_SUCCESS) {
        LOGE("Esys_RSA_Encrypt: 0x%x", rc);
        return CKR_GENERAL_ERROR;
    }

    if (!cctext) {
        *cctextlen = ctext->size;
        rv = CKR_OK;
        goto out;
    }

    if (*cctextlen < ctext->size) {
        *cctextlen = ctext->size;
        rv = CKR_BUFFER_TOO_SMALL;
        goto out;
    }

    *cctextlen = ctext->size;
    memcpy(cctext, ctext->buffer, ctext->size);

    rv = CKR_OK;

out:
    free(ctext);

    return rv;
}

static CK_RV encrypt_decrypt(tpm_ctx *ctx, uint32_t handle, twist objauth, TPMI_ALG_SYM_MODE mode, TPMI_YES_NO is_decrypt,
        TPM2B_IV *iv, CK_BYTE_PTR data_in, CK_ULONG data_in_len, CK_BYTE_PTR data_out, CK_ULONG_PTR data_out_len) {

    CK_RV rv = CKR_GENERAL_ERROR;

    bool result = set_esys_auth(ctx->esys_ctx, handle, objauth);
    if (!result) {
        return CKR_GENERAL_ERROR;
    }

    /*
     * Copy the data into TPM structures
     */
    TPM2B_MAX_BUFFER tpm_data_in = {
         .size = data_in_len,
    };

    if (data_in_len > sizeof(tpm_data_in.buffer)) {
        return false;
    }

    memcpy(tpm_data_in.buffer, data_in, tpm_data_in.size);

    TPM2B_IV empty_iv_in = { .size = sizeof(empty_iv_in.buffer), .buffer = { 0 } };
    if (!iv) {
        iv = &empty_iv_in;
    }

    /* setup the output structures */
    TPM2B_MAX_BUFFER *tpm_data_out = NULL;
    TPM2B_IV *tpm_iv_out = NULL;

    unsigned version = 2;

    TSS2_RC rval =
        Esys_EncryptDecrypt2(
            ctx->esys_ctx,
            handle,
            ctx->hmac_session,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            &tpm_data_in,
            is_decrypt,
            mode,
            iv,
            &tpm_data_out,
            &tpm_iv_out);

    if (tpm2_error_get(rval) == TPM2_RC_COMMAND_CODE) {
        version = 1;

        flags_turndown(ctx, TPMA_SESSION_DECRYPT);
        rval = Esys_EncryptDecrypt(
            ctx->esys_ctx,
            handle,
            ctx->hmac_session,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            is_decrypt,
            mode,
            iv,
            &tpm_data_in,
            &tpm_data_out,
            &tpm_iv_out);
        flags_restore(ctx);
    }

    if(rval != TSS2_RC_SUCCESS) {
        LOGE("Esys_EncryptDecrypt%u: 0x%x", version, rval);
        return CKR_GENERAL_ERROR;
    }

    assert(tpm_data_out);
    assert(tpm_iv_out);

    if (!data_out) {
        *data_out_len = tpm_data_out->size;
        rv = CKR_OK;
        goto out;
    }

    if (tpm_data_out->size > *data_out_len) {
        *data_out_len = tpm_data_out->size;
        rv = CKR_BUFFER_TOO_SMALL;
        goto out;
    }

    *data_out_len = tpm_data_out->size;
    memcpy(data_out, tpm_data_out->buffer, tpm_data_out->size);

    /* swap iv's */
    memcpy(iv, tpm_iv_out, sizeof(*tpm_iv_out));

    rv = CKR_OK;

out:
    free(tpm_data_out);
    free(tpm_iv_out);

    return rv;
}

/*
 * These align with the specifications TPMI_YES_NO values as understood for encryptdecrypt routines.
 */
#define ENCRYPT 0
#define DECRYPT 1

CK_RV tpm_encrypt(tpm_encrypt_data *tpm_enc_data,
        CK_BYTE_PTR ptext, CK_ULONG ptextlen,
        CK_BYTE_PTR ctext, CK_ULONG_PTR ctextlen) {

    if (tpm_enc_data->is_rsa) {
        return tpm_rsa_encrypt(tpm_enc_data, ptext, ptextlen, ctext, ctextlen);
    }

    tpm_ctx *ctx = tpm_enc_data->ctx;
    TPMI_ALG_SYM_MODE mode = tpm_enc_data->sym.mode;
    TPM2B_IV *iv = &tpm_enc_data->sym.iv;

    twist auth = tpm_enc_data->auth;
    ESYS_TR handle = tpm_enc_data->handle;

    return encrypt_decrypt(ctx, handle, auth, mode, ENCRYPT,
            iv, ptext, ptextlen, ctext, ctextlen);
}

CK_RV tpm_decrypt(tpm_encrypt_data *tpm_enc_data,
        CK_BYTE_PTR ctext, CK_ULONG ctextlen,
        CK_BYTE_PTR ptext, CK_ULONG_PTR ptextlen) {

    if (tpm_enc_data->is_rsa) {
        return tpm_rsa_decrypt(tpm_enc_data, ctext, ctextlen, ptext, ptextlen);
    }

    tpm_ctx *ctx = tpm_enc_data->ctx;
    TPMI_ALG_SYM_MODE mode = tpm_enc_data->sym.mode;
    TPM2B_IV *iv = &tpm_enc_data->sym.iv;

    twist auth = tpm_enc_data->auth;
    ESYS_TR handle = tpm_enc_data->handle;

    return encrypt_decrypt(ctx, handle, auth, mode, DECRYPT,
            iv, ctext, ctextlen, ptext, ptextlen);
}

CK_RV tpm_changeauth(tpm_ctx *ctx, uint32_t parent_handle, uint32_t object_handle,
        twist oldauth, twist newauth,
        twist *newblob) {

    /* Set up the new auth value */
    TPM2B_AUTH new_tpm_auth;
    size_t newauthlen = twist_len(newauth);
    if (newauthlen > sizeof(new_tpm_auth.buffer)) {
        return CKR_PIN_LEN_RANGE;
    }

    new_tpm_auth.size = newauthlen;
    memcpy(new_tpm_auth.buffer, newauth, newauthlen);

    /* set the old auth value */
    bool result = set_esys_auth(ctx->esys_ctx, object_handle, oldauth);
    if (!result) {
        return CKR_GENERAL_ERROR;
    }

    /* call changeauth */
    TPM2B_PRIVATE *newprivate = NULL;
    TSS2_RC rval = Esys_ObjectChangeAuth(ctx->esys_ctx,
                        object_handle,
                        parent_handle,
                        ctx->hmac_session, ESYS_TR_NONE, ESYS_TR_NONE,
                        &new_tpm_auth, &newprivate);

    if (rval != TPM2_RC_SUCCESS) {
        LOGE("Esys_ObjectChangeAuth: 0x%x", rval);
        return CKR_GENERAL_ERROR;
    }

    uint8_t serialized[sizeof(*newprivate)];

    /* serialize the new blob private */
    size_t offset = 0;
    rval = Tss2_MU_TPM2B_PRIVATE_Marshal(newprivate, serialized, sizeof(*newprivate), &offset);
    if (rval != TSS2_RC_SUCCESS) {
        free(newprivate);
        LOGE("Tss2_MU_TPM2B_PRIVATE_Marshal: 0x%x", rval);
        return CKR_GENERAL_ERROR;
    }

    *newblob = twistbin_new(serialized, offset);
    free(newprivate);

    return *newblob ? CKR_OK : CKR_HOST_MEMORY;
}
/*
 * Esys_CreateLoaded(
    ESYS_CONTEXT *esysContext,
    ESYS_TR parentHandle,
    ESYS_TR shandle1,
    ESYS_TR shandle2,
    ESYS_TR shandle3,
    const TPM2B_SENSITIVE_CREATE *inSensitive,
    const TPM2B_TEMPLATE *inPublic,
    ESYS_TR *objectHandle,
    TPM2B_PRIVATE **outPrivate,
    TPM2B_PUBLIC **outPublic);
 */

CK_RV tpm2_create_seal_obj(tpm_ctx *ctx, twist parentauth, uint32_t parent_handle, twist objauth, twist oldpubblob, twist sealdata, twist *newpubblob, twist *newprivblob, uint32_t *handle) {

    CK_RV rv = CKR_GENERAL_ERROR;

    /*
     * clone the public portion from the existing object by unmarshaling (aka unserializing) it from the
     * pub blob and converting it to a TPM2B_TEMPLATE
     */
    TPM2B_PUBLIC pub = { .size = 0 };
    size_t len = twist_len(oldpubblob);

    size_t offset = 0;
    TSS2_RC rc = Tss2_MU_TPM2B_PUBLIC_Unmarshal((uint8_t *)oldpubblob, len, &offset, &pub);
    if (rc != TSS2_RC_SUCCESS) {
        LOGE("Tss2_MU_TPM2B_PUBLIC_Unmarshal: 0x%x", rc);
        return CKR_GENERAL_ERROR;
    }

    offset = 0;
    TPM2B_TEMPLATE template = { .size = 0 };
    rc = Tss2_MU_TPMT_PUBLIC_Marshal(&pub.publicArea, &template.buffer[0],
                                    sizeof(TPMT_PUBLIC), &offset);
    if (rc != TSS2_RC_SUCCESS) {
        LOGE("Tss2_MU_TPMT_PUBLIC_Marshal: 0x%x:", rc);
        return CKR_GENERAL_ERROR;
    }

    template.size = offset;

    /*
     * Set the seal data and auth value in the sensitive portion
     */
    TPM2B_SENSITIVE_CREATE sensitive = { .size = 0 };

    len = twist_len(sealdata);
    if (len > sizeof(sensitive.sensitive.data.buffer)) {
        LOGE("Seal data too big");
        return CKR_GENERAL_ERROR;
    }

    memcpy(sensitive.sensitive.data.buffer, sealdata, len);
    sensitive.sensitive.data.size = len;

    len = twist_len(objauth);
    if (len > sizeof(sensitive.sensitive.userAuth.buffer)) {
        LOGE("Auth value too big");
        return CKR_GENERAL_ERROR;
    }

    memcpy(sensitive.sensitive.userAuth.buffer, objauth, len);
    sensitive.sensitive.userAuth.size = len;

    /*
     * Set the parent object auth
     */
    bool res = set_esys_auth(ctx->esys_ctx, parent_handle, parentauth);
    if (!res) {
        return CKR_GENERAL_ERROR;
    }

    TPM2B_PRIVATE *newpriv = NULL;
    TPM2B_PUBLIC *newpub = NULL;
    rc = Esys_CreateLoaded(
            ctx->esys_ctx,
            parent_handle,
            ctx->hmac_session, ESYS_TR_NONE, ESYS_TR_NONE,
            &sensitive,
            &template,
            handle,
            &newpriv,
            &newpub
    );
    if (rc != TSS2_RC_SUCCESS) {
        LOGE("Esys_CreateLoaded: 0x%x:", rc);
        return CKR_GENERAL_ERROR;
    }

    uint8_t serialized[sizeof(*newpriv) > sizeof(*newpub) ? sizeof(*newpriv) : sizeof(*newpub)];

    /* serialize the new blob private */
    offset = 0;
    rc = Tss2_MU_TPM2B_PRIVATE_Marshal(newpriv, serialized, sizeof(*newpriv), &offset);
    if (rc != TSS2_RC_SUCCESS) {
        LOGE("Tss2_MU_TPM2B_PRIVATE_Marshal: 0x%x", rc);
        goto out;
    }

    *newprivblob = twistbin_new(serialized, offset);
    if (!*newprivblob) {
        goto out;
    }

    /* serialize the new blob public */
    offset = 0;
    rc = Tss2_MU_TPM2B_PUBLIC_Marshal(newpub, serialized, sizeof(*newpub), &offset);
    if (rc != TSS2_RC_SUCCESS) {
        twist_free(*newprivblob);
        *newprivblob = NULL;
        LOGE("Tss2_MU_TPM2B_PUBLIC_Marshal: 0x%x", rc);
        goto out;
    }

    *newpubblob = twistbin_new(serialized, offset);
    if (!*newpubblob) {
        twist_free(*newprivblob);
        *newprivblob = NULL;
        goto out;
    }

    rv = CKR_OK;

out:
    free(newpriv);
    free(newpub);

    return rv;
}

typedef struct tpm_key_data tpm_key_data;
struct tpm_key_data {
    TPM2B_PUBLIC pub;
    TPM2B_SENSITIVE_CREATE priv;
};

static CK_RV generic_bbool_check(CK_ATTRIBUTE_PTR attr, CK_BBOOL check) {

    CK_BBOOL value;
    CK_RV rv = attr_CK_BBOOL(attr, &value);
    if (rv != CKR_OK) {
        return rv;
    }

    if (value != check) {
        LOGE("Expected attr 0x%x to be %u, got %u", attr->type, value, check);
        return CKR_ATTRIBUTE_VALUE_INVALID;
    }

    return CKR_OK;
}

static CK_RV generic_bbool_true(CK_ATTRIBUTE_PTR attr, void *udata) {
    UNUSED(udata);

    return generic_bbool_check(attr, CK_TRUE);
}

static CK_RV generic_bbool_false(CK_ATTRIBUTE_PTR attr, void *udata) {
    UNUSED(udata);

    return generic_bbool_check(attr, CK_FALSE);
}

static CK_RV generic_bbool_any(CK_ATTRIBUTE_PTR attr, void *udata) {
    UNUSED(udata);

    CK_BBOOL value;
    CK_RV rv = attr_CK_BBOOL(attr, &value);
    if (rv != CKR_OK) {
        return rv;
    }

    if (value != CK_TRUE && value != CK_FALSE) {
        return CKR_ATTRIBUTE_VALUE_INVALID;
    }

    return CKR_OK;
}

static CK_RV handle_modulus(CK_ATTRIBUTE_PTR attr, void *udata) {

    tpm_key_data *keydat = (tpm_key_data *)udata;

    CK_ULONG value;
    CK_RV rv = attr_CK_ULONG(attr, &value);
    if (rv != CKR_OK) {
        return rv;
    }

    // TODO get known bit sizes from TPM on init and check
    if (value != 1024 && value != 2048) {
        return CKR_ATTRIBUTE_VALUE_INVALID;
    }

    keydat->pub.publicArea.parameters.rsaDetail.keyBits = value;

    return CKR_OK;
}

static CK_RV handle_ecparams(CK_ATTRIBUTE_PTR attr, void *udata) {

    tpm_key_data *keydat = (tpm_key_data *)udata;

    int nid = 0;
    CK_RV rv = ec_params_to_nid(attr, &nid);
    if (rv != CKR_OK) {
        return rv;
    }

    TPMS_ECC_PARMS *ec = &keydat->pub.publicArea.parameters.eccDetail;

    switch (nid) {
    case NID_X9_62_prime192v1:
        ec->curveID = TPM2_ECC_NIST_P192;
    break;
    case NID_secp224r1:
        ec->curveID = TPM2_ECC_NIST_P224;
    break;
    case NID_X9_62_prime256v1:
        ec->curveID = TPM2_ECC_NIST_P256;
    break;
    case NID_secp384r1:
        ec->curveID = TPM2_ECC_NIST_P384;
    break;
    case NID_secp521r1:
        ec->curveID = TPM2_ECC_NIST_P521;
    break;
    default:
        LOGE("Unsupported nid to tpm EC algorithm mapping, got nid: %d", nid);
        return CKR_CURVE_NOT_SUPPORTED;
    }

    return CKR_OK;
}

static CK_RV handle_encrypt(CK_ATTRIBUTE_PTR attr, void *udata) {

    tpm_key_data *keydat = (tpm_key_data *)udata;

    CK_BBOOL value;
    CK_RV rv = attr_CK_BBOOL(attr, &value);
    if (rv != CKR_OK) {
        return rv;
    }

    if (value == CK_TRUE) {
        keydat->pub.publicArea.objectAttributes |= TPMA_OBJECT_SIGN_ENCRYPT;
    } else {
        keydat->pub.publicArea.objectAttributes &= ~TPMA_OBJECT_SIGN_ENCRYPT;
    }

    return CKR_OK;
}

static CK_RV handle_decrypt(CK_ATTRIBUTE_PTR attr, void *udata) {

    tpm_key_data *keydat = (tpm_key_data *)udata;

    CK_BBOOL value;
    CK_RV rv = attr_CK_BBOOL(attr, &value);
    if (rv != CKR_OK) {
        return rv;
    }

    if (value == CK_TRUE) {
        keydat->pub.publicArea.objectAttributes |= TPMA_OBJECT_DECRYPT;
    } else {
        keydat->pub.publicArea.objectAttributes &= ~TPMA_OBJECT_DECRYPT;
    }

    return CKR_OK;
}

static CK_RV handle_exp(CK_ATTRIBUTE_PTR attr, void *udata) {

    tpm_key_data *keydat = (tpm_key_data *)udata;

    UINT32 *e = &keydat->pub.publicArea.parameters.rsaDetail.exponent;
    if (attr->ulValueLen > sizeof(*e)) {
        return CKR_ATTRIBUTE_VALUE_INVALID;
    }

    BIGNUM *bn = NULL;
    bn = BN_bin2bn(attr->pValue, attr->ulValueLen, NULL);
    if (!bn) {
        return CKR_HOST_MEMORY;
    }

    BN_ULONG value = BN_get_word(bn);

    *e = value;

    BN_free(bn);

    return CKR_OK;
}

static CK_RV handle_ckobject_class(CK_ATTRIBUTE_PTR attr, void *udata) {
    UNUSED(udata);

    CK_OBJECT_CLASS class[2] = {
        CKA_PRIVATE,    /* p11tool */
        CKO_PRIVATE_KEY /* pkcs11-tool */
    };

    if (attr->ulValueLen != sizeof(class[0])) {
        LOGE("Expected CK_OBJECT_CLASS length to be %zu got %lu", sizeof(class), attr->ulValueLen);
        return CKR_ATTRIBUTE_VALUE_INVALID;
    }

    CK_OBJECT_CLASS_PTR class_ptr = (CK_OBJECT_CLASS_PTR)attr->pValue;

    size_t i;
    bool found = false;
    for (i=0; i < ARRAY_LEN(class); i++) {
        if (*class_ptr == class[i]) {
            found = true;
            break;
        }
    }

    if (!found) {
        LOGE("Unexpected CK_OBJECT_CLASS got %lu", *class_ptr);
        return CKR_ATTRIBUTE_VALUE_INVALID;
    }

    return CKR_OK;
}

static CK_RV handle_extractable_common(CK_ATTRIBUTE_PTR attr, bool is_extractable, void *udata) {

    tpm_key_data *keydat = (tpm_key_data *)udata;

    CK_BBOOL value;
    CK_RV rv = attr_CK_BBOOL(attr, &value);
    if (rv != CKR_OK) {
        return rv;
    }

    if (value == is_extractable ? CK_TRUE : CK_FALSE) {
        keydat->pub.publicArea.objectAttributes &= ~(TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT);
    } else {
        keydat->pub.publicArea.objectAttributes |= (TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT);
    }

    return CKR_OK;
}

static CK_RV handle_extractable(CK_ATTRIBUTE_PTR attr, void *udata) {
    return handle_extractable_common(attr, true, udata);
}

static CK_RV handle_sensitive(CK_ATTRIBUTE_PTR attr, void *udata) {
    return handle_extractable_common(attr, false, udata);
}

static CK_RV handle_key_type(CK_ATTRIBUTE_PTR attr, void *udata) {

    tpm_key_data *keydat = (tpm_key_data *)udata;

    CK_ULONG value;
    CK_RV rv = attr_CK_ULONG(attr, &value);
    if (rv != CKR_OK) {
        return rv;
    }

    if ((value == CKK_RSA && keydat->pub.publicArea.type == TPM2_ALG_RSA) ||
        (value == CKK_EC && keydat->pub.publicArea.type == TPM2_ALG_ECC)) {
        return CKR_OK;
    }

    return CKR_ATTRIBUTE_VALUE_INVALID;
}

static TSS2_RC create_loaded(
        ESYS_CONTEXT *ectx,
        ESYS_TR parent,
        ESYS_TR session,
        TPM2B_SENSITIVE_CREATE *in_sens,
        TPM2B_PUBLIC *in_pub,

        ESYS_TR *out_handle,
        TPM2B_PUBLIC **out_pub,
        TPM2B_PRIVATE **out_priv
    ) {

    size_t offset = 0;
    TPM2B_TEMPLATE template = { .size = 0 };
    TSS2_RC rval = Tss2_MU_TPMT_PUBLIC_Marshal(&in_pub->publicArea, &template.buffer[0],
                                    sizeof(TPMT_PUBLIC), &offset);
    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Tss2_MU_TPMT_PUBLIC_Marshal: 0x%x", rval);
        return false;
    }

    template.size = offset;

    rval = Esys_CreateLoaded(
            ectx,
            parent,
            session, ESYS_TR_NONE, ESYS_TR_NONE,
            in_sens,
            &template,
            out_handle,
            out_priv,
            out_pub);
    if(rval != TPM2_RC_SUCCESS) {
        LOGE("Esys_CreateLoaded: 0x%x", rval);
    }

    return rval;
}

static CK_RV sanity_check_mech(CK_MECHANISM_PTR mechanism) {
    switch (mechanism->mechanism) {
        case CKM_RSA_PKCS_KEY_PAIR_GEN:
            /* falls-thru */
        case CKM_EC_KEY_PAIR_GEN:
            break;
        default:
            LOGE("Only supports mechanism \"CKM_RSA_PKCS_KEY_PAIR_GEN\" or"
                 "\"CKM_EC_KEY_PAIR_GEN\", got: 0x%lx", mechanism->mechanism);
            return CKR_MECHANISM_INVALID;
    }

    if (mechanism->ulParameterLen) {
        LOGE("Expected mechanism  with an empty parameter, got length: %lu",
                mechanism->ulParameterLen);
        return CKR_MECHANISM_PARAM_INVALID;
    }

    if (mechanism->pParameter) {
        LOGE("Expected mechanism with an empty parameter, got a parameter pointer");
        return CKR_MECHANISM_PARAM_INVALID;
    }

    return CKR_OK;
}

static const attr_handler tpm_handlers[] = {
    { CKA_TOKEN,           generic_bbool_true    },
    { CKA_PRIVATE,         generic_bbool_any     },
    { CKA_VERIFY,          handle_decrypt        }, // Verify is same as decrypt
    { CKA_ENCRYPT,         handle_encrypt        },
    { CKA_DECRYPT,         handle_decrypt        },
    { CKA_SIGN,            handle_encrypt        }, // SIGN_ENCRYPT are same in TPM, depends on SCHEME
    { CKA_MODULUS_BITS,    handle_modulus        },
    { CKA_PUBLIC_EXPONENT, handle_exp            },
    { CKA_SENSITIVE,       handle_sensitive      },
    { CKA_CLASS,           handle_ckobject_class },
    { CKA_EXTRACTABLE,     handle_extractable    },
    { CKA_EC_PARAMS,       handle_ecparams       },
    { CKA_KEY_TYPE,        handle_key_type       },
    { CKA_TRUSTED,           generic_bbool_false },
    { CKA_WRAP_WITH_TRUSTED, generic_bbool_false },
    { CKA_WRAP,              generic_bbool_false },
    { CKA_UNWRAP,            generic_bbool_false },
    { CKA_SIGN_RECOVER,      generic_bbool_false },
    { CKA_VERIFY_RECOVER,    generic_bbool_false },
    { CKA_DERIVE,            generic_bbool_false },
};

static CK_RV tpm_data_init(CK_MECHANISM_PTR mechanism,
        attr_list *pubattrs,
        attr_list *privattrs,
        tpm_key_data *tpmdat) {

    static const TPM2B_PUBLIC rsa_template = {
        .size = 0,
        .publicArea = {
            .type = TPM2_ALG_RSA,
            .nameAlg = TPM2_ALG_SHA256,
            .objectAttributes =
                    TPMA_OBJECT_FIXEDTPM
                  | TPMA_OBJECT_FIXEDPARENT
                  | TPMA_OBJECT_SENSITIVEDATAORIGIN
                  | TPMA_OBJECT_USERWITHAUTH
                  | TPMA_OBJECT_DECRYPT
                  | TPMA_OBJECT_SIGN_ENCRYPT,
            .authPolicy = {
                 .size = 0,
             },
            .parameters.rsaDetail = {
                 .symmetric = {
                     .algorithm = TPM2_ALG_NULL,
                 },
                 .scheme = {
                      .scheme = TPM2_ALG_NULL
                  },
                 .keyBits = 2048,
                 .exponent = 0,
             },
            .unique.rsa = {
                 .size = 0,
             },
        },
    };

    static const TPM2B_PUBLIC ecc_template = {
        .size = 0,
        .publicArea = {
            .type = TPM2_ALG_ECC,
            .nameAlg = TPM2_ALG_SHA256,
            .objectAttributes =
                TPMA_OBJECT_FIXEDTPM
                | TPMA_OBJECT_FIXEDPARENT
                | TPMA_OBJECT_SENSITIVEDATAORIGIN
                | TPMA_OBJECT_USERWITHAUTH
                | TPMA_OBJECT_SIGN_ENCRYPT,
            .authPolicy = {
                .size = 0,
            },
            .parameters.eccDetail = {
                .symmetric = {
                    .algorithm = TPM2_ALG_NULL,
                    .keyBits.aes = 0,
                    .mode.aes = 0,
                },
                .scheme = {
                    .scheme = TPM2_ALG_NULL,
                    .details = {
                       // {.hashAlg = TPM2_ALG_SHA1}
                    }
                },
                .curveID = TPM2_ECC_NIST_P256,
                .kdf = {.scheme =
                    TPM2_ALG_NULL,.details = {}
                }
            },
            .unique.ecc = {
                .x = {.size = 0,.buffer = {}},
                .y = {.size = 0,.buffer = {}}
            },
        },
    };

    memset(&tpmdat->priv, 0, sizeof(tpmdat->priv));
    memset(&tpmdat->pub,  0, sizeof(tpmdat->pub));

    switch(mechanism->mechanism) {
        case CKM_RSA_PKCS_KEY_PAIR_GEN:
            tpmdat->pub = rsa_template;
            break;
        case CKM_EC_KEY_PAIR_GEN:
            tpmdat->pub = ecc_template;
            break;
        default:
            /* should never happen checked at entry */
            LOGE("Unsupported keypair mechanism: 0x%lx",
                    mechanism->mechanism);
            assert(0);
            return CKR_MECHANISM_INVALID;
    }


    CK_RV rv = attr_list_invoke_handlers(pubattrs,
            tpm_handlers, ARRAY_LEN(tpm_handlers), tpmdat);
    if (rv != CKR_OK) {
        return rv;
    }

    rv = attr_list_invoke_handlers(privattrs,
            tpm_handlers, ARRAY_LEN(tpm_handlers), tpmdat);
    if (rv != CKR_OK) {
        return rv;
    }

    return CKR_OK;
}

static CK_RV uint32_to_BN(uint32_t value, void **bytes, CK_ULONG_PTR len) {

    CK_RV rv = CKR_GENERAL_ERROR;

    BIGNUM *b = BN_new();
    if (!b) {
        LOGE("oom");
        return CKR_HOST_MEMORY;
    }

    int rc = BN_set_word(b, value);
    if (!rc) {
        LOGE("BN_set_word failed: %d", rc);
        goto out;
    }

    int l = BN_num_bytes(b);
    if (!l) {
        LOGE("Expected bignum to not be 0");
        return CKR_GENERAL_ERROR;
    }

    void *x = malloc(l);
    if (!x) {
        LOGE("oom");
        rv = CKR_HOST_MEMORY;
        goto out;
    }

    rc = BN_bn2bin(b, x);
    if (!rc) {
        free(x);
        LOGE("BN_bn2bin failed: %d", rc);
        goto out;
    }

    *bytes = x;
    *len = l;

    rv = CKR_OK;

out:
    BN_free(b);
    return rv;
}

static CK_RV tpm_object_data_populate_rsa(TPM2B_PUBLIC *out_pub, tpm_object_data *objdata) {
    assert(out_pub);
    assert(objdata);

    /* MODULUS */
    bool r = attr_list_add_buf(objdata->attrs, CKA_MODULUS,
            out_pub->publicArea.unique.rsa.buffer, out_pub->publicArea.unique.rsa.size);
    if (!r) {
        return CKR_GENERAL_ERROR;
    }

    UINT32 exp = out_pub->publicArea.parameters.rsaDetail.exponent;
    if (!exp) {
        exp = 65537;
    }

    void *buf = NULL;
    CK_ULONG len = 0;
    CK_RV rv = uint32_to_BN(exp, &buf, &len);
    if (rv != CKR_OK) {
        return rv;
    }

    r = attr_list_add_buf(objdata->attrs, CKA_PUBLIC_EXPONENT, buf, len);
    free(buf);
    if (!r) {
        return CKR_GENERAL_ERROR;
    }

    return CKR_OK;
}

static EC_POINT *tpm_pub_to_ossl_pub(EC_GROUP *group, TPM2B_PUBLIC *key) {

    BIGNUM *bn_x = NULL;
    BIGNUM *bn_y = NULL;

    EC_POINT *pub_key_point_tmp = NULL;
    EC_POINT *r = NULL;

    /* Create the big numbers for the coordinates of the point */
    bn_x = BN_bin2bn(&key->publicArea.unique.ecc.x.buffer[0],
                               key->publicArea.unique.ecc.x.size,
                               NULL);
    if (!bn_x) {
        LOGE("Create big num from byte buffer.");
        goto out;
    }

    bn_y = BN_bin2bn(&key->publicArea.unique.ecc.y.buffer[0],
                               key->publicArea.unique.ecc.y.size,
                               NULL);
    if (!bn_y) {
        LOGE("Create big num from byte buffer.");
        goto out;
    }

    /* Create the ec point with the affine coordinates of the TPM point */
    pub_key_point_tmp = EC_POINT_new(group);
    if (!pub_key_point_tmp) {
        LOGE("Could not create new affine point from X and Y coordinates");
        goto out;
    }

    int rc = EC_POINT_set_affine_coordinates_GFp(group,
            pub_key_point_tmp,
            bn_x,
            bn_y,
            NULL);
    if (!rc) {
        EC_POINT_free(pub_key_point_tmp);
        LOGE("Could not set affine coordinate points");
        goto out;
    }

    /* sanity check that the point created in the group is on the curve */
    rc = EC_POINT_is_on_curve(group, pub_key_point_tmp, NULL);
    if (!rc) {
        EC_POINT_free(pub_key_point_tmp);
        LOGE("The TPM point is not on the curve");
        goto out;
    }

    r = pub_key_point_tmp;

out:
    BN_free(bn_x);
    BN_free(bn_y);

    return r;
}

static CK_RV tpm_object_data_populate_ecc(TPM2B_PUBLIC *out_pub, tpm_object_data *objdata) {

    CK_RV rv = CKR_GENERAL_ERROR;

    EC_GROUP *group = NULL;               /* Group defines the used curve */
    EC_POINT *tpm_pub_key = NULL;         /* Public part of TPM key */
    int curveId;
    unsigned char *mydata = NULL;

    /* Set ossl constant for curve type and create group for curve */
    switch (out_pub->publicArea.parameters.eccDetail.curveID) {
        case TPM2_ECC_NIST_P192:
            curveId = NID_X9_62_prime192v1;
            break;
        case TPM2_ECC_NIST_P224:
            curveId = NID_secp224r1;
            break;
        case TPM2_ECC_NIST_P256:
            curveId = NID_X9_62_prime256v1;
            break;
        case TPM2_ECC_NIST_P384:
            curveId = NID_secp384r1;
            break;
        case TPM2_ECC_NIST_P521:
            curveId = NID_secp521r1;
            break;
        default:
            LOGE("ECC Curve not implemented");
            return CKR_GENERAL_ERROR;
    }

    group = EC_GROUP_new_by_curve_name(curveId);
    if (!group) {
        LOGE("EC_GROUP_new failed");
        goto out;
    }

    tpm_pub_key = tpm_pub_to_ossl_pub(group, out_pub);
    if (!tpm_pub_key){
        goto out;
    }

    ssize_t len = EC_POINT_point2buf(group, tpm_pub_key, POINT_CONVERSION_UNCOMPRESSED,
            &mydata, NULL);
    if (len <= 0) {
        LOGE("EC_POINT_point2buf failed: %zd", len);
        goto out;
    }

    if (len > 255) {
        LOGE("Length must fit within a byte, got %zd", len);
        goto out;
    }

    /*
     * Build a DER encoded uncompressed representation of the points
     * per X9.62.
     *
     * TODO get better link to documentation around this or build
     * with OSSL.
     */
    char *padded_data = malloc(len + 2);
    if (!padded_data) {
        LOGE("oom");
        goto out;
    }

    padded_data[0] = 4;
    padded_data[1] = len;
    memcpy(&padded_data[2], mydata, len);

    bool r = attr_list_add_buf(objdata->attrs, CKA_EC_POINT,
            (CK_BYTE_PTR)padded_data, len + 2);
    free(padded_data);
    if (!r) {
        goto out;
    }

    rv = CKR_OK;

out:
    EC_POINT_free(tpm_pub_key);
    EC_GROUP_free(group);
    OPENSSL_free(mydata);

    return rv;
}

CK_RV tpm2_generate_key(
        tpm_ctx *tpm,

        uint32_t parent,
        twist parentauth,

        twist newauthbin,

        CK_MECHANISM_PTR mechanism,

        attr_list *pubattrs,

        attr_list *privattrs,

        tpm_object_data *objdata) {

    twist tmppub = NULL;
    twist tmppriv = NULL;

    TPM2B_PUBLIC *out_pub = NULL;
    TPM2B_PRIVATE *out_priv = NULL;

    ESYS_TR out_handle = 0;

    CK_RV rv = CKR_GENERAL_ERROR;

    assert(objdata);

    rv = sanity_check_mech(mechanism);
    if (rv != CKR_OK) {
        goto error;
    }

    tpm_key_data tpmdat;
    rv = tpm_data_init(mechanism,
        pubattrs,
        privattrs,
        &tpmdat);
    if (rv != CKR_OK) {
        goto error;
    }

    bool res = set_esys_auth(tpm->esys_ctx, parent, parentauth);
    if (!res) {
        rv = CKR_GENERAL_ERROR;
        goto error;
    }

    /*
     * Guaranteed to fit but throw an assert in just in case
     * utils_setup_new_object_auth() changes.
     */
    TPM2B_AUTH *auth = &tpmdat.priv.sensitive.userAuth;
    size_t len = twist_len(newauthbin);
    assert(len < sizeof(auth->buffer));
    auth->size = len;
    memcpy(auth->buffer, newauthbin, auth->size);

    TSS2_RC rc = create_loaded(
            tpm->esys_ctx,
            parent,
            tpm->hmac_session,
            &tpmdat.priv,
            &tpmdat.pub,

            &out_handle,
            &out_pub,
            &out_priv
        );
    if (rc != TSS2_RC_SUCCESS) {
        rv = CKR_GENERAL_ERROR;
        LOGE("create_loaded 0x%x", rc);
        goto error;
    }

    assert(out_pub);
    assert(out_priv);

    /* load the public only object portion */
    res = tpm_loadexternal(tpm, out_pub, &objdata->pubhandle);
    if (!res) {
        rv = CKR_GENERAL_ERROR;
        goto error;
    }

    /* serialize the tpm public private object portions */
    BYTE pubb[sizeof(*out_pub)];
    size_t pubb_size = 0;

    BYTE privb[sizeof(*out_priv)];
    size_t privb_size = 0;

    size_t offset = 0;
    rc = Tss2_MU_TPM2B_PUBLIC_Marshal(out_pub, pubb, sizeof(pubb), &offset);
    if (rc != TSS2_RC_SUCCESS) {
        LOGE("Tss2_MU_TPM2B_PUBLIC_Marshal: 0x%x", rc);
        rv = CKR_GENERAL_ERROR;
        goto error;
    }

    pubb_size = offset;

    offset = 0;
    rc = Tss2_MU_TPM2B_PRIVATE_Marshal(out_priv, privb, sizeof(privb), &offset);
    if (rc != TSS2_RC_SUCCESS) {
        LOGE("Tss2_MU_TPM2B_PRIVATE_Marshal: 0x%x", rc);
        rv = CKR_GENERAL_ERROR;
        goto error;
    }

    privb_size = offset;

    tmppub = twistbin_new(pubb, pubb_size);
    if (!tmppub) {
        LOGE("oom");
        rv = CKR_HOST_MEMORY;
        goto error;
    }

    tmppriv = twistbin_new(privb, privb_size);
    if (!tmppriv) {
        twist_free(tmppub);
        LOGE("oom");
        rv = CKR_HOST_MEMORY;
        goto error;
    }

    objdata->attrs = attr_list_new();
    if (!objdata->attrs) {
        twist_free(tmppub);
        twist_free(tmppriv);
        LOGE("oom");
        rv = CKR_HOST_MEMORY;
        goto error;
    }

    switch(mechanism->mechanism) {
    case CKM_RSA_PKCS_KEY_PAIR_GEN:
        rv = tpm_object_data_populate_rsa(out_pub, objdata);
        break;
    case CKM_EC_KEY_PAIR_GEN:
        rv = tpm_object_data_populate_ecc(out_pub, objdata);
        break;
    default:
        LOGE("Impossible keygen type, got: 0x%lx", mechanism->mechanism);
        rv = CKR_MECHANISM_INVALID;
        assert(rv == CKR_OK);
        goto error;
    }

    if (rv != CKR_OK) {
        goto error;
    }

    /* everything common*/
    TPMA_OBJECT objattrs = out_pub->publicArea.objectAttributes;

    CK_BBOOL extractable = !!!(objattrs & (TPMA_OBJECT_FIXEDTPM|TPMA_OBJECT_FIXEDPARENT));
    bool r = attr_list_add_bool(objdata->attrs, CKA_EXTRACTABLE, extractable);
    goto_error_false(r);


    CK_BBOOL sensitive = !extractable;
    r = attr_list_add_bool(objdata->attrs, CKA_ALWAYS_SENSITIVE, sensitive);
    goto_error_false(r);


    CK_BBOOL never_extractable = !extractable;
    r = attr_list_add_bool(objdata->attrs, CKA_NEVER_EXTRACTABLE, never_extractable);
    goto_error_false(r);

    CK_BBOOL local = !!(objattrs & TPMA_OBJECT_SENSITIVEDATAORIGIN);
    r = attr_list_add_bool(objdata->attrs, CKA_LOCAL, local);
    goto_error_false(r);

    /* conditional block */
    CK_BBOOL decrypt = !!(objattrs & TPMA_OBJECT_DECRYPT);
    r = attr_list_add_bool(objdata->attrs, CKA_DECRYPT, decrypt);
    goto_error_false(r);

    /* decrypt and verify are the same */
    r = attr_list_add_bool(objdata->attrs, CKA_VERIFY, decrypt);
    goto_error_false(r);

    CK_BBOOL sign = !!(objattrs & TPMA_OBJECT_SIGN_ENCRYPT);
    r = attr_list_add_bool(objdata->attrs, CKA_SIGN, sign);
    goto_error_false(r);

    /* sign and encrypt are same */
    r = attr_list_add_bool(objdata->attrs, CKA_ENCRYPT, sign);
    goto_error_false(r);

    objdata->privblob = tmppriv;
    objdata->pubblob = tmppub;
    objdata->privhandle = out_handle;

    rv = CKR_OK;
error:

    Esys_Free(out_pub);
    Esys_Free(out_priv);

    if (rv != CKR_OK) {
        tpm_objdata_free(objdata);
    }

    return rv;
}

void tpm_objdata_free(tpm_object_data *objdata) {

    if (!objdata) {
        return;
    }

    attr_list_free(objdata->attrs);

    twist_free(objdata->privblob);
    twist_free(objdata->pubblob);
}

CK_RV tpm_get_algorithms (tpm_ctx *ctx, TPMS_CAPABILITY_DATA **capabilityData) {

    TPM2_CAP capability = TPM2_CAP_ALGS;
    UINT32 property = TPM2_ALG_FIRST;
    UINT32 propertyCount = TPM2_MAX_CAP_ALGS;
    TPMI_YES_NO moreData;

    check_pointer(ctx);
    check_pointer(capabilityData);

    TSS2_RC rval = Esys_GetCapability(ctx->esys_ctx,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            ESYS_TR_NONE,
            capability,
            property, propertyCount, &moreData, capabilityData);

    if (rval != TSS2_RC_SUCCESS) {
        LOGE("Esys_GetCapability: 0x%x:", rval);
        return CKR_GENERAL_ERROR;
    }

    if (!capabilityData) {
        LOGE("TPM did not reply with correct amount of capabilities");
        return CKR_GENERAL_ERROR;
    }

    return CKR_OK;
}

static CK_BBOOL is_algorithm_supported(TPMU_CAPABILITIES *capabilities, TPM2_ALG_ID algorithm){
    for (unsigned int i = 0 ; i < capabilities->algorithms.count ; i++){
        if (capabilities->algorithms.algProperties[i].alg == algorithm){
            return CK_TRUE;
        }
    }
    return CK_FALSE;
}

#define add_mech(mech) \
    if (mechanism_list) { /* Only update if not called for size*/ \
        if (*count <= supported) { \
            rv = CKR_BUFFER_TOO_SMALL; \
            goto out; \
        } \
        mechanism_list[supported] = mech; \
    } \
    supported++;

CK_RV tpm2_getmechanisms(tpm_ctx *ctx, CK_MECHANISM_TYPE *mechanism_list, CK_ULONG_PTR count){
    check_pointer(count);
    check_pointer(ctx);
    CK_ULONG supported = 0;
    CK_RV rv;

    TPMS_CAPABILITY_DATA *capabilityData = NULL;
    rv = tpm_get_algorithms (ctx, &capabilityData);
    if (rv != CKR_OK) {
        LOGE("Retrieving supported algorithms from TPM failed");
        return rv;
    }
    TPMU_CAPABILITIES *algs= &capabilityData->data;

    if (is_algorithm_supported(algs, TPM2_ALG_RSA)) {
        add_mech(CKM_RSA_PKCS);
        add_mech(CKM_RSA_PKCS_KEY_PAIR_GEN);
        add_mech(CKM_RSA_X_509);
        if (is_algorithm_supported(algs, TPM2_ALG_SHA1)) {
           add_mech(CKM_SHA1_RSA_PKCS);
        }
        if (is_algorithm_supported(algs, TPM2_ALG_SHA256)) {
           add_mech(CKM_SHA256_RSA_PKCS);
        }
        if (is_algorithm_supported(algs, TPM2_ALG_SHA384)) {
           add_mech(CKM_SHA384_RSA_PKCS);
        }
        if (is_algorithm_supported(algs, TPM2_ALG_SHA512)) {
           add_mech(CKM_SHA512_RSA_PKCS);
        }
    }

    if (is_algorithm_supported(algs, TPM2_ALG_OAEP)) {
        add_mech(CKM_RSA_PKCS_OAEP);
    }
    if (is_algorithm_supported(algs, TPM2_ALG_CBC)) {
        add_mech(CKM_AES_CBC);
    }
    if (is_algorithm_supported(algs, TPM2_ALG_CFB)) {
        add_mech(CKM_AES_CFB1);
    }
    if (is_algorithm_supported(algs, TPM2_ALG_ECB)) {
        add_mech(CKM_AES_CFB1);
    }
    if (is_algorithm_supported(algs, TPM2_ALG_ECDSA)) {
        add_mech(CKM_ECDSA);
        add_mech(CKM_ECDSA_SHA1);
    }
    if (is_algorithm_supported(algs, TPM2_ALG_ECC)) {
        add_mech(CKM_EC_KEY_PAIR_GEN);
    }
    if (is_algorithm_supported(algs, TPM2_ALG_SHA1)) {
        add_mech(CKM_SHA_1);
    }
    if (is_algorithm_supported(algs, TPM2_ALG_SHA256)) {
        add_mech(CKM_SHA256);
    }

out:
    *count = supported;
    free(capabilityData);

    return rv;

}

void tpm_init(void) {
    /* nothing to do */
}

void tpm_destroy(void) {
    Esys_Free(tpms_fixed_property_cache);
    Esys_Free(tpms_alg_cache);
}
