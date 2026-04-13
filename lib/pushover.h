#ifndef PUSHOVER_H
#define PUSHOVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef struct pushover_request {
  const char *token;
  const char *user;
  const char *message;
  const char *title;
  const char *url;
  const char *url_title;
  const char *device;
  const char *sound;
  int priority;   /* -2..2 */
  int timestamp;  /* unix timestamp */
  int ttl;        /* seconds, >0 */
  int retry;      /* for priority 2 */
  int expire;     /* for priority 2 */
} pushover_request;

typedef struct pushover_client {
  const char *token;
  const char *user;
  const char *device;
  const char *sound;
  int priority;   /* -2..2 */
  int timestamp;  /* unix timestamp */
  int ttl;        /* seconds, >0 */
  int retry;      /* for priority 2 */
  int expire;     /* for priority 2 */
} pushover_client;

typedef enum pushover_status {
  PUSHOVER_OK = 0,
  PUSHOVER_E_INVALID = -1,
  PUSHOVER_E_CURL_INIT = -2,
  PUSHOVER_E_MIME = -3,
  PUSHOVER_E_CURL = -4,
  PUSHOVER_E_HTTP = -5,
  PUSHOVER_E_CURL_GLOBAL = -6,
  PUSHOVER_E_SPLIT = -7
} pushover_status;

typedef struct pushover_result {
  long http_status;
  int curl_code;
  char error[256];
} pushover_result;

int pushover_global_init(void);
void pushover_global_cleanup(void);

void pushover_client_init(pushover_client *client);
int pushover_client_set_auth(pushover_client *client, const char *token, const char *user);
void pushover_client_set_device(pushover_client *client, const char *device);
void pushover_client_set_sound(pushover_client *client, const char *sound);
void pushover_client_set_priority(pushover_client *client, int priority);
void pushover_client_set_timestamp(pushover_client *client, int timestamp);
void pushover_client_set_ttl(pushover_client *client, int ttl);
void pushover_client_set_retry_expire(pushover_client *client, int retry, int expire);

pushover_status pushover_send(const pushover_request *req, char *response, size_t response_cap,
                              pushover_result *result);
pushover_status pushover_send_client(const pushover_client *client, const char *title,
                                     const char *message, const char *url, const char *url_title,
                                     char *response, size_t response_cap,
                                     pushover_result *result);
pushover_status pushover_alert(const pushover_client *client, const char *title,
                               const char *message);

#ifdef __cplusplus
}
#endif

#endif /* PUSHOVER_H */

#ifdef PUSHOVER_IMPLEMENTATION

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <curl/curl.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif

#ifndef PUSHOVER_MESSAGE_MAX
#define PUSHOVER_MESSAGE_MAX 1024
#endif
#ifndef PUSHOVER_TITLE_MAX
#define PUSHOVER_TITLE_MAX 250
#endif
#ifndef PUSHOVER_UTF8_MAX_BYTES
#define PUSHOVER_UTF8_MAX_BYTES(chars) ((chars) * 4)
#endif
#ifndef PUSHOVER_URL_MAX
#define PUSHOVER_URL_MAX 512
#endif
#ifndef PUSHOVER_URL_TITLE_MAX
#define PUSHOVER_URL_TITLE_MAX 100
#endif

typedef struct pushover_buf {
  char *data;
  size_t cap;
  size_t len;
} pushover_buf;

#if defined(_WIN32)
static INIT_ONCE pushover_once = INIT_ONCE_STATIC_INIT;
static int pushover_init_result = -1;

static BOOL CALLBACK pushover_init_once_cb(PINIT_ONCE InitOnce, PVOID Parameter,
                                           PVOID *Context) {
  (void)InitOnce;
  (void)Parameter;
  (void)Context;
  pushover_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  return pushover_init_result == 0;
}
#elif defined(__unix__) || defined(__APPLE__)
static pthread_once_t pushover_once = PTHREAD_ONCE_INIT;
static int pushover_init_result = -1;

static void pushover_init_once_func(void) {
  pushover_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}
#else
static int pushover_init_result = -1;
#endif

static size_t pushover_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  pushover_buf *buf = (pushover_buf *)userdata;
  size_t n = size * nmemb;

  if (!buf || !buf->data || buf->cap == 0) {
    return n;
  }

  if (buf->len + n >= buf->cap) {
    n = buf->cap - buf->len - 1;
  }

  if (n > 0) {
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
  }

  return size * nmemb;
}

static int pushover_global_acquire(void) {
#if defined(_WIN32)
  if (!InitOnceExecuteOnce(&pushover_once, pushover_init_once_cb, NULL, NULL)) {
    return -1;
  }
  return pushover_init_result == 0 ? 0 : -1;
#elif defined(__unix__) || defined(__APPLE__)
  pthread_once(&pushover_once, pushover_init_once_func);
  return pushover_init_result == 0 ? 0 : -1;
#else
  if (pushover_init_result == -1) {
    pushover_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  }
  return pushover_init_result == 0 ? 0 : -1;
#endif
}

static void pushover_global_release(void) {
  /* No-op: keep libcurl initialized for process lifetime for thread safety. */
}

int pushover_global_init(void) {
  return pushover_global_acquire();
}

void pushover_global_cleanup(void) {
  pushover_global_release();
}

static int pushover_add_str_part(CURL *curl, curl_mime *mime, const char *name,
                                 const char *value) {
  if (!value || !*value) {
    return 0;
  }
  curl_mimepart *part = curl_mime_addpart(mime);
  if (!part) {
    return -1;
  }
  curl_mime_name(part, name);
  curl_mime_data(part, value, CURL_ZERO_TERMINATED);
  return 0;
}

static int pushover_add_int_part(CURL *curl, curl_mime *mime, const char *name, int value) {
  char buf[32];
  if (value == 0) {
    return 0;
  }
  snprintf(buf, sizeof(buf), "%d", value);
  return pushover_add_str_part(curl, mime, name, buf);
}

static void pushover_set_error(pushover_result *result, const char *msg) {
  if (!result || !msg) {
    return;
  }
  strncpy(result->error, msg, sizeof(result->error) - 1);
  result->error[sizeof(result->error) - 1] = '\0';
}

static void pushover_clear_error(pushover_result *result) {
  if (result) {
    result->error[0] = '\0';
  }
}

static void pushover_set_errorf(pushover_result *result, const char *fmt, ...) {
  va_list args;
  if (!result || !fmt) {
    return;
  }
  va_start(args, fmt);
  vsnprintf(result->error, sizeof(result->error), fmt, args);
  va_end(args);
}

static size_t pushover_utf8_next(const char *s, size_t i, size_t len) {
  size_t remaining = len - i;
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80) {
    return 1;
  }
  if ((c & 0xE0) == 0xC0 && remaining >= 2) {
    unsigned char c1 = (unsigned char)s[i + 1];
    return (c1 & 0xC0) == 0x80 ? 2 : 1;
  }
  if ((c & 0xF0) == 0xE0 && remaining >= 3) {
    unsigned char c1 = (unsigned char)s[i + 1];
    unsigned char c2 = (unsigned char)s[i + 2];
    return ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) ? 3 : 1;
  }
  if ((c & 0xF8) == 0xF0 && remaining >= 4) {
    unsigned char c1 = (unsigned char)s[i + 1];
    unsigned char c2 = (unsigned char)s[i + 2];
    unsigned char c3 = (unsigned char)s[i + 3];
    return ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80)
               ? 4
               : 1;
  }
  return 1;
}

static int pushover_utf8_within_limit(const char *s, size_t max_chars) {
  size_t i = 0;
  size_t count = 0;
  size_t len;
  if (!s) {
    return 1;
  }
  len = strlen(s);
  while (s[i]) {
    if (count >= max_chars) {
      return 0;
    }
    i += pushover_utf8_next(s, i, len);
    count++;
  }
  return 1;
}

static size_t pushover_utf8_trunc_bytes(const char *s, size_t max_chars) {
  size_t i = 0;
  size_t count = 0;
  size_t len;
  if (!s) {
    return 0;
  }
  len = strlen(s);
  while (s[i] && count < max_chars) {
    i += pushover_utf8_next(s, i, len);
    count++;
  }
  return i;
}

static size_t pushover_utf8_chunk_len(const char *s, size_t max_chars) {
  size_t i = 0;
  size_t count = 0;
  size_t last_nl = 0;
  size_t len;
  if (!s || !*s) {
    return 0;
  }
  len = strlen(s);
  while (s[i] && count < max_chars) {
    if (s[i] == '\n') {
      last_nl = i;
    }
    i += pushover_utf8_next(s, i, len);
    count++;
  }
  if (!s[i]) {
    return i;
  }
  if (last_nl > 0) {
    return last_nl;
  }
  return i;
}

static void pushover_utf8_copy_trunc(char *dst, size_t cap, const char *src,
                                     size_t max_chars) {
  size_t len;
  if (!dst || cap == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  len = pushover_utf8_trunc_bytes(src, max_chars);
  if (len >= cap) {
    len = cap - 1;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static void pushover_build_split_title(char *dst, size_t cap, const char *title,
                                       int part, int total, size_t max_chars) {
  char suffix[32];
  size_t suffix_chars;
  size_t avail_chars;
  size_t title_bytes = 0;

  if (!dst || cap == 0) {
    return;
  }

  snprintf(suffix, sizeof(suffix), " (%d/%d)", part, total);
  suffix_chars = strlen(suffix);
  if (suffix_chars >= max_chars) {
    pushover_utf8_copy_trunc(dst, cap, suffix, max_chars);
    return;
  }

  avail_chars = max_chars - suffix_chars;
  if (title && *title) {
    title_bytes = pushover_utf8_trunc_bytes(title, avail_chars);
    if (title_bytes >= cap) {
      title_bytes = cap - 1;
    }
    memcpy(dst, title, title_bytes);
  }

  if (title_bytes < cap - 1) {
    size_t suffix_len = strlen(suffix);
    if (title_bytes + suffix_len >= cap) {
      suffix_len = cap - title_bytes - 1;
    }
    memcpy(dst + title_bytes, suffix, suffix_len);
    dst[title_bytes + suffix_len] = '\0';
  } else {
    dst[cap - 1] = '\0';
  }
}

void pushover_client_init(pushover_client *client) {
  if (!client) {
    return;
  }
  client->token = NULL;
  client->user = NULL;
  client->device = NULL;
  client->sound = NULL;
  client->priority = 0;
  client->timestamp = 0;
  client->ttl = 0;
  client->retry = 60;
  client->expire = 600;
}

int pushover_client_set_auth(pushover_client *client, const char *token, const char *user) {
  if (!client || !token || !user) {
    return -1;
  }
  client->token = token;
  client->user = user;
  return 0;
}

void pushover_client_set_device(pushover_client *client, const char *device) {
  if (client) {
    client->device = device;
  }
}

void pushover_client_set_sound(pushover_client *client, const char *sound) {
  if (client) {
    client->sound = sound;
  }
}

void pushover_client_set_priority(pushover_client *client, int priority) {
  if (!client) {
    return;
  }
  if (priority < -2) {
    priority = -2;
  } else if (priority > 2) {
    priority = 2;
  }
  client->priority = priority;
}

void pushover_client_set_timestamp(pushover_client *client, int timestamp) {
  if (client) {
    client->timestamp = timestamp;
  }
}

void pushover_client_set_ttl(pushover_client *client, int ttl) {
  if (!client) {
    return;
  }
  if (ttl < 0) {
    ttl = 0;
  }
  client->ttl = ttl;
}

void pushover_client_set_retry_expire(pushover_client *client, int retry, int expire) {
  if (!client) {
    return;
  }
  if (retry <= 0 || expire <= 0) {
    client->retry = 0;
    client->expire = 0;
    return;
  }
  if (retry < 30) {
    retry = 30;
  }
  if (expire > 10800) {
    expire = 10800;
  }
  client->retry = retry;
  client->expire = expire;
}

pushover_status pushover_send(const pushover_request *req, char *response, size_t response_cap,
                              pushover_result *result) {
  CURL *curl = NULL;
  curl_mime *mime = NULL;
  CURLcode code;
  pushover_buf buf;
  long http_status = 0;

  if (result) {
    result->http_status = 0;
    result->curl_code = 0;
    pushover_clear_error(result);
  }

  if (!req || !req->token || !*req->token || !req->user || !*req->user ||
      !req->message || !*req->message) {
    pushover_set_error(result, "invalid request: token, user, and message are required");
    return PUSHOVER_E_INVALID;
  }
  if (req->priority < -2 || req->priority > 2) {
    pushover_set_error(result, "invalid priority");
    return PUSHOVER_E_INVALID;
  }
  if (!pushover_utf8_within_limit(req->message, PUSHOVER_MESSAGE_MAX)) {
    pushover_set_error(result, "message too long");
    return PUSHOVER_E_INVALID;
  }
  if (req->title && !pushover_utf8_within_limit(req->title, PUSHOVER_TITLE_MAX)) {
    pushover_set_error(result, "title too long");
    return PUSHOVER_E_INVALID;
  }
  if (req->url && strlen(req->url) > PUSHOVER_URL_MAX) {
    pushover_set_error(result, "url too long");
    return PUSHOVER_E_INVALID;
  }
  if (req->url_title && !pushover_utf8_within_limit(req->url_title, PUSHOVER_URL_TITLE_MAX)) {
    pushover_set_error(result, "url title too long");
    return PUSHOVER_E_INVALID;
  }
  if (req->ttl < 0) {
    pushover_set_error(result, "invalid ttl");
    return PUSHOVER_E_INVALID;
  }
  if (req->priority == 2) {
    if (req->retry < 30 || req->expire <= 0 || req->expire > 10800) {
      pushover_set_error(result, "invalid retry/expire for emergency priority");
      return PUSHOVER_E_INVALID;
    }
  }

  if (pushover_global_acquire() != 0) {
    pushover_set_error(result, "curl global init failed");
    return PUSHOVER_E_CURL_GLOBAL;
  }

  curl = curl_easy_init();
  if (!curl) {
    pushover_global_release();
    pushover_set_error(result, "curl init failed");
    return PUSHOVER_E_CURL_INIT;
  }

  mime = curl_mime_init(curl);
  if (!mime) {
    curl_easy_cleanup(curl);
    pushover_global_release();
    pushover_set_error(result, "curl mime init failed");
    return PUSHOVER_E_MIME;
  }

  pushover_add_str_part(curl, mime, "token", req->token);
  pushover_add_str_part(curl, mime, "user", req->user);
  pushover_add_str_part(curl, mime, "message", req->message);
  pushover_add_str_part(curl, mime, "title", req->title);
  pushover_add_str_part(curl, mime, "url", req->url);
  pushover_add_str_part(curl, mime, "url_title", req->url_title);
  pushover_add_str_part(curl, mime, "device", req->device);
  pushover_add_str_part(curl, mime, "sound", req->sound);
  pushover_add_int_part(curl, mime, "priority", req->priority);
  pushover_add_int_part(curl, mime, "timestamp", req->timestamp);
  pushover_add_int_part(curl, mime, "ttl", req->ttl);
  pushover_add_int_part(curl, mime, "retry", req->retry);
  pushover_add_int_part(curl, mime, "expire", req->expire);

  buf.data = response;
  buf.cap = response_cap;
  buf.len = 0;
  if (buf.data && buf.cap > 0) {
    buf.data[0] = '\0';
  }

  curl_easy_setopt(curl, CURLOPT_URL, "https://api.pushover.net/1/messages.json");
  curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, pushover_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "push-c/1.0");

  code = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

  curl_mime_free(mime);
  curl_easy_cleanup(curl);
  pushover_global_release();

  if (result) {
    result->http_status = http_status;
    result->curl_code = (int)code;
  }

  if (code != CURLE_OK) {
    pushover_set_error(result, curl_easy_strerror(code));
    return PUSHOVER_E_CURL;
  }
  if (http_status < 200 || http_status >= 300) {
    pushover_set_errorf(result, "http request failed (status %ld)", http_status);
    return PUSHOVER_E_HTTP;
  }

  return PUSHOVER_OK;
}

pushover_status pushover_send_client(const pushover_client *client, const char *title,
                                     const char *message, const char *url, const char *url_title,
                                     char *response, size_t response_cap,
                                     pushover_result *result) {
  pushover_request req;
  int is_short;
  char msg_buf[PUSHOVER_UTF8_MAX_BYTES(PUSHOVER_MESSAGE_MAX) + 1];
  char title_buf[PUSHOVER_UTF8_MAX_BYTES(PUSHOVER_TITLE_MAX) + 1];
  const char *title_use = title;

  if (!client || !client->token || !*client->token || !client->user || !*client->user ||
      !message || !*message) {
    pushover_set_error(result, "invalid client: token, user, and message are required");
    return PUSHOVER_E_INVALID;
  }
  if (client->priority < -2 || client->priority > 2) {
    pushover_set_error(result, "invalid priority");
    return PUSHOVER_E_INVALID;
  }

  if (url && strlen(url) > PUSHOVER_URL_MAX) {
    pushover_set_error(result, "url too long");
    return PUSHOVER_E_INVALID;
  }
  if (url_title && !pushover_utf8_within_limit(url_title, PUSHOVER_URL_TITLE_MAX)) {
    pushover_set_error(result, "url title too long");
    return PUSHOVER_E_INVALID;
  }
  if (client->ttl < 0) {
    pushover_set_error(result, "invalid ttl");
    return PUSHOVER_E_INVALID;
  }
  if (client->priority == 2) {
    if (client->retry < 30 || client->expire <= 0 || client->expire > 10800) {
      pushover_set_error(result, "invalid retry/expire for emergency priority");
      return PUSHOVER_E_INVALID;
    }
  }

  if (title && !pushover_utf8_within_limit(title, PUSHOVER_TITLE_MAX)) {
    pushover_utf8_copy_trunc(title_buf, sizeof(title_buf), title, PUSHOVER_TITLE_MAX);
    title_use = title_buf;
  }

  is_short = pushover_utf8_within_limit(message, PUSHOVER_MESSAGE_MAX);
  if (is_short) {
    req.token = client->token;
    req.user = client->user;
    req.message = message;
    req.title = title_use;
    req.url = url;
    req.url_title = url_title;
    req.device = client->device;
    req.sound = client->sound;
    req.priority = client->priority;
    req.timestamp = client->timestamp;
    req.ttl = client->priority == 2 ? 0 : client->ttl;
    if (client->priority == 2) {
      req.retry = client->retry;
      req.expire = client->expire;
    } else {
      req.retry = 0;
      req.expire = 0;
    }

    return pushover_send(&req, response, response_cap, result);
  }

  /* Split large messages: prefer last newline before limit, otherwise hard split. */
  {
    const char *p = message;
    int total_parts = 0;
    int part = 0;

    while (*p) {
      size_t chunk_len;

      chunk_len = pushover_utf8_chunk_len(p, PUSHOVER_MESSAGE_MAX);
      if (chunk_len == 0) {
        pushover_set_error(result, "split error");
        return PUSHOVER_E_SPLIT;
      }

      total_parts++;
      p += chunk_len;
      if (*p == '\n') {
        p++;
      }
    }

    p = message;
    while (*p) {
      size_t chunk_len;
      int rc;

      chunk_len = pushover_utf8_chunk_len(p, PUSHOVER_MESSAGE_MAX);
      if (chunk_len == 0) {
        pushover_set_error(result, "split error");
        return PUSHOVER_E_SPLIT;
      }

      memcpy(msg_buf, p, chunk_len);
      msg_buf[chunk_len] = '\0';

      part++;
      pushover_build_split_title(title_buf, sizeof(title_buf), title, part, total_parts,
                                 PUSHOVER_TITLE_MAX);

      req.token = client->token;
      req.user = client->user;
      req.message = msg_buf;
      req.title = title_buf;
      req.url = url;
      req.url_title = url_title;
      req.device = client->device;
      req.sound = client->sound;
      req.priority = client->priority;
      req.timestamp = client->timestamp;
      req.ttl = client->priority == 2 ? 0 : client->ttl;
      if (client->priority == 2) {
        req.retry = client->retry;
        req.expire = client->expire;
      } else {
        req.retry = 0;
        req.expire = 0;
      }

      rc = pushover_send(&req, response, response_cap, result);
      if (rc != PUSHOVER_OK) {
        return rc;
      }

      p += chunk_len;
      if (*p == '\n') {
        p++;
      }
    }
  }

  return PUSHOVER_OK;
}

pushover_status pushover_alert(const pushover_client *client, const char *title,
                               const char *message) {
  return pushover_send_client(client, title, message, NULL, NULL, NULL, 0, NULL);
}

#endif /* PUSHOVER_IMPLEMENTATION */
