#ifndef API_AUTH_H
#define API_AUTH_H

#include <cutils/db.h>

/* --- Auth system ---
 *
 * Admin user with password hash in DB.
 * Token-based sessions for API access.
 * Setup endpoint for first-run password creation. */

/* Initialize auth system. */
int auth_init(cutils_db_t *db);

/* Check if admin password has been set (first-run detection). */
int auth_is_setup(cutils_db_t *db);

/* Set the admin password (first run or change). Hashes with SHA256. */
int auth_set_password(cutils_db_t *db, const char *password);

/* Verify a password against the stored hash. Returns 1 if valid. */
int auth_verify_password(cutils_db_t *db, const char *password);

/* Create a session token. Returns a malloc'd token string (caller frees).
 * Token is valid for expire_hours. */
char *auth_create_token(cutils_db_t *db, int expire_hours);

/* Validate a session token. Returns 1 if valid and not expired. */
int auth_validate_token(cutils_db_t *db, const char *token);

/* Clean up expired sessions. */
void auth_cleanup_sessions(cutils_db_t *db);

#endif
