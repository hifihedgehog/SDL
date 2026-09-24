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

/* Writes src/joystick/SDL_gamepad_db_community.h from the pinned community
   SDL_GameControllerDB in build-scripts/gamecontrollerdb, or checks that the
   checked-in header still matches it byte for byte.

   gen_community_mappings write|check|report <gamecontrollerdb.txt> <source.txt> <SDL_gamepad_db.h> <SDL_gamepad_db_community.h>

   test/community-mappings builds it against the static library from this
   tree, because only the HIDAPI drivers' own IsSupportedDevice logic can say
   which devices they read, and the shared library does not export it.

   A Windows row is kept when
   - its GUID is the USB form 03000000vvvv0000pppp000000000000 with a nonzero
     vendor and product,
   - SDL_gamepad_db.h maps that vendor and product on no platform,
   - controller_list.h does not list it,
   - and no HIDAPI driver supports it, whether or not a hint enables that
     driver.
   Each kept row is copied as written, minus its platform field, which the
   SDL_JOYSTICK_DINPUT section of SDL_gamepad_db.h already implies. The rows
   are sorted by GUID.

   To move the pin: copy gamecontrollerdb.txt and LICENSE from the community
   repository at the new commit, byte for byte, put that commit in
   source.txt, run the write mode, and commit all four files together.
*/

#include <SDL3/SDL.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/joystick/controller_type.h"
#include "../src/joystick/controller_list.h"

/* Declared in src/joystick/hidapi/SDL_hidapijoystick_c.h, in the static library. */
extern bool HIDAPI_IsDeviceSupportedByAnyDriver(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name);

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* SHA-256, FIPS 180-4. */

static const Uint32 sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void Sha256Block(Uint32 state[8], const Uint8 block[64])
{
    Uint32 w[64];
    Uint32 a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; ++i) {
        w[i] = ((Uint32)block[i * 4] << 24) | ((Uint32)block[i * 4 + 1] << 16) |
               ((Uint32)block[i * 4 + 2] << 8) | (Uint32)block[i * 4 + 3];
    }
    for (i = 16; i < 64; ++i) {
        const Uint32 s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const Uint32 s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];
    for (i = 0; i < 64; ++i) {
        const Uint32 sum1 = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        const Uint32 choose = (e & f) ^ (~e & g);
        const Uint32 t1 = h + sum1 + choose + sha256_k[i] + w[i];
        const Uint32 sum0 = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        const Uint32 majority = (a & b) ^ (a & c) ^ (b & c);
        const Uint32 t2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

/* Writes 64 lowercase hex digits and a terminator. */
static void Sha256Hex(const Uint8 *data, size_t length, char hex[65])
{
    Uint32 state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    Uint8 block[64];
    const Uint64 bits = (Uint64)length * 8;
    size_t offset = 0;
    size_t tail;
    int i;

    while (length - offset >= 64) {
        Sha256Block(state, data + offset);
        offset += 64;
    }

    /* The last bytes, a 1 bit, zeros, and the length in bits, big-endian. */
    tail = length - offset;
    memset(block, 0, sizeof(block));
    if (tail) {
        memcpy(block, data + offset, tail);
    }
    block[tail] = 0x80;
    if (tail >= 56) {
        Sha256Block(state, block);
        memset(block, 0, sizeof(block));
    }
    for (i = 0; i < 8; ++i) {
        block[63 - i] = (Uint8)(bits >> (i * 8));
    }
    Sha256Block(state, block);

    for (i = 0; i < 32; ++i) {
        static const char digits[] = "0123456789abcdef";
        const Uint8 byte = (Uint8)(state[i / 4] >> (24 - (i % 4) * 8));
        hex[i * 2] = digits[byte >> 4];
        hex[i * 2 + 1] = digits[byte & 0x0F];
    }
    hex[64] = '\0';
}

/* Published FIPS 180-2 examples. A wrong constant or step fails here. */
static bool Sha256SelfTest(void)
{
    static const struct
    {
        const char *message;
        const char *digest;
    } vectors[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    size_t i;

    for (i = 0; i < ARRAY_COUNT(vectors); ++i) {
        char hex[65];
        Sha256Hex((const Uint8 *)vectors[i].message, strlen(vectors[i].message), hex);
        if (strcmp(hex, vectors[i].digest) != 0) {
            fprintf(stderr, "SHA-256 self-test %u failed: %s\n", (unsigned int)i, hex);
            return false;
        }
    }
    return true;
}

/* Vendor and product pairs, as (vendor << 16) | product, sorted. */

typedef struct PairSet
{
    Uint32 *pairs;
    size_t count;
    size_t capacity;
} PairSet;

static bool PairSetAdd(PairSet *set, Uint16 vendor, Uint16 product)
{
    if (set->count == set->capacity) {
        const size_t capacity = set->capacity ? set->capacity * 2 : 256;
        Uint32 *pairs = (Uint32 *)realloc(set->pairs, capacity * sizeof(*pairs));
        if (!pairs) {
            return false;
        }
        set->pairs = pairs;
        set->capacity = capacity;
    }
    set->pairs[set->count++] = ((Uint32)vendor << 16) | product;
    return true;
}

static int ComparePairs(const void *a, const void *b)
{
    const Uint32 x = *(const Uint32 *)a;
    const Uint32 y = *(const Uint32 *)b;
    return (x > y) - (x < y);
}

static void PairSetSort(PairSet *set)
{
    if (set->count) {
        qsort(set->pairs, set->count, sizeof(*set->pairs), ComparePairs);
    }
}

static bool PairSetHas(const PairSet *set, Uint16 vendor, Uint16 product)
{
    const Uint32 key = ((Uint32)vendor << 16) | product;
    return set->count && bsearch(&key, set->pairs, set->count, sizeof(*set->pairs), ComparePairs) != NULL;
}

/* Every quoted GUID in a C source, with comments skipped. The standard
   vendor and product form of each is decoded by SDL's own function, the one
   gamepad mapping lookups use. */
static bool CollectMappedPairs(const char *text, size_t length, PairSet *set)
{
    size_t i = 0;

    while (i < length) {
        if (text[i] == '/' && i + 1 < length && text[i + 1] == '/') {
            while (i < length && text[i] != '\n') {
                ++i;
            }
        } else if (text[i] == '/' && i + 1 < length && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < length && !(text[i] == '*' && text[i + 1] == '/')) {
                ++i;
            }
            i += 2;
        } else if (text[i] == '\'') {
            ++i;
            while (i < length && text[i] != '\'') {
                i += (text[i] == '\\') ? 2 : 1;
            }
            ++i;
        } else if (text[i] == '"') {
            const size_t start = ++i;
            size_t j;
            bool guid = true;

            while (i < length && text[i] != '"') {
                i += (text[i] == '\\') ? 2 : 1;
            }
            if (i - start < 33 || text[start + 32] != ',') {
                guid = false;
            }
            for (j = 0; guid && j < 32; ++j) {
                if (!SDL_isxdigit((unsigned char)text[start + j])) {
                    guid = false;
                }
            }
            if (guid) {
                char string[33];
                Uint16 vendor = 0, product = 0;

                memcpy(string, &text[start], 32);
                string[32] = '\0';
                SDL_GetJoystickGUIDInfo(SDL_StringToGUID(string), &vendor, &product, NULL, NULL);
                if (vendor && product && !PairSetAdd(set, vendor, product)) {
                    return false;
                }
            }
            ++i;
        } else {
            ++i;
        }
    }
    PairSetSort(set);
    return true;
}

static bool CollectListedPairs(PairSet *set)
{
    size_t i;

    for (i = 0; i < ARRAY_COUNT(arrControllers); ++i) {
        const unsigned int id = arrControllers[i].m_unDeviceID;
        if (!PairSetAdd(set, (Uint16)(id >> 16), (Uint16)(id & 0xFFFF))) {
            return false;
        }
    }
    PairSetSort(set);
    return true;
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* 03000000vvvv0000pppp000000000000, the IDs little-endian. */
static bool ParseUSBGUID(const char *guid, size_t length, Uint16 *vendor, Uint16 *product)
{
    static const char pattern[] = "03000000xxxx0000xxxx000000000000";
    int digits[32];
    size_t i;

    if (length != 32) {
        return false;
    }
    for (i = 0; i < 32; ++i) {
        digits[i] = HexDigit(guid[i]);
        if (digits[i] < 0 || (pattern[i] != 'x' && guid[i] != pattern[i])) {
            return false;
        }
    }
    *vendor = (Uint16)((digits[8] << 4) | digits[9] | (digits[10] << 12) | (digits[11] << 8));
    *product = (Uint16)((digits[16] << 4) | digits[17] | (digits[18] << 12) | (digits[19] << 8));
    return *vendor != 0 && *product != 0;
}

typedef struct Text
{
    char *data;
    size_t length;
    size_t capacity;
    bool failed; /* An allocation failed, and the text is incomplete */
} Text;

static bool TextAppend(Text *text, const char *data, size_t length)
{
    if (text->failed) {
        return false;
    }
    if (text->length + length + 1 > text->capacity) {
        size_t capacity = text->capacity ? text->capacity : 4096;
        char *grown;
        while (text->length + length + 1 > capacity) {
            capacity *= 2;
        }
        grown = (char *)realloc(text->data, capacity);
        if (!grown) {
            text->failed = true;
            return false;
        }
        text->data = grown;
        text->capacity = capacity;
    }
    memcpy(text->data + text->length, data, length);
    text->length += length;
    text->data[text->length] = '\0';
    return true;
}

static bool TextAppendString(Text *text, const char *string)
{
    return TextAppend(text, string, strlen(string));
}

typedef struct Row
{
    char *text;  /* The row minus its platform field */
    Uint32 pair; /* (vendor << 16) | product */
} Row;

static int CompareRows(const void *a, const void *b)
{
    return strcmp(((const Row *)a)->text, ((const Row *)b)->text);
}

typedef struct Counts
{
    unsigned int windows;
    unsigned int usb;
    unsigned int mapped;
    unsigned int listed;
    unsigned int hidapi;
    unsigned int kept;
} Counts;

static void *LoadFile(const char *path, size_t *length)
{
    void *data = SDL_LoadFile(path, length);
    if (!data) {
        fprintf(stderr, "Couldn't read %s: %s\n", path, SDL_GetError());
    }
    return data;
}

/* SDL hints come from the environment too. Clear every SDL variable so the
   output depends on the files alone. */
static void ClearSDLEnvironment(void)
{
    SDL_Environment *environment = SDL_GetEnvironment();
    char **variables = SDL_GetEnvironmentVariables(environment);
    int i;

    if (!variables) {
        return;
    }
    for (i = 0; variables[i]; ++i) {
        if (SDL_strncmp(variables[i], "SDL_", 4) == 0) {
            char *equals = SDL_strchr(variables[i], '=');
            if (equals) {
                *equals = '\0';
            }
            SDL_UnsetEnvironmentVariable(environment, variables[i]);
        }
    }
    SDL_free(variables);
}

int main(int argc, char *argv[])
{
    const char *mode, *db_path, *source_path, *mappings_path, *header_path;
    char *db = NULL, *source = NULL, *mappings = NULL, *existing = NULL;
    size_t db_length = 0, source_length = 0, mappings_length = 0, existing_length = 0;
    PairSet mapped = { NULL, 0, 0 }, listed = { NULL, 0, 0 };
    Row *rows = NULL;
    size_t row_count = 0, row_capacity = 0;
    Counts counts;
    Text out = { NULL, 0, 0, false };
    char sha256[65], repository[256], commit[64], line[512];
    char *cursor, *end;
    size_t i;
    int status = 1;

    if (argc != 6 || (strcmp(argv[1], "write") != 0 && strcmp(argv[1], "check") != 0 && strcmp(argv[1], "report") != 0)) {
        fprintf(stderr, "Usage: %s write|check|report <gamecontrollerdb.txt> <source.txt> <SDL_gamepad_db.h> <SDL_gamepad_db_community.h>\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    db_path = argv[2];
    source_path = argv[3];
    mappings_path = argv[4];
    header_path = argv[5];

    ClearSDLEnvironment();
    if (!Sha256SelfTest()) {
        return 1;
    }
    /* The Switch 2 driver is compiled only with libusb. Without it the HIDAPI
       check would keep rows for devices the shipped library reads. */
    if (!HIDAPI_IsDeviceSupportedByAnyDriver(0x057E, 0x2073, 0, "Switch 2 GameCube Controller")) {
        fprintf(stderr, "The static library lacks the Switch 2 HIDAPI driver. Configure it with SDL_HIDAPI_LIBUSB and a libusb, as the shared library is.\n");
        return 1;
    }

    db = (char *)LoadFile(db_path, &db_length);
    source = (char *)LoadFile(source_path, &source_length);
    mappings = (char *)LoadFile(mappings_path, &mappings_length);
    if (!db || !source || !mappings) {
        goto done;
    }
    if (memchr(db, '\r', db_length)) {
        fprintf(stderr, "%s has carriage returns. It must be the repository's file byte for byte, which .gitattributes keeps.\n", db_path);
        goto done;
    }
    Sha256Hex((const Uint8 *)db, db_length, sha256);

    /* source.txt: the repository URL, then the commit. */
    if (SDL_sscanf(source, "%255s %63s", repository, commit) != 2 || strlen(commit) != 40 ||
        strspn(commit, "0123456789abcdef") != 40) {
        fprintf(stderr, "%s must hold the repository URL and a 40-digit commit hash.\n", source_path);
        goto done;
    }

    if (!CollectMappedPairs(mappings, mappings_length, &mapped) || !CollectListedPairs(&listed)) {
        fprintf(stderr, "Out of memory\n");
        goto done;
    }

    SDL_zero(counts);
    cursor = db;
    end = db + db_length;
    while (cursor < end) {
        char *line_end = (char *)memchr(cursor, '\n', (size_t)(end - cursor));
        const size_t length = line_end ? (size_t)(line_end - cursor) : (size_t)(end - cursor);
        const char *fields[64];
        size_t field_lengths[64];
        size_t field_count = 0, platform = 0, start = 0;
        bool windows = false;
        Uint16 vendor, product;
        char name[256];
        const char *reason = NULL;

        if (length >= sizeof(line)) {
            fprintf(stderr, "Line too long: %.60s\n", cursor);
            goto done;
        }
        memcpy(line, cursor, length);
        line[length] = '\0';
        cursor += length + (line_end ? 1 : 0);

        if (length == 0 || line[0] == '#') {
            continue;
        }

        for (i = 0; i <= length; ++i) {
            if (i == length || line[i] == ',') {
                if (field_count == ARRAY_COUNT(fields)) {
                    fprintf(stderr, "Too many fields: %.60s\n", line);
                    goto done;
                }
                fields[field_count] = &line[start];
                field_lengths[field_count] = i - start;
                if (field_lengths[field_count] == 16 && strncmp(fields[field_count], "platform:Windows", 16) == 0) {
                    windows = true;
                    platform = field_count;
                }
                ++field_count;
                start = i + 1;
            }
        }
        if (!windows) {
            continue;
        }
        ++counts.windows;

        if (field_count < 3 || !ParseUSBGUID(fields[0], field_lengths[0], &vendor, &product)) {
            continue;
        }
        ++counts.usb;

        /* The row becomes a C string literal. */
        for (i = 0; i < length; ++i) {
            if (line[i] < 0x20 || line[i] > 0x7E || line[i] == '"' || line[i] == '\\' ||
                (line[i] == '?' && i + 1 < length && line[i + 1] == '?')) {
                fprintf(stderr, "Row needs escaping, which this generator does not do: %s\n", line);
                goto done;
            }
        }
        if (field_lengths[1] >= sizeof(name)) {
            fprintf(stderr, "Name too long: %s\n", line);
            goto done;
        }
        memcpy(name, fields[1], field_lengths[1]);
        name[field_lengths[1]] = '\0';

        if (PairSetHas(&mapped, vendor, product)) {
            ++counts.mapped;
            reason = "mapped in SDL_gamepad_db.h";
        } else if (PairSetHas(&listed, vendor, product)) {
            ++counts.listed;
            reason = "listed in controller_list.h";
        } else if (HIDAPI_IsDeviceSupportedByAnyDriver(vendor, product, 0, name)) {
            ++counts.hidapi;
            reason = "supported by a HIDAPI driver";
        }
        if (strcmp(mode, "report") == 0) {
            printf("%04X:%04X %-30s %s\n", vendor, product, reason ? reason : "kept", name);
        }
        if (reason) {
            continue;
        }

        /* Two rows for one device would leave SDL to pick one. */
        for (i = 0; i < row_count; ++i) {
            if (rows[i].pair == (((Uint32)vendor << 16) | product)) {
                fprintf(stderr, "Two Windows rows map %04X:%04X\n", vendor, product);
                goto done;
            }
        }

        {
            Text row = { NULL, 0, 0, false };
            size_t f;

            for (f = 0; f < field_count; ++f) {
                if (f == platform) {
                    continue;
                }
                if (f > 0) {
                    TextAppend(&row, ",", 1);
                }
                TextAppend(&row, fields[f], field_lengths[f]);
            }
            if (row.failed || !row.data) {
                fprintf(stderr, "Out of memory\n");
                free(row.data);
                goto done;
            }
            if (row_count == row_capacity) {
                const size_t capacity = row_capacity ? row_capacity * 2 : 256;
                Row *grown = (Row *)realloc(rows, capacity * sizeof(*rows));
                if (!grown) {
                    free(row.data);
                    goto done;
                }
                rows = grown;
                row_capacity = capacity;
            }
            rows[row_count].text = row.data;
            rows[row_count].pair = ((Uint32)vendor << 16) | product;
            ++row_count;
            ++counts.kept;
        }
    }
    if (row_count) {
        qsort(rows, row_count, sizeof(*rows), CompareRows);
    }

    /* The header. */
    TextAppendString(&out,
        "/*\n"
        "  Simple DirectMedia Layer\n"
        "  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>\n"
        "\n"
        "  This software is provided 'as-is', without any express or implied\n"
        "  warranty.  In no event will the authors be held liable for any damages\n"
        "  arising from the use of this software.\n"
        "\n"
        "  Permission is granted to anyone to use this software for any purpose,\n"
        "  including commercial applications, and to alter it and redistribute it\n"
        "  freely, subject to the following restrictions:\n"
        "\n"
        "  1. The origin of this software must not be misrepresented; you must not\n"
        "     claim that you wrote the original software. If you use this software\n"
        "     in a product, an acknowledgment in the product documentation would be\n"
        "     appreciated but is not required.\n"
        "  2. Altered source versions must be plainly marked as such, and must not be\n"
        "     misrepresented as being the original software.\n"
        "  3. This notice may not be removed or altered from any source distribution.\n"
        "*/\n"
        "\n"
        "/* GENERATED FILE, do not edit. build-scripts/gen_community_mappings.c\n"
        "   writes it from the pinned community SDL_GameControllerDB, and the\n"
        "   community-mappings test fails when it no longer matches.\n"
        "\n");
    SDL_snprintf(line, sizeof(line), "   Source: %s\n   Commit: %s\n", repository, commit);
    TextAppendString(&out, line);
    SDL_snprintf(line, sizeof(line), "   gamecontrollerdb.txt SHA-256: %s\n", sha256);
    TextAppendString(&out, line);
    TextAppendString(&out, "   License: zlib, in build-scripts/gamecontrollerdb/LICENSE.txt\n\n");
    SDL_snprintf(line, sizeof(line),
                 "   Windows rows: %u. In the USB form with a vendor and product: %u.\n"
                 "   Left out because SDL_gamepad_db.h maps the device on some platform: %u,\n"
                 "   because controller_list.h lists it: %u, because a HIDAPI driver\n"
                 "   supports it: %u. Kept: %u.\n"
                 "*/\n",
                 counts.windows, counts.usb, counts.mapped, counts.listed, counts.hidapi, counts.kept);
    TextAppendString(&out, line);
    for (i = 0; i < row_count; ++i) {
        TextAppendString(&out, "    \"");
        TextAppendString(&out, rows[i].text);
        TextAppendString(&out, "\",\n");
    }
    if (out.failed || !out.data) {
        fprintf(stderr, "Out of memory\n");
        goto done;
    }

    printf("Windows rows %u, USB form %u, mapped %u, listed %u, HIDAPI %u, kept %u, SHA-256 %s\n",
           counts.windows, counts.usb, counts.mapped, counts.listed, counts.hidapi, counts.kept, sha256);

    if (strcmp(mode, "write") == 0) {
        if (!SDL_SaveFile(header_path, out.data, out.length)) {
            fprintf(stderr, "Couldn't write %s: %s\n", header_path, SDL_GetError());
            goto done;
        }
        printf("Wrote %s\n", header_path);
    } else if (strcmp(mode, "check") == 0) {
        existing = (char *)LoadFile(header_path, &existing_length);
        if (!existing) {
            goto done;
        }
        if (existing_length != out.length || memcmp(existing, out.data, out.length) != 0) {
            size_t at = 0, line_number = 1;
            while (at < existing_length && at < out.length && existing[at] == out.data[at]) {
                if (existing[at] == '\n') {
                    ++line_number;
                }
                ++at;
            }
            fprintf(stderr, "%s does not match its regeneration: first difference at line %u. Run the write mode and review the change.\n",
                    header_path, (unsigned int)line_number);
            goto done;
        }
        printf("%s matches its regeneration byte for byte\n", header_path);
    }
    status = 0;

done:
    for (i = 0; i < row_count; ++i) {
        free(rows[i].text);
    }
    free(rows);
    free(out.data);
    free(mapped.pairs);
    free(listed.pairs);
    SDL_free(db);
    SDL_free(source);
    SDL_free(mappings);
    SDL_free(existing);
    return status;
}
