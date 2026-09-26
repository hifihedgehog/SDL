/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* Facts from zwiftplay and Makinolo's 2023 post, read for facts only (see
 * hifihedgehog/SDL#33 Part 12, "Encrypted handshake on older firmware"):
 * - Each side has a P-256 key pair. The host sends its public key as X then
 *   Y, 32 bytes each, big-endian, always 64 bytes (zwiftplay
 *   LocalKeyProvider.cs:39-47).
 * - The shared secret is the X coordinate of the ECDH point, all 32 bytes
 *   (EncryptionUtils.kt:47-52).
 * - HKDF with SHA-256 derives 36 bytes from the secret, salted with the
 *   device's key then the host's key, with empty info (EncryptionUtils.kt:54-63,
 *   ZapCrypto.kt:61-66). The AES-256 key is bytes 0 to 31 and the nonce
 *   prefix bytes 32 to 35 (ZapCrypto.kt:25-29).
 * - A frame is 4 counter bytes, the ciphertext and a 4-byte tag. AES-CCM
 *   opens it with a 4-byte tag, the nonce prefix and the counter bytes as
 *   received for the nonce, and no associated data (ZapCrypto.kt:43-59).
 *
 * The cryptography is Windows' CNG in bcrypt.dll, loaded at run time from
 * System32 as SDL's notification code loads it, so SDL's import table does
 * not change. The one computation done here checks that the device's key is
 * a point on the curve. The pages cited by title are on Microsoft Learn. */

#ifdef _WIN32

#include "SDL_ble_zwift_crypto.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#define ZWIFT_KEY_BITS    256 /* "BCryptGenerateKeyPair function (bcrypt.h)": ECDH P-256 keys are 256 bits */
#define ZWIFT_FIELD_SIZE  32  /* Bytes in a coordinate, a private scalar and the shared secret */
#define ZWIFT_SALT_SIZE   (2 * SDL_ZWIFT_KEY_SIZE)
#define ZWIFT_SHA256_SIZE 32
#define ZWIFT_AES_SIZE    32
#define ZWIFT_PREFIX_SIZE (SDL_ZWIFT_HKDF_SIZE - ZWIFT_AES_SIZE)
#define ZWIFT_COUNTER_SIZE 4
#define ZWIFT_TAG_SIZE    4
#define ZWIFT_NONCE_SIZE  (ZWIFT_PREFIX_SIZE + ZWIFT_COUNTER_SIZE)
/* The longest attribute value, 512 bytes (Bluetooth Core, Vol 3, Part F,
   3.2.9 "Long attribute values") */
#define ZWIFT_MAX_FRAME   512

#define ZWIFT_PUBLIC_BLOB_SIZE  (sizeof(BCRYPT_ECCKEY_BLOB) + 2 * ZWIFT_FIELD_SIZE)
#define ZWIFT_PRIVATE_BLOB_SIZE (sizeof(BCRYPT_ECCKEY_BLOB) + 3 * ZWIFT_FIELD_SIZE)

/* The functions of bcrypt.dll, declared as bcrypt.h declares them */
typedef NTSTATUS(WINAPI *ZwiftCrypto_OpenAlgorithmProviderFunc)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_CloseAlgorithmProviderFunc)(BCRYPT_ALG_HANDLE, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_SetPropertyFunc)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_GetPropertyFunc)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG *, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_GenerateKeyPairFunc)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE *, ULONG, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_FinalizeKeyPairFunc)(BCRYPT_KEY_HANDLE, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_ExportKeyFunc)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG *, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_ImportKeyPairFunc)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, BCRYPT_KEY_HANDLE *, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_DestroyKeyFunc)(BCRYPT_KEY_HANDLE);
typedef NTSTATUS(WINAPI *ZwiftCrypto_SecretAgreementFunc)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE, BCRYPT_SECRET_HANDLE *, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_DeriveKeyFunc)(BCRYPT_SECRET_HANDLE, LPCWSTR, BCryptBufferDesc *, PUCHAR, ULONG, ULONG *, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_DestroySecretFunc)(BCRYPT_SECRET_HANDLE);
typedef NTSTATUS(WINAPI *ZwiftCrypto_CreateHashFunc)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_HashDataFunc)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_FinishHashFunc)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_DestroyHashFunc)(BCRYPT_HASH_HANDLE);
typedef NTSTATUS(WINAPI *ZwiftCrypto_GenerateSymmetricKeyFunc)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *ZwiftCrypto_DecryptFunc)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG *, ULONG);

struct SDL_ZwiftKeys
{
    HMODULE library;
    ZwiftCrypto_OpenAlgorithmProviderFunc OpenAlgorithmProvider;
    ZwiftCrypto_CloseAlgorithmProviderFunc CloseAlgorithmProvider;
    ZwiftCrypto_SetPropertyFunc SetProperty;
    ZwiftCrypto_GetPropertyFunc GetProperty;
    ZwiftCrypto_GenerateKeyPairFunc GenerateKeyPair;
    ZwiftCrypto_FinalizeKeyPairFunc FinalizeKeyPair;
    ZwiftCrypto_ExportKeyFunc ExportKey;
    ZwiftCrypto_ImportKeyPairFunc ImportKeyPair;
    ZwiftCrypto_DestroyKeyFunc DestroyKey;
    ZwiftCrypto_SecretAgreementFunc SecretAgreement;
    ZwiftCrypto_DeriveKeyFunc DeriveKey;
    ZwiftCrypto_DestroySecretFunc DestroySecret;
    ZwiftCrypto_CreateHashFunc CreateHash;
    ZwiftCrypto_HashDataFunc HashData;
    ZwiftCrypto_FinishHashFunc FinishHash;
    ZwiftCrypto_DestroyHashFunc DestroyHash;
    ZwiftCrypto_GenerateSymmetricKeyFunc GenerateSymmetricKey;
    ZwiftCrypto_DecryptFunc Decrypt;

    BCRYPT_ALG_HANDLE ecdh;
    BCRYPT_ALG_HANDLE hmac;
    BCRYPT_ALG_HANDLE aes;         /* In CCM mode */
    BCRYPT_KEY_HANDLE pair;        /* The host's key pair, NULL before MakeKey or SetKeyPair */
    bool fixed;                    /* SetKeyPair's pair stands in for the one MakeKey would make */
    uint8_t public_key[SDL_ZWIFT_KEY_SIZE]; /* The pair's public key as sent, X then Y */
    BCRYPT_KEY_HANDLE session;     /* The AES key of the last Derive, NULL until one succeeds */
    uint8_t hkdf[SDL_ZWIFT_HKDF_SIZE];      /* That Derive's HKDF output */
};

/* secp256r1, y^2 = x^3 + ax + b over the integers modulo p, with the values
   SEC 2 v2 prints in section 2.4.2 "Recommended Parameters secp256r1" */
static const uint8_t zwift_p256_p[ZWIFT_FIELD_SIZE] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t zwift_p256_a[ZWIFT_FIELD_SIZE] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFC
};
static const uint8_t zwift_p256_b[ZWIFT_FIELD_SIZE] = {
    0x5A, 0xC6, 0x35, 0xD8, 0xAA, 0x3A, 0x93, 0xE7, 0xB3, 0xEB, 0xBD, 0x55, 0x76, 0x98, 0x86, 0xBC,
    0x65, 0x1D, 0x06, 0xB0, 0xCC, 0x53, 0xB0, 0xF6, 0x3B, 0xCE, 0x3C, 0x3E, 0x27, 0xD2, 0x60, 0x4B
};

#define ZWIFT_LIMBS (ZWIFT_FIELD_SIZE / 4)

/* 32 big-endian bytes as eight 32-bit limbs, the least significant first */
static void ZwiftCrypto_Limbs(uint32_t *limbs, const uint8_t *bytes)
{
    int i;

    for (i = 0; i < ZWIFT_LIMBS; ++i) {
        const uint8_t *word = &bytes[(ZWIFT_LIMBS - 1 - i) * 4];

        limbs[i] = ((uint32_t)word[0] << 24) | ((uint32_t)word[1] << 16) | ((uint32_t)word[2] << 8) | word[3];
    }
}

/* -1, 0 or 1 as a is below, equal to or above b */
static int ZwiftCrypto_Compare(const uint32_t *a, const uint32_t *b)
{
    int i;

    for (i = ZWIFT_LIMBS - 1; i >= 0; --i) {
        if (a[i] != b[i]) {
            return (a[i] < b[i]) ? -1 : 1;
        }
    }
    return 0;
}

/* result = a + b mod p, for a and b below p. result may be a or b. */
static void ZwiftCrypto_AddMod(uint32_t *result, const uint32_t *a, const uint32_t *b, const uint32_t *p)
{
    uint32_t sum[ZWIFT_LIMBS];
    uint64_t carry = 0, borrow = 0;
    int i;

    for (i = 0; i < ZWIFT_LIMBS; ++i) {
        carry += (uint64_t)a[i] + b[i];
        sum[i] = (uint32_t)carry;
        carry >>= 32;
    }
    /* The sum is below 2p, so one subtraction of p brings it below p. The
       borrow out of the top limb then cancels the carry. */
    if (carry || ZwiftCrypto_Compare(sum, p) >= 0) {
        for (i = 0; i < ZWIFT_LIMBS; ++i) {
            const uint64_t difference = (uint64_t)sum[i] - p[i] - borrow;

            sum[i] = (uint32_t)difference;
            borrow = (difference >> 32) & 1;
        }
    }
    memcpy(result, sum, sizeof(sum));
}

/* result = a * b mod p, for a and b below p, by doubling and adding over the
   bits of b. result may be a or b. The inputs are public, so the time this
   takes may depend on them. */
static void ZwiftCrypto_MulMod(uint32_t *result, const uint32_t *a, const uint32_t *b, const uint32_t *p)
{
    uint32_t product[ZWIFT_LIMBS];
    int bit;

    memset(product, 0, sizeof(product));
    for (bit = ZWIFT_LIMBS * 32 - 1; bit >= 0; --bit) {
        ZwiftCrypto_AddMod(product, product, product, p);
        if ((b[bit / 32] >> (bit % 32)) & 1) {
            ZwiftCrypto_AddMod(product, product, a, p);
        }
    }
    memcpy(result, product, sizeof(product));
}

/* NIST SP 800-56A Rev. 3, 5.6.2.3.4 "ECC Partial Public-Key Validation
   Routine": both coordinates below p and the point on the curve. The point at
   infinity has no X and Y to send. P-256 has cofactor 1, so every other point
   on the curve has order n and the order check of the full routine
   (5.6.2.3.3) adds nothing. */
static bool ZwiftCrypto_OnCurve(const uint8_t *key)
{
    uint32_t p[ZWIFT_LIMBS], a[ZWIFT_LIMBS], b[ZWIFT_LIMBS], x[ZWIFT_LIMBS], y[ZWIFT_LIMBS];
    uint32_t left[ZWIFT_LIMBS], right[ZWIFT_LIMBS];

    ZwiftCrypto_Limbs(p, zwift_p256_p);
    ZwiftCrypto_Limbs(a, zwift_p256_a);
    ZwiftCrypto_Limbs(b, zwift_p256_b);
    ZwiftCrypto_Limbs(x, key);
    ZwiftCrypto_Limbs(y, &key[ZWIFT_FIELD_SIZE]);
    if (ZwiftCrypto_Compare(x, p) >= 0 || ZwiftCrypto_Compare(y, p) >= 0) {
        return false;
    }
    ZwiftCrypto_MulMod(left, y, y, p);
    ZwiftCrypto_MulMod(right, x, x, p);
    ZwiftCrypto_AddMod(right, right, a, p);
    ZwiftCrypto_MulMod(right, right, x, p);
    ZwiftCrypto_AddMod(right, right, b, p);
    return ZwiftCrypto_Compare(left, right) == 0;
}

/* bcrypt.dll from System32 only, as SDL's notification code loads it
   (SDL_windowsnotification.c, WIN_BCryptGenRandom), so no copy beside the
   application is picked up. "LoadLibraryExW function (libloaderapi.h)",
   "GetProcAddress function (libloaderapi.h)" */
static bool ZwiftCrypto_Load(SDL_ZwiftKeys *keys)
{
    HMODULE library = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!library) {
        return false;
    }
    keys->library = library;
    keys->OpenAlgorithmProvider = (ZwiftCrypto_OpenAlgorithmProviderFunc)GetProcAddress(library, "BCryptOpenAlgorithmProvider");
    keys->CloseAlgorithmProvider = (ZwiftCrypto_CloseAlgorithmProviderFunc)GetProcAddress(library, "BCryptCloseAlgorithmProvider");
    keys->SetProperty = (ZwiftCrypto_SetPropertyFunc)GetProcAddress(library, "BCryptSetProperty");
    keys->GetProperty = (ZwiftCrypto_GetPropertyFunc)GetProcAddress(library, "BCryptGetProperty");
    keys->GenerateKeyPair = (ZwiftCrypto_GenerateKeyPairFunc)GetProcAddress(library, "BCryptGenerateKeyPair");
    keys->FinalizeKeyPair = (ZwiftCrypto_FinalizeKeyPairFunc)GetProcAddress(library, "BCryptFinalizeKeyPair");
    keys->ExportKey = (ZwiftCrypto_ExportKeyFunc)GetProcAddress(library, "BCryptExportKey");
    keys->ImportKeyPair = (ZwiftCrypto_ImportKeyPairFunc)GetProcAddress(library, "BCryptImportKeyPair");
    keys->DestroyKey = (ZwiftCrypto_DestroyKeyFunc)GetProcAddress(library, "BCryptDestroyKey");
    keys->SecretAgreement = (ZwiftCrypto_SecretAgreementFunc)GetProcAddress(library, "BCryptSecretAgreement");
    keys->DeriveKey = (ZwiftCrypto_DeriveKeyFunc)GetProcAddress(library, "BCryptDeriveKey");
    keys->DestroySecret = (ZwiftCrypto_DestroySecretFunc)GetProcAddress(library, "BCryptDestroySecret");
    keys->CreateHash = (ZwiftCrypto_CreateHashFunc)GetProcAddress(library, "BCryptCreateHash");
    keys->HashData = (ZwiftCrypto_HashDataFunc)GetProcAddress(library, "BCryptHashData");
    keys->FinishHash = (ZwiftCrypto_FinishHashFunc)GetProcAddress(library, "BCryptFinishHash");
    keys->DestroyHash = (ZwiftCrypto_DestroyHashFunc)GetProcAddress(library, "BCryptDestroyHash");
    keys->GenerateSymmetricKey = (ZwiftCrypto_GenerateSymmetricKeyFunc)GetProcAddress(library, "BCryptGenerateSymmetricKey");
    keys->Decrypt = (ZwiftCrypto_DecryptFunc)GetProcAddress(library, "BCryptDecrypt");
    return keys->OpenAlgorithmProvider && keys->CloseAlgorithmProvider && keys->SetProperty && keys->GetProperty &&
           keys->GenerateKeyPair && keys->FinalizeKeyPair && keys->ExportKey && keys->ImportKeyPair &&
           keys->DestroyKey && keys->SecretAgreement && keys->DeriveKey && keys->DestroySecret &&
           keys->CreateHash && keys->HashData && keys->FinishHash && keys->DestroyHash &&
           keys->GenerateSymmetricKey && keys->Decrypt;
}

/* "BCryptOpenAlgorithmProvider function (bcrypt.h)": the default provider of
   the algorithm. The handle is kept only when the call succeeds. */
static bool ZwiftCrypto_Open(SDL_ZwiftKeys *keys, BCRYPT_ALG_HANDLE *algorithm, LPCWSTR id, ULONG flags)
{
    BCRYPT_ALG_HANDLE handle = NULL;

    if (!BCRYPT_SUCCESS(keys->OpenAlgorithmProvider(&handle, id, NULL, flags))) {
        return false;
    }
    *algorithm = handle;
    return true;
}

/* True when CCM takes a 4-byte tag. "BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO
   (bcrypt.h)" names the BCRYPT_AUTH_TAG_LENGTH property as the source of the
   tag lengths a mode accepts. "Cryptography Primitive Property Identifiers
   (Bcrypt.h)" gives its type, a BCRYPT_AUTH_TAG_LENGTHS_STRUCT. */
static bool ZwiftCrypto_TagAllowed(const BCRYPT_AUTH_TAG_LENGTHS_STRUCT *lengths)
{
    const ULONG tag = ZWIFT_TAG_SIZE;

    if (tag < lengths->dwMinLength || tag > lengths->dwMaxLength) {
        return false;
    }
    if (lengths->dwIncrement == 0) {
        return tag == lengths->dwMinLength;
    }
    return (tag - lengths->dwMinLength) % lengths->dwIncrement == 0;
}

/* The session key and the derived bytes go. "BCryptDestroyKey function
   (bcrypt.h)" */
static void ZwiftCrypto_DropSession(SDL_ZwiftKeys *keys)
{
    if (keys->session) {
        keys->DestroyKey(keys->session);
        keys->session = NULL;
    }
    SecureZeroMemory(keys->hkdf, sizeof(keys->hkdf));
}

/* The key pair goes too */
static void ZwiftCrypto_DropPair(SDL_ZwiftKeys *keys)
{
    ZwiftCrypto_DropSession(keys);
    if (keys->pair) {
        keys->DestroyKey(keys->pair);
        keys->pair = NULL;
    }
    keys->fixed = false;
    SecureZeroMemory(keys->public_key, sizeof(keys->public_key));
}

/* The public key of pair as X then Y. "BCryptExportKey function (bcrypt.h)":
   a BCRYPT_ECCPUBLIC_BLOB is a BCRYPT_ECCKEY_BLOB followed by the key data.
   "BCRYPT_ECCKEY_BLOB (bcrypt.h)": the data is X[cbKey] then Y[cbKey], both
   big-endian, so a coordinate keeps its leading zero bytes. */
static bool ZwiftCrypto_ExportPublic(SDL_ZwiftKeys *keys, BCRYPT_KEY_HANDLE pair, uint8_t *public_key)
{
    uint8_t blob[ZWIFT_PUBLIC_BLOB_SIZE];
    BCRYPT_ECCKEY_BLOB header;
    ULONG size = 0;

    if (!BCRYPT_SUCCESS(keys->ExportKey(pair, NULL, BCRYPT_ECCPUBLIC_BLOB, blob, (ULONG)sizeof(blob), &size, 0)) ||
        size != sizeof(blob)) {
        return false;
    }
    memcpy(&header, blob, sizeof(header));
    if (header.dwMagic != BCRYPT_ECDH_PUBLIC_P256_MAGIC || header.cbKey != ZWIFT_FIELD_SIZE) {
        return false;
    }
    memcpy(public_key, &blob[sizeof(header)], SDL_ZWIFT_KEY_SIZE);
    return true;
}

/* HMAC-SHA256 of first then second, either of which may be empty.
   "BCryptCreateHash function (bcrypt.h)": with no object buffer CNG holds the
   object itself, and pbSecret is the key of an algorithm opened with
   BCRYPT_ALG_HANDLE_HMAC_FLAG. The data goes in with "BCryptHashData
   function (bcrypt.h)". "BCryptFinishHash function (bcrypt.h)": cbOutput
   must be the hash length. The object goes with "BCryptDestroyHash function
   (bcrypt.h)". */
static bool ZwiftCrypto_HMAC(SDL_ZwiftKeys *keys, const uint8_t *key, ULONG key_length, const uint8_t *first,
                             ULONG first_length, const uint8_t *second, ULONG second_length, uint8_t *mac)
{
    BCRYPT_HASH_HANDLE hash = NULL;
    bool ok;

    if (!BCRYPT_SUCCESS(keys->CreateHash(keys->hmac, &hash, NULL, 0, (PUCHAR)key, key_length, 0))) {
        return false;
    }
    ok = (first_length == 0 || BCRYPT_SUCCESS(keys->HashData(hash, (PUCHAR)first, first_length, 0))) &&
         (second_length == 0 || BCRYPT_SUCCESS(keys->HashData(hash, (PUCHAR)second, second_length, 0))) &&
         BCRYPT_SUCCESS(keys->FinishHash(hash, mac, ZWIFT_SHA256_SIZE, 0));
    keys->DestroyHash(hash);
    return ok;
}

/* HKDF with SHA-256 and empty info, 36 bytes of output (RFC 5869, 2.2
   "Step 1: Extract" and 2.3 "Step 2: Expand"):
   PRK = HMAC(salt, secret), T(1) = HMAC(PRK, 01), T(2) = HMAC(PRK, T(1) | 02),
   output = T(1) | the first 4 bytes of T(2) */
static bool ZwiftCrypto_HKDF(SDL_ZwiftKeys *keys, const uint8_t *secret, const uint8_t *salt, uint8_t *output)
{
    uint8_t prk[ZWIFT_SHA256_SIZE], first[ZWIFT_SHA256_SIZE], second[ZWIFT_SHA256_SIZE];
    uint8_t one = 0x01, two = 0x02;
    bool ok;

    ok = ZwiftCrypto_HMAC(keys, salt, ZWIFT_SALT_SIZE, secret, ZWIFT_FIELD_SIZE, NULL, 0, prk) &&
         ZwiftCrypto_HMAC(keys, prk, ZWIFT_SHA256_SIZE, &one, 1, NULL, 0, first) &&
         ZwiftCrypto_HMAC(keys, prk, ZWIFT_SHA256_SIZE, first, ZWIFT_SHA256_SIZE, &two, 1, second);
    if (ok) {
        memcpy(output, first, ZWIFT_SHA256_SIZE);
        memcpy(&output[ZWIFT_SHA256_SIZE], second, SDL_ZWIFT_HKDF_SIZE - ZWIFT_SHA256_SIZE);
    }
    SecureZeroMemory(prk, sizeof(prk));
    SecureZeroMemory(first, sizeof(first));
    SecureZeroMemory(second, sizeof(second));
    return ok;
}

/* A new key pair each time, the host's half of a new exchange. With
   SetKeyPair's pair in place, that pair's key instead.
   "BCryptGenerateKeyPair function (bcrypt.h)": the key cannot be used until
   "BCryptFinalizeKeyPair function (bcrypt.h)" is called. */
static bool ZwiftCrypto_MakeKey(void *userdata, uint8_t *public_key)
{
    SDL_ZwiftKeys *keys = (SDL_ZwiftKeys *)userdata;
    BCRYPT_KEY_HANDLE pair = NULL;
    uint8_t key[SDL_ZWIFT_KEY_SIZE];

    ZwiftCrypto_DropSession(keys);
    if (keys->fixed) {
        memcpy(public_key, keys->public_key, SDL_ZWIFT_KEY_SIZE);
        return true;
    }
    ZwiftCrypto_DropPair(keys);
    if (!BCRYPT_SUCCESS(keys->GenerateKeyPair(keys->ecdh, &pair, ZWIFT_KEY_BITS, 0))) {
        return false;
    }
    if (!BCRYPT_SUCCESS(keys->FinalizeKeyPair(pair, 0)) || !ZwiftCrypto_ExportPublic(keys, pair, key)) {
        keys->DestroyKey(pair);
        return false;
    }
    keys->pair = pair;
    memcpy(keys->public_key, key, sizeof(key));
    memcpy(public_key, key, sizeof(key));
    return true;
}

/* The session key from the device's public key. "BCryptImportKeyPair
   function (bcrypt.h)" takes the key as a BCRYPT_ECCPUBLIC_BLOB and does not
   say that it checks the point, so the key is checked here first.
   "BCryptSecretAgreement function (bcrypt.h)" pairs it with the host's
   private key. "BCryptDeriveKey function (bcrypt.h)": BCRYPT_KDF_RAW_SECRET
   returns the raw secret, the X coordinate for ECDH, in little-endian order,
   so it is reversed into the big-endian X. The secret goes with
   "BCryptDestroySecret function (bcrypt.h)". The AES key comes from
   "BCryptGenerateSymmetricKey function (bcrypt.h)" on the CCM algorithm
   handle, with no object buffer. */
static bool ZwiftCrypto_Derive(void *userdata, const uint8_t *device_key)
{
    SDL_ZwiftKeys *keys = (SDL_ZwiftKeys *)userdata;
    uint8_t blob[ZWIFT_PUBLIC_BLOB_SIZE];
    uint8_t raw[ZWIFT_FIELD_SIZE], secret[ZWIFT_FIELD_SIZE], salt[ZWIFT_SALT_SIZE], output[SDL_ZWIFT_HKDF_SIZE];
    BCRYPT_ECCKEY_BLOB header;
    BCRYPT_KEY_HANDLE device = NULL, session = NULL;
    BCRYPT_SECRET_HANDLE agreed = NULL;
    ULONG size = 0;
    bool ok = false;
    int i;

    ZwiftCrypto_DropSession(keys);
    if (!keys->pair || !ZwiftCrypto_OnCurve(device_key)) {
        return false;
    }
    header.dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header.cbKey = ZWIFT_FIELD_SIZE;
    memcpy(blob, &header, sizeof(header));
    memcpy(&blob[sizeof(header)], device_key, SDL_ZWIFT_KEY_SIZE);
    if (!BCRYPT_SUCCESS(keys->ImportKeyPair(keys->ecdh, NULL, BCRYPT_ECCPUBLIC_BLOB, &device, blob, (ULONG)sizeof(blob), 0))) {
        return false;
    }
    if (BCRYPT_SUCCESS(keys->SecretAgreement(keys->pair, device, &agreed, 0))) {
        if (BCRYPT_SUCCESS(keys->DeriveKey(agreed, BCRYPT_KDF_RAW_SECRET, NULL, raw, (ULONG)sizeof(raw), &size, 0)) &&
            size == sizeof(raw)) {
            for (i = 0; i < ZWIFT_FIELD_SIZE; ++i) {
                secret[i] = raw[ZWIFT_FIELD_SIZE - 1 - i];
            }
            /* The salt is the device's key then the host's */
            memcpy(salt, device_key, SDL_ZWIFT_KEY_SIZE);
            memcpy(&salt[SDL_ZWIFT_KEY_SIZE], keys->public_key, SDL_ZWIFT_KEY_SIZE);
            if (ZwiftCrypto_HKDF(keys, secret, salt, output) &&
                BCRYPT_SUCCESS(keys->GenerateSymmetricKey(keys->aes, &session, NULL, 0, output, ZWIFT_AES_SIZE, 0))) {
                keys->session = session;
                memcpy(keys->hkdf, output, sizeof(output));
                ok = true;
            }
        }
        keys->DestroySecret(agreed);
    }
    keys->DestroyKey(device);
    SecureZeroMemory(raw, sizeof(raw));
    SecureZeroMemory(secret, sizeof(secret));
    SecureZeroMemory(output, sizeof(output));
    return ok;
}

/* 4 counter bytes, the ciphertext and a 4-byte tag into length - 8 bytes of
   plain. "BCryptDecrypt function (bcrypt.h)": an authenticated mode takes a
   BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO as the padding information and
   returns STATUS_AUTH_TAG_MISMATCH when the tag does not match. The
   structure is set up with BCRYPT_INIT_AUTH_MODE_INFO, and its nonce, tag
   and absent associated data follow "BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO
   (bcrypt.h)". When CNG turns a frame down, plain is zeroed, so no
   unauthenticated bytes stay behind. */
static bool ZwiftCrypto_Decrypt(void *userdata, const uint8_t *frame, size_t length, uint8_t *plain)
{
    SDL_ZwiftKeys *keys = (SDL_ZwiftKeys *)userdata;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    uint8_t nonce[ZWIFT_NONCE_SIZE], tag[ZWIFT_TAG_SIZE];
    ULONG count, size = 0;
    NTSTATUS status;

    if (!keys->session || length <= SDL_ZWIFT_FRAME_EXTRA || length > ZWIFT_MAX_FRAME) {
        return false;
    }
    count = (ULONG)(length - SDL_ZWIFT_FRAME_EXTRA);
    /* The nonce prefix, then the counter bytes exactly as received */
    memcpy(nonce, &keys->hkdf[ZWIFT_AES_SIZE], ZWIFT_PREFIX_SIZE);
    memcpy(&nonce[ZWIFT_PREFIX_SIZE], frame, ZWIFT_COUNTER_SIZE);
    memcpy(tag, &frame[length - ZWIFT_TAG_SIZE], ZWIFT_TAG_SIZE);
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = ZWIFT_NONCE_SIZE;
    info.pbTag = tag;
    info.cbTag = ZWIFT_TAG_SIZE;
    status = keys->Decrypt(keys->session, (PUCHAR)&frame[ZWIFT_COUNTER_SIZE], count, &info, NULL, 0, plain, count, &size, 0);
    SecureZeroMemory(nonce, sizeof(nonce));
    if (!BCRYPT_SUCCESS(status) || size != count) {
        SecureZeroMemory(plain, count);
        return false;
    }
    return true;
}

SDL_ZwiftKeys *SDL_ZwiftCrypto_Create(SDL_ZwiftCrypto *crypto)
{
    SDL_ZwiftKeys *keys;
    BCRYPT_AUTH_TAG_LENGTHS_STRUCT lengths;
    ULONG size = 0;

    if (!crypto) {
        return NULL;
    }
    memset(crypto, 0, sizeof(*crypto));
    keys = (SDL_ZwiftKeys *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*keys));
    if (!keys) {
        return NULL;
    }
    /* "CNG Algorithm Identifiers (Bcrypt.h)": ECDH on P-256, SHA-256 made
       HMAC by BCRYPT_ALG_HANDLE_HMAC_FLAG, and AES. "BCryptSetProperty
       function (bcrypt.h)" sets BCRYPT_CHAINING_MODE on the AES handle, and
       the keys made from it take the mode, as "Encrypting Data with CNG" sets
       BCRYPT_CHAIN_MODE_CBC with its sizeof. "BCryptGetProperty function
       (bcrypt.h)" then reads the tag lengths CCM accepts. */
    if (!ZwiftCrypto_Load(keys) ||
        !ZwiftCrypto_Open(keys, &keys->ecdh, BCRYPT_ECDH_P256_ALGORITHM, 0) ||
        !ZwiftCrypto_Open(keys, &keys->hmac, BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG) ||
        !ZwiftCrypto_Open(keys, &keys->aes, BCRYPT_AES_ALGORITHM, 0) ||
        !BCRYPT_SUCCESS(keys->SetProperty(keys->aes, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CCM,
                                          (ULONG)sizeof(BCRYPT_CHAIN_MODE_CCM), 0)) ||
        !BCRYPT_SUCCESS(keys->GetProperty(keys->aes, BCRYPT_AUTH_TAG_LENGTH, (PUCHAR)&lengths, (ULONG)sizeof(lengths),
                                          &size, 0)) ||
        size != sizeof(lengths) || !ZwiftCrypto_TagAllowed(&lengths)) {
        SDL_ZwiftCrypto_Destroy(keys);
        return NULL;
    }
    crypto->userdata = keys;
    crypto->MakeKey = ZwiftCrypto_MakeKey;
    crypto->Derive = ZwiftCrypto_Derive;
    crypto->Decrypt = ZwiftCrypto_Decrypt;
    return keys;
}

/* Every key goes before the algorithm handles that made it.
   "BCryptCloseAlgorithmProvider function (bcrypt.h)" */
void SDL_ZwiftCrypto_Destroy(SDL_ZwiftKeys *keys)
{
    if (!keys) {
        return;
    }
    ZwiftCrypto_DropPair(keys);
    if (keys->aes) {
        keys->CloseAlgorithmProvider(keys->aes, 0);
    }
    if (keys->hmac) {
        keys->CloseAlgorithmProvider(keys->hmac, 0);
    }
    if (keys->ecdh) {
        keys->CloseAlgorithmProvider(keys->ecdh, 0);
    }
    if (keys->library) {
        FreeLibrary(keys->library);
    }
    SecureZeroMemory(keys, sizeof(*keys));
    HeapFree(GetProcessHeap(), 0, keys);
}

/* "BCryptImportKeyPair function (bcrypt.h)" takes a BCRYPT_ECCPRIVATE_BLOB,
   which "BCRYPT_ECCKEY_BLOB (bcrypt.h)" lays out as X, Y and d, each cbKey
   big-endian bytes. The key sent is then read back from CNG. */
bool SDL_ZwiftCrypto_SetKeyPair(SDL_ZwiftKeys *keys, const uint8_t *d, const uint8_t *x, const uint8_t *y)
{
    uint8_t blob[ZWIFT_PRIVATE_BLOB_SIZE];
    uint8_t key[SDL_ZWIFT_KEY_SIZE];
    BCRYPT_ECCKEY_BLOB header;
    BCRYPT_KEY_HANDLE pair = NULL;
    NTSTATUS status;

    if (!keys || !d || !x || !y) {
        return false;
    }
    header.dwMagic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
    header.cbKey = ZWIFT_FIELD_SIZE;
    memcpy(blob, &header, sizeof(header));
    memcpy(&blob[sizeof(header)], x, ZWIFT_FIELD_SIZE);
    memcpy(&blob[sizeof(header) + ZWIFT_FIELD_SIZE], y, ZWIFT_FIELD_SIZE);
    memcpy(&blob[sizeof(header) + 2 * ZWIFT_FIELD_SIZE], d, ZWIFT_FIELD_SIZE);
    status = keys->ImportKeyPair(keys->ecdh, NULL, BCRYPT_ECCPRIVATE_BLOB, &pair, blob, (ULONG)sizeof(blob), 0);
    SecureZeroMemory(blob, sizeof(blob));
    if (!BCRYPT_SUCCESS(status)) {
        return false;
    }
    if (!ZwiftCrypto_ExportPublic(keys, pair, key)) {
        keys->DestroyKey(pair);
        return false;
    }
    ZwiftCrypto_DropPair(keys);
    keys->pair = pair;
    keys->fixed = true;
    memcpy(keys->public_key, key, sizeof(key));
    return true;
}

bool SDL_ZwiftCrypto_GetDerived(const SDL_ZwiftKeys *keys, uint8_t *hkdf)
{
    if (!keys || !keys->session || !hkdf) {
        return false;
    }
    memcpy(hkdf, keys->hkdf, SDL_ZWIFT_HKDF_SIZE);
    return true;
}

#endif /* _WIN32 */
