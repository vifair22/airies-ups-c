#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <openssl/sha.h>
#include <cutils/db.h>
#include <cutils/error.h>
#include "api/auth.h"

/* --- Test fixture: in-memory SQLite DB with auth schema --- */

/* Mirrors migrations/006_auth.sql (auth) + 016_sessions_v2.sql (sessions).
 * Kept as a literal so the test runs without the migration loader. */
static const char *SCHEMA =
    "CREATE TABLE auth ("
    "  id            INTEGER PRIMARY KEY CHECK (id = 1),"
    "  password_hash TEXT    NOT NULL,"
    "  created_at    TEXT    NOT NULL DEFAULT (datetime('now')),"
    "  updated_at    TEXT    NOT NULL DEFAULT (datetime('now'))"
    ");"
    "CREATE TABLE sessions ("
    "  token        TEXT PRIMARY KEY,"
    "  user_id      INTEGER,"
    "  kind         TEXT NOT NULL DEFAULT 'session',"
    "  name         TEXT,"
    "  scopes       TEXT,"
    "  created_at   TEXT NOT NULL DEFAULT (datetime('now')),"
    "  last_used_at TEXT,"
    "  expires_at   TEXT NOT NULL,"
    "  revoked_at   TEXT"
    ");"
    "CREATE INDEX idx_sessions_expires ON sessions(expires_at);"
    "CREATE INDEX idx_sessions_user    ON sessions(user_id);"
    "CREATE INDEX idx_sessions_kind    ON sessions(kind);";

static int setup(void **state)
{
    cutils_db_t *db = NULL;
    int rc = db_open(&db, "/tmp/airies_test_auth.db");
    if (rc != 0 || !db) return -1;

    rc = db_exec_raw(db, SCHEMA);
    if (rc != 0) { db_close(db); return -1; }

    *state = db;
    return 0;
}

static int teardown(void **state)
{
    cutils_db_t *db = *state;
    db_close(db);
    remove("/tmp/airies_test_auth.db");
    remove("/tmp/airies_test_auth.db-wal");
    remove("/tmp/airies_test_auth.db-shm");
    return 0;
}

/* --- auth_is_setup --- */

static void test_not_setup_initially(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(auth_is_setup(db), 0);
}

/* --- auth_set_password + auth_is_setup --- */

static void test_set_password_marks_setup(void **state)
{
    cutils_db_t *db = *state;
    int rc = auth_set_password(db, "hunter2");
    assert_int_equal(rc, 0);
    assert_int_equal(auth_is_setup(db), 1);
}

/* --- auth_verify_password --- */

static void test_verify_correct_password(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "correcthorse");
    assert_int_equal(auth_verify_password(db, "correcthorse"), 1);
}

static void test_verify_wrong_password(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "correcthorse");
    assert_int_equal(auth_verify_password(db, "wrongpassword"), 0);
}

static void test_verify_empty_password(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "something");
    assert_int_equal(auth_verify_password(db, ""), 0);
}

static void test_verify_no_password_set(void **state)
{
    cutils_db_t *db = *state;
    /* No password stored — should return 0 */
    assert_int_equal(auth_verify_password(db, "anything"), 0);
}

/* --- Password change --- */

static void test_change_password(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "oldpass");
    assert_int_equal(auth_verify_password(db, "oldpass"), 1);

    auth_set_password(db, "newpass");
    assert_int_equal(auth_verify_password(db, "oldpass"), 0);
    assert_int_equal(auth_verify_password(db, "newpass"), 1);
}

/* --- Legacy SHA256 hash migration --- */

static void test_legacy_hash_migration(void **state)
{
    cutils_db_t *db = *state;

    /* Insert a legacy SHA256 hash directly (SHA256 of "legacypass") */
    /* We compute it: SHA256("legacypass") and insert the hex */
    /* Rather than hardcoding, let's use the DB to insert a known SHA256 */
    unsigned char hash[32];
    SHA256((const unsigned char *)"legacypass", 10, hash);
    char hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(hex + i * 2, "%02x", hash[i]);
    hex[64] = '\0';

    const char *params[] = { hex, NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO auth (id, password_hash) VALUES (1, ?)",
        params, NULL), CUTILS_OK);

    /* Verify works with legacy hash */
    assert_int_equal(auth_verify_password(db, "legacypass"), 1);

    /* Should have auto-upgraded to PBKDF2 */
    db_result_t *result = NULL;
    assert_int_equal(db_execute(db, "SELECT password_hash FROM auth WHERE id = 1",
               NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_true(result->nrows > 0);
    assert_true(strncmp(result->rows[0][0], "$pbkdf2$", 8) == 0);
    db_result_free(result);

    /* Still verifiable after upgrade */
    assert_int_equal(auth_verify_password(db, "legacypass"), 1);
    assert_int_equal(auth_verify_password(db, "wrongpass"), 0);
}

/* --- Token lifecycle --- */

static void test_create_and_validate_token(void **state)
{
    cutils_db_t *db = *state;

    char *token = auth_create_token(db, 24);
    assert_non_null(token);
    assert_true(strlen(token) > 0);

    assert_int_equal(auth_validate_token(db, token), 1);
    free(token);
}

static void test_validate_bogus_token(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(auth_validate_token(db, "not_a_real_token"), 0);
}

static void test_validate_null_token(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(auth_validate_token(db, NULL), 0);
}

static void test_validate_empty_token(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(auth_validate_token(db, ""), 0);
}

static void test_validate_bearer_prefix(void **state)
{
    cutils_db_t *db = *state;

    char *token = auth_create_token(db, 24);
    assert_non_null(token);

    /* auth_validate_token should strip "Bearer " prefix */
    char bearer[128];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    assert_int_equal(auth_validate_token(db, bearer), 1);

    free(token);
}

static void test_multiple_tokens(void **state)
{
    cutils_db_t *db = *state;

    char *t1 = auth_create_token(db, 24);
    char *t2 = auth_create_token(db, 24);
    assert_non_null(t1);
    assert_non_null(t2);

    /* Both should be valid */
    assert_int_equal(auth_validate_token(db, t1), 1);
    assert_int_equal(auth_validate_token(db, t2), 1);

    /* They should be different tokens */
    assert_true(strcmp(t1, t2) != 0);

    free(t1);
    free(t2);
}

/* --- Expired token --- */

static void test_expired_token_invalid(void **state)
{
    cutils_db_t *db = *state;

    /* Insert a token that expired in the past */
    const char *params[] = { "expired_tok_123", "2020-01-01 00:00:00", NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO sessions (token, expires_at) VALUES (?, ?)",
        params, NULL), CUTILS_OK);

    assert_int_equal(auth_validate_token(db, "expired_tok_123"), 0);
}

/* --- Session cleanup --- */

static void test_cleanup_removes_expired(void **state)
{
    cutils_db_t *db = *state;

    /* Insert an expired session */
    const char *exp_params[] = { "old_token", "2020-01-01 00:00:00", NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO sessions (token, expires_at) VALUES (?, ?)",
        exp_params, NULL), CUTILS_OK);

    /* Insert a valid session */
    char *valid = auth_create_token(db, 24);
    assert_non_null(valid);

    auth_cleanup_sessions(db);

    /* Expired token row should be gone */
    assert_int_equal(auth_validate_token(db, "old_token"), 0);
    /* Valid token should still work */
    assert_int_equal(auth_validate_token(db, valid), 1);

    free(valid);
}

/* --- Malformed PBKDF2 hash strings ---
 * These test the verify_password error paths by injecting bad hashes
 * directly into the DB and verifying they're rejected. */

static void inject_hash(cutils_db_t *db, const char *hash)
{
    /* Ensure row exists first, then update */
    const char *ins_params[] = { hash, NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT OR REPLACE INTO auth (id, password_hash) VALUES (1, ?)",
        ins_params, NULL), CUTILS_OK);
}

static void test_malformed_pbkdf2_missing_fields(void **state)
{
    cutils_db_t *db = *state;
    /* $pbkdf2$ prefix but missing salt and hash fields */
    inject_hash(db, "$pbkdf2$100000");
    assert_int_equal(auth_verify_password(db, "anything"), 0);
}

static void test_malformed_pbkdf2_no_hash(void **state)
{
    cutils_db_t *db = *state;
    /* Has iterations and salt but no hash field */
    inject_hash(db, "$pbkdf2$100000$aabbccdd");
    assert_int_equal(auth_verify_password(db, "anything"), 0);
}

static void test_malformed_pbkdf2_zero_iterations(void **state)
{
    cutils_db_t *db = *state;
    inject_hash(db, "$pbkdf2$0$aabbccdd$eeff0011");
    assert_int_equal(auth_verify_password(db, "anything"), 0);
}

static void test_malformed_pbkdf2_negative_iterations(void **state)
{
    cutils_db_t *db = *state;
    inject_hash(db, "$pbkdf2$-1$aabbccdd$eeff0011");
    assert_int_equal(auth_verify_password(db, "anything"), 0);
}

static void test_malformed_pbkdf2_bad_salt_hex(void **state)
{
    cutils_db_t *db = *state;
    /* Salt contains non-hex characters */
    inject_hash(db, "$pbkdf2$100000$ZZZZZZZZ$aabbccdd");
    assert_int_equal(auth_verify_password(db, "anything"), 0);
}

static void test_malformed_pbkdf2_bad_key_hex(void **state)
{
    cutils_db_t *db = *state;
    /* Valid salt but key contains non-hex characters */
    inject_hash(db, "$pbkdf2$100000$aabbccdd$ZZZZZZZZ");
    assert_int_equal(auth_verify_password(db, "anything"), 0);
}

static void test_malformed_pbkdf2_empty_salt(void **state)
{
    cutils_db_t *db = *state;
    /* Empty salt field */
    inject_hash(db, "$pbkdf2$100000$$aabbccdd");
    assert_int_equal(auth_verify_password(db, "anything"), 0);
}

static void test_malformed_pbkdf2_empty_hash(void **state)
{
    cutils_db_t *db = *state;
    /* Empty hash field — sscanf won't match the third field */
    inject_hash(db, "$pbkdf2$100000$aabbccdd$");
    assert_int_equal(auth_verify_password(db, "anything"), 0);
}

/* --- Legacy hash: wrong password --- */

static void test_legacy_hash_wrong_password(void **state)
{
    cutils_db_t *db = *state;
    /* Insert a legacy SHA256 hash for "rightpass" */
    unsigned char hash[32];
    SHA256((const unsigned char *)"rightpass", 9, hash);
    char hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(hex + i * 2, "%02x", hash[i]);
    hex[64] = '\0';

    inject_hash(db, hex);
    assert_int_equal(auth_verify_password(db, "wrongpass"), 0);
}

/* --- Session cleanup with nothing to clean --- */

static void test_cleanup_nothing_expired(void **state)
{
    cutils_db_t *db = *state;
    /* No sessions at all — cleanup should be a no-op */
    auth_cleanup_sessions(db);

    /* Insert a valid (non-expired) session and cleanup again */
    char *token = auth_create_token(db, 24);
    assert_non_null(token);
    auth_cleanup_sessions(db);
    /* Valid token should survive */
    assert_int_equal(auth_validate_token(db, token), 1);
    free(token);
}

/* --- Sliding expiry / kind-aware validation / revoke --- */

/* Read expires_at for a token. NULL on error. Caller frees. */
static char *get_expires_at(cutils_db_t *db, const char *token)
{
    const char *params[] = { token, NULL };
    db_result_t *result = NULL;
    int rc = db_execute(db, "SELECT expires_at FROM sessions WHERE token = ?",
                        params, &result);
    if (rc != 0 || !result || result->nrows == 0) {
        if (result) db_result_free(result);
        return NULL;
    }
    char *out = strdup(result->rows[0][0]);
    db_result_free(result);
    return out;
}

static void test_session_validation_slides_expiry(void **state)
{
    cutils_db_t *db = *state;

    /* Create a session that expires in 1 hour */
    char *token = auth_create_token(db, 1);
    assert_non_null(token);

    char *before = get_expires_at(db, token);
    assert_non_null(before);

    /* Validate — should slide expires_at to AUTH_SESSION_TTL_HOURS in the
     * future, well past the original 1-hour mark. */
    assert_int_equal(auth_validate_token(db, token), 1);

    char *after = get_expires_at(db, token);
    assert_non_null(after);

    /* String compare on the ISO-8601 timestamp is a valid ordering test
     * (Y-M-D H:M:S with zero-padding sorts lexicographically). */
    assert_true(strcmp(after, before) > 0);

    free(before);
    free(after);
    free(token);
}

static void test_api_token_validation_does_not_slide(void **state)
{
    cutils_db_t *db = *state;

    /* Insert an api_token row directly with kind='api_token' and a
     * known expires_at. */
    const char *params[] = {
        "api_test_tok", "api_token", "2099-12-31 23:59:59", NULL
    };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO sessions (token, kind, expires_at) VALUES (?, ?, ?)",
        params, NULL), 0);

    char *before = get_expires_at(db, "api_test_tok");
    assert_string_equal(before, "2099-12-31 23:59:59");

    /* Validate — api_token rows should NOT have expires_at slid. */
    assert_int_equal(auth_validate_token(db, "api_test_tok"), 1);

    char *after = get_expires_at(db, "api_test_tok");
    assert_string_equal(after, before);

    free(before);
    free(after);
}

static void test_revoke_token_removes_row(void **state)
{
    cutils_db_t *db = *state;

    char *token = auth_create_token(db, 24);
    assert_non_null(token);
    assert_int_equal(auth_validate_token(db, token), 1);

    auth_revoke_token(db, token);

    /* Validation should fail after revoke */
    assert_int_equal(auth_validate_token(db, token), 0);

    free(token);
}

static void test_revoke_token_strips_bearer_prefix(void **state)
{
    cutils_db_t *db = *state;

    char *token = auth_create_token(db, 24);
    assert_non_null(token);

    char bearer[128];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    auth_revoke_token(db, bearer);

    assert_int_equal(auth_validate_token(db, token), 0);
    free(token);
}

static void test_revoke_token_safe_with_null(void **state)
{
    cutils_db_t *db = *state;
    /* Should not crash or affect existing tokens. */
    auth_revoke_token(db, NULL);
    auth_revoke_token(db, "");

    char *token = auth_create_token(db, 24);
    assert_int_equal(auth_validate_token(db, token), 1);
    free(token);
}

static void test_revoked_at_filters_out_session(void **state)
{
    cutils_db_t *db = *state;

    char *token = auth_create_token(db, 24);
    assert_non_null(token);

    /* Soft-delete via revoked_at — validation should fail even though
     * expires_at is still in the future. */
    const char *params[] = { token, NULL };
    assert_int_equal(db_execute_non_query(db,
        "UPDATE sessions SET revoked_at = datetime('now') WHERE token = ?",
        params, NULL), 0);

    assert_int_equal(auth_validate_token(db, token), 0);
    free(token);
}

/* --- Password validation (enforced in auth_set_password) --- */

static void test_set_password_rejects_short(void **state)
{
    cutils_db_t *db = *state;
    assert_true(auth_set_password(db, "abc") != 0);
    assert_true(auth_set_password(db, "ab") != 0);
    assert_true(auth_set_password(db, "a") != 0);
    assert_true(auth_set_password(db, "") != 0);
    /* Should not be marked as setup */
    assert_int_equal(auth_is_setup(db), 0);
}

static void test_set_password_rejects_null(void **state)
{
    cutils_db_t *db = *state;
    assert_true(auth_set_password(db, NULL) != 0);
    assert_int_equal(auth_is_setup(db), 0);
}

static void test_set_password_accepts_minimum(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(auth_set_password(db, "abcd"), 0);
    assert_int_equal(auth_is_setup(db), 1);
    assert_int_equal(auth_verify_password(db, "abcd"), 1);
}

/* --- auth_check middleware --- */

static api_request_t make_request(int is_local, const char *token)
{
    api_request_t req = {0};
    req.is_local = is_local;
    req.auth_token = token;
    return req;
}

static void test_auth_check_local_always_allowed(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "password");

    api_request_t req = make_request(1, NULL);
    assert_int_equal(auth_check(&req, "/api/status", db), 1);
    assert_int_equal(auth_check(&req, "/api/shutdown/groups", db), 1);
}

static void test_auth_check_public_endpoints(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "password");

    api_request_t req = make_request(0, NULL);
    assert_int_equal(auth_check(&req, "/api/auth/setup", db), 1);
    assert_int_equal(auth_check(&req, "/api/auth/login", db), 1);
    assert_int_equal(auth_check(&req, "/api/setup/status", db), 1);
    assert_int_equal(auth_check(&req, "/api/setup/ports", db), 1);
    assert_int_equal(auth_check(&req, "/api/setup/test", db), 1);
}

static void test_auth_check_protected_requires_token(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "password");

    /* No token → denied */
    api_request_t req = make_request(0, NULL);
    assert_int_equal(auth_check(&req, "/api/status", db), 0);
    assert_int_equal(auth_check(&req, "/api/cmd", db), 0);

    /* Valid token → allowed */
    char *token = auth_create_token(db, 24);
    req.auth_token = token;
    assert_int_equal(auth_check(&req, "/api/status", db), 1);
    assert_int_equal(auth_check(&req, "/api/cmd", db), 1);
    free(token);
}

static void test_auth_check_setup_mode_allows_all(void **state)
{
    cutils_db_t *db = *state;
    /* No password set → setup mode → everything allowed */
    api_request_t req = make_request(0, NULL);
    assert_int_equal(auth_check(&req, "/api/status", db), 1);
    assert_int_equal(auth_check(&req, "/api/cmd", db), 1);
    assert_int_equal(auth_check(&req, "/api/shutdown/groups", db), 1);
}

static void test_auth_check_invalid_token_denied(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "password");

    api_request_t req = make_request(0, "bogus_token_123");
    assert_int_equal(auth_check(&req, "/api/status", db), 0);
}

static void test_auth_check_bearer_prefix(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "password");

    char *token = auth_create_token(db, 24);
    char bearer[128];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);

    api_request_t req = make_request(0, bearer);
    assert_int_equal(auth_check(&req, "/api/status", db), 1);
    free(token);
}

/* --- Boundary tightening: public-endpoint allowlist is exact, not prefix --- */

static void test_auth_check_unknown_setup_path_rejected(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "password");

    /* Paths that look like setup endpoints but aren't registered must
     * require auth — the allowlist is exact-match, not prefix-match. */
    api_request_t req = make_request(0, NULL);
    assert_int_equal(auth_check(&req, "/api/setup/foo",    db), 0);
    assert_int_equal(auth_check(&req, "/api/setup/",       db), 0);
    assert_int_equal(auth_check(&req, "/api/setup/statusx", db), 0);
    assert_int_equal(auth_check(&req, "/api/auth/loginx",   db), 0);
}

/* --- Fail-closed behavior: DB errors must not grant access --- */

static void test_auth_check_failsafe_on_db_error(void **state)
{
    cutils_db_t *db = *state;
    auth_set_password(db, "password");

    /* Drop the auth table so auth_is_setup's SELECT fails. auth_is_setup
     * must report "set up" (fail-closed) so auth_check falls through to
     * token validation — which then rejects the request for lack of a
     * valid token. If this returned 1, we'd be fail-open. */
    assert_int_equal(db_exec_raw(db, "DROP TABLE auth"), CUTILS_OK);
    assert_int_equal(auth_is_setup(db), 1);

    api_request_t req = make_request(0, NULL);
    assert_int_equal(auth_check(&req, "/api/status", db), 0);
}

/* --- UPSERT preserves created_at across password changes --- */

static void test_set_password_preserves_created_at(void **state)
{
    cutils_db_t *db = *state;

    /* Seed with a known created_at we can check for equality later. */
    const char *seed_params[] = { "$pbkdf2$100000$aa$bb", "2020-01-01 00:00:00", NULL };
    assert_int_equal(db_execute_non_query(db,
        "INSERT INTO auth (id, password_hash, created_at) VALUES (1, ?, ?)",
        seed_params, NULL), CUTILS_OK);

    assert_int_equal(auth_set_password(db, "newpassword"), 0);

    CUTILS_AUTO_DBRES db_result_t *result = NULL;
    assert_int_equal(db_execute(db,
        "SELECT created_at FROM auth WHERE id = 1", NULL, &result), CUTILS_OK);
    assert_non_null(result);
    assert_true(result->nrows > 0);
    assert_string_equal(result->rows[0][0], "2020-01-01 00:00:00");
}

/* --- auth_init (currently a no-op, but should succeed) --- */

static void test_auth_init(void **state)
{
    cutils_db_t *db = *state;
    assert_int_equal(auth_init(db), 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_not_setup_initially, setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_password_marks_setup, setup, teardown),
        cmocka_unit_test_setup_teardown(test_verify_correct_password, setup, teardown),
        cmocka_unit_test_setup_teardown(test_verify_wrong_password, setup, teardown),
        cmocka_unit_test_setup_teardown(test_verify_empty_password, setup, teardown),
        cmocka_unit_test_setup_teardown(test_verify_no_password_set, setup, teardown),
        cmocka_unit_test_setup_teardown(test_change_password, setup, teardown),
        cmocka_unit_test_setup_teardown(test_legacy_hash_migration, setup, teardown),
        cmocka_unit_test_setup_teardown(test_legacy_hash_wrong_password, setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_and_validate_token, setup, teardown),
        cmocka_unit_test_setup_teardown(test_validate_bogus_token, setup, teardown),
        cmocka_unit_test_setup_teardown(test_validate_null_token, setup, teardown),
        cmocka_unit_test_setup_teardown(test_validate_empty_token, setup, teardown),
        cmocka_unit_test_setup_teardown(test_validate_bearer_prefix, setup, teardown),
        cmocka_unit_test_setup_teardown(test_multiple_tokens, setup, teardown),
        cmocka_unit_test_setup_teardown(test_expired_token_invalid, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cleanup_removes_expired, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cleanup_nothing_expired, setup, teardown),
        /* malformed PBKDF2 hash strings */
        cmocka_unit_test_setup_teardown(test_malformed_pbkdf2_missing_fields, setup, teardown),
        cmocka_unit_test_setup_teardown(test_malformed_pbkdf2_no_hash, setup, teardown),
        cmocka_unit_test_setup_teardown(test_malformed_pbkdf2_zero_iterations, setup, teardown),
        cmocka_unit_test_setup_teardown(test_malformed_pbkdf2_negative_iterations, setup, teardown),
        cmocka_unit_test_setup_teardown(test_malformed_pbkdf2_bad_salt_hex, setup, teardown),
        cmocka_unit_test_setup_teardown(test_malformed_pbkdf2_bad_key_hex, setup, teardown),
        cmocka_unit_test_setup_teardown(test_malformed_pbkdf2_empty_salt, setup, teardown),
        cmocka_unit_test_setup_teardown(test_malformed_pbkdf2_empty_hash, setup, teardown),
        /* sliding expiry / api_token / revoke */
        cmocka_unit_test_setup_teardown(test_session_validation_slides_expiry, setup, teardown),
        cmocka_unit_test_setup_teardown(test_api_token_validation_does_not_slide, setup, teardown),
        cmocka_unit_test_setup_teardown(test_revoke_token_removes_row, setup, teardown),
        cmocka_unit_test_setup_teardown(test_revoke_token_strips_bearer_prefix, setup, teardown),
        cmocka_unit_test_setup_teardown(test_revoke_token_safe_with_null, setup, teardown),
        cmocka_unit_test_setup_teardown(test_revoked_at_filters_out_session, setup, teardown),
        /* password validation */
        cmocka_unit_test_setup_teardown(test_set_password_rejects_short, setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_password_rejects_null, setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_password_accepts_minimum, setup, teardown),
        /* auth_check middleware */
        cmocka_unit_test_setup_teardown(test_auth_check_local_always_allowed, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auth_check_public_endpoints, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auth_check_protected_requires_token, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auth_check_setup_mode_allows_all, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auth_check_invalid_token_denied, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auth_check_bearer_prefix, setup, teardown),
        /* boundary tightening */
        cmocka_unit_test_setup_teardown(test_auth_check_unknown_setup_path_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auth_check_failsafe_on_db_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_password_preserves_created_at, setup, teardown),
        cmocka_unit_test_setup_teardown(test_auth_init, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
