#include "api/auth.h"
#include <cutils/log.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Password hashing (PBKDF2-SHA256) ---
 *
 * Stored format: "$pbkdf2$100000$<salt_hex>$<hash_hex>"
 * - 100000 iterations of PBKDF2-HMAC-SHA256
 * - 16-byte random salt
 * - 32-byte derived key */

#define PBKDF2_ITERATIONS  100000
#define SALT_BYTES         16
#define KEY_BYTES          32

static void hex_encode(const unsigned char *in, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", in[i]);
    out[len * 2] = '\0';
}

static int hex_decode(const char *in, unsigned char *out, size_t max_len)
{
    size_t len = strlen(in) / 2;
    if (len > max_len) len = max_len;
    for (size_t i = 0; i < len; i++) {
        unsigned int byte;
        if (sscanf(in + i * 2, "%2x", &byte) != 1) return -1;
        out[i] = (unsigned char)byte;
    }
    return (int)len;
}

static char *hash_password(const char *password)
{
    unsigned char salt[SALT_BYTES];
    unsigned char key[KEY_BYTES];
    RAND_bytes(salt, SALT_BYTES);

    PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                       salt, SALT_BYTES,
                       PBKDF2_ITERATIONS,
                       EVP_sha256(),
                       KEY_BYTES, key);

    char salt_hex[SALT_BYTES * 2 + 1];
    char key_hex[KEY_BYTES * 2 + 1];
    hex_encode(salt, SALT_BYTES, salt_hex);
    hex_encode(key, KEY_BYTES, key_hex);

    /* "$pbkdf2$100000$<salt>$<hash>" */
    size_t total = 8 + 6 + 1 + strlen(salt_hex) + 1 + strlen(key_hex) + 1;
    char *result = malloc(total);
    if (result)
        snprintf(result, total, "$pbkdf2$%d$%s$%s", PBKDF2_ITERATIONS, salt_hex, key_hex);
    return result;
}

/* Legacy SHA256 verification for migration from old hashes */
static int verify_sha256_legacy(const char *password, const char *stored_hex)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)password, strlen(password), hash);
    char computed[65];
    hex_encode(hash, SHA256_DIGEST_LENGTH, computed);
    return strcmp(computed, stored_hex) == 0;
}

static int verify_password(const char *password, const char *stored)
{
    /* Legacy plain SHA256 hash (64 hex chars, no prefix) — migrate on verify */
    if (strncmp(stored, "$pbkdf2$", 8) != 0)
        return verify_sha256_legacy(password, stored);

    int iterations = 0;
    char salt_hex[64] = "", hash_hex[128] = "";
    if (sscanf(stored, "$pbkdf2$%d$%63[^$]$%127s", &iterations, salt_hex, hash_hex) != 3)
        return 0;
    if (iterations < 1) return 0;

    unsigned char salt[SALT_BYTES];
    int salt_len = hex_decode(salt_hex, salt, SALT_BYTES);
    if (salt_len <= 0) return 0;

    unsigned char stored_key[KEY_BYTES];
    int key_len = hex_decode(hash_hex, stored_key, KEY_BYTES);
    if (key_len <= 0) return 0;

    unsigned char derived[KEY_BYTES];
    PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                       salt, salt_len,
                       iterations,
                       EVP_sha256(),
                       key_len, derived);

    /* Constant-time comparison */
    int diff = 0;
    for (int i = 0; i < key_len; i++)
        diff |= derived[i] ^ stored_key[i];

    return diff == 0;
}

/* --- Token generation --- */

static void generate_token(char *buf, size_t len)
{
    unsigned char random[32];
    RAND_bytes(random, sizeof(random));
    size_t max = (len - 1) / 2;
    if (max > 32) max = 32;
    for (size_t i = 0; i < max; i++)
        sprintf(buf + i * 2, "%02x", random[i]);
    buf[max * 2] = '\0';
}

/* --- Public API --- */

int auth_init(cutils_db_t *db)
{
    (void)db;
    return 0;
}

int auth_is_setup(cutils_db_t *db)
{
    db_result_t *result = NULL;
    int rc = db_execute(db, "SELECT id FROM auth WHERE id = 1",
                        NULL, &result);
    if (rc != 0 || !result) return 0;
    int setup = result->nrows > 0;
    db_result_free(result);
    return setup;
}

int auth_set_password(cutils_db_t *db, const char *password)
{
    char *hashed = hash_password(password);
    if (!hashed) return -1;

    int rc;
    if (auth_is_setup(db)) {
        const char *params[] = { hashed, NULL };
        rc = db_execute_non_query(db,
            "UPDATE auth SET password_hash = ?, updated_at = datetime('now') "
            "WHERE id = 1",
            params, NULL);
    } else {
        const char *params[] = { hashed, NULL };
        rc = db_execute_non_query(db,
            "INSERT INTO auth (id, password_hash) VALUES (1, ?)",
            params, NULL);
    }

    free(hashed);
    return rc;
}

int auth_verify_password(cutils_db_t *db, const char *password)
{
    db_result_t *result = NULL;
    int rc = db_execute(db,
        "SELECT password_hash FROM auth WHERE id = 1",
        NULL, &result);

    if (rc != 0 || !result || result->nrows == 0) {
        db_result_free(result);
        return 0;
    }

    const char *stored = result->rows[0][0];
    int valid = verify_password(password, stored);
    int is_legacy = (strncmp(stored, "$pbkdf2$", 8) != 0);
    db_result_free(result);

    /* Auto-upgrade legacy SHA256 hashes to PBKDF2 on successful login */
    if (valid && is_legacy) {
        log_info("auth: upgrading legacy password hash to PBKDF2");
        auth_set_password(db, password);
    }

    return valid;
}

char *auth_create_token(cutils_db_t *db, int expire_hours)
{
    char token[65];
    generate_token(token, sizeof(token));

    char expires[32];
    time_t exp_time = time(NULL) + (time_t)expire_hours * 3600;
    struct tm tm;
    gmtime_r(&exp_time, &tm);
    strftime(expires, sizeof(expires), "%Y-%m-%d %H:%M:%S", &tm);

    const char *params[] = { token, expires, NULL };
    int rc = db_execute_non_query(db,
        "INSERT INTO sessions (token, expires_at) VALUES (?, ?)",
        params, NULL);

    if (rc != 0) return NULL;

    return strdup(token);
}

int auth_validate_token(cutils_db_t *db, const char *token)
{
    if (!token || !token[0]) return 0;

    /* Strip "Bearer " prefix if present */
    const char *tok = token;
    if (strncmp(tok, "Bearer ", 7) == 0) tok += 7;

    const char *params[] = { tok, NULL };
    db_result_t *result = NULL;
    int rc = db_execute(db,
        "SELECT token FROM sessions WHERE token = ? "
        "AND expires_at > datetime('now')",
        params, &result);

    if (rc != 0 || !result) return 0;
    int valid = result->nrows > 0;
    db_result_free(result);
    return valid;
}

void auth_cleanup_sessions(cutils_db_t *db)
{
    int deleted = 0;
    db_execute_non_query(db,
        "DELETE FROM sessions WHERE expires_at < datetime('now')",
        NULL, &deleted);
    if (deleted > 0)
        log_info("auth: cleaned up %d expired sessions", deleted);
}
