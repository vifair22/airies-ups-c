#include "api/auth.h"
#include <cutils/log.h>

#include <openssl/sha.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Hashing --- */

static void sha256_hex(const char *input, char *output)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)input, strlen(input), hash);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(output + i * 2, "%02x", hash[i]);
    output[SHA256_DIGEST_LENGTH * 2] = '\0';
}

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
    char hash[65];
    sha256_hex(password, hash);

    if (auth_is_setup(db)) {
        const char *params[] = { hash, NULL };
        return db_execute_non_query(db,
            "UPDATE auth SET password_hash = ?, updated_at = datetime('now') "
            "WHERE id = 1",
            params, NULL);
    }

    const char *params[] = { hash, NULL };
    return db_execute_non_query(db,
        "INSERT INTO auth (id, password_hash) VALUES (1, ?)",
        params, NULL);
}

int auth_verify_password(cutils_db_t *db, const char *password)
{
    char hash[65];
    sha256_hex(password, hash);

    const char *params[] = { hash, NULL };
    db_result_t *result = NULL;
    int rc = db_execute(db,
        "SELECT id FROM auth WHERE id = 1 AND password_hash = ?",
        params, &result);

    if (rc != 0 || !result) return 0;
    int valid = result->nrows > 0;
    db_result_free(result);
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
