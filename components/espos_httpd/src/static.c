/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Static UI. The gzipped Vite bundle is built into the app image
 * (ui_bundle.h, CONFIG_ESPOS_HTTPD_UI_EMBED) and served from there, so an OTA
 * update brings the UI that matches its firmware. The LittleFS "storage"
 * partition (mounted at CONFIG_ESPOS_HTTPD_WWW_DIR) comes first only for a
 * custom UI packed into it (ESPOS_HTTPD_UI_FROM_STORAGE), and otherwise
 * answers what the built-in bundle does not have. There `<path>.gz` is tried
 * first and sent with Content-Encoding: gzip. Unknown extension-less paths
 * fall back to index.html (SPA routes). With neither, the placeholder page
 * embedded in the binary is served, so a device is never without a setup page.
 *
 * Wired in as the 404 fallback for GET (see espos_httpd.c) so API handlers
 * registered later by other components are never shadowed by a wildcard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_LINUX
#include "esp_littlefs.h"
#endif

#include "espos_httpd.h"
#include "espos_httpd_priv.h"
#include "ui_bundle.h"

#if !CONFIG_IDF_TARGET_LINUX
static const char *TAG = "espos_httpd";
#endif

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static char s_www[64] = CONFIG_ESPOS_HTTPD_WWW_DIR;
static bool s_mounted;
/* The storage partition is the UI, not the built-in bundle: a project packed
 * its own UI into it, or (host tests) ESPOS_WWW_DIR names one. */
#ifdef ESPOS_HTTPD_UI_FROM_STORAGE
static bool s_storage_first = true;
#else
static bool s_storage_first;
#endif

static const struct {
    const char *ext;
    const char *type;
} TYPES[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".js", "application/javascript; charset=utf-8" },
    { ".mjs", "application/javascript; charset=utf-8" },
    { ".css", "text/css; charset=utf-8" },
    { ".json", "application/json" },
    { ".webmanifest", "application/manifest+json" },
    { ".svg", "image/svg+xml" },
    { ".png", "image/png" },
    { ".ico", "image/x-icon" },
    { ".woff2", "font/woff2" },
    { ".woff", "font/woff" },
    { ".map", "application/json" },
    { ".txt", "text/plain; charset=utf-8" },
};

static const char *content_type(const char *path)
{
    size_t n = strlen(path);
    for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        size_t e = strlen(TYPES[i].ext);
        if (n >= e && strcmp(path + n - e, TYPES[i].ext) == 0) {
            return TYPES[i].type;
        }
    }
    return "application/octet-stream";
}

static esp_err_t send_embedded_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    /* EMBED_TXTFILES appends a NUL terminator; do not send it. */
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static const espos_ui_file_t *embedded_find(const char *logical)
{
    if (s_storage_first) {
        return NULL;
    }
    for (size_t i = 0; i < espos_ui_file_count; i++) {
        if (strcmp(espos_ui_files[i].path, logical) == 0) {
            return &espos_ui_files[i];
        }
    }
    return NULL;
}

static void set_cache(httpd_req_t *req, const char *logical)
{
    /* Vite hashes everything under /assets/: cache forever. The rest can change with the next upload. */
    httpd_resp_set_hdr(req, "Cache-Control", strncmp(logical, "/assets/", 8) == 0 ? "public, max-age=31536000, immutable" : "no-cache");
}

static esp_err_t send_embedded(httpd_req_t *req, const espos_ui_file_t *f)
{
    httpd_resp_set_type(req, content_type(f->path));
    if (f->gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    set_cache(req, f->path);
    return httpd_resp_send(req, (const char *)f->start, f->end - f->start);
}

/* Send <fs path> (already known to exist). */
static esp_err_t send_file(httpd_req_t *req, const char *fs_path, const char *logical, bool gz)
{
    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        return espos_httpd_send_error(req, "500 Internal Server Error", "io", "cannot open file");
    }
    httpd_resp_set_type(req, content_type(logical));
    if (gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    set_cache(req, logical);
    char *buf = malloc(2048);
    if (!buf) {
        fclose(f);
        return espos_httpd_send_error(req, "500 Internal Server Error", "no_mem", "out of memory");
    }
    esp_err_t err = ESP_OK;
    size_t n;
    while (err == ESP_OK && (n = fread(buf, 1, 2048, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, n);
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

static bool is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Try <www><logical>.gz then <www><logical>. */
static bool resolve(const char *logical, char *out, size_t out_size, bool *gz)
{
    if (snprintf(out, out_size, "%s%s.gz", s_www, logical) >= (int)out_size) {
        return false;
    }
    if (is_file(out)) {
        *gz = true;
        return true;
    }
    snprintf(out, out_size, "%s%s", s_www, logical);
    *gz = false;
    return is_file(out);
}

esp_err_t espos_httpd_static_serve(httpd_req_t *req)
{
    char logical[128];
    const char *uri = req->uri;
    size_t n = strcspn(uri, "?#");
    if (n == 0 || uri[0] != '/' || n >= sizeof(logical) - 12) {
        return espos_httpd_send_error(req, "404 Not Found", "not_found", "no such resource");
    }
    memcpy(logical, uri, n);
    logical[n] = '\0';
    if (strstr(logical, "..") || strstr(logical, "//")) {
        return espos_httpd_send_error(req, "404 Not Found", "not_found", "no such resource");
    }
    if (logical[n - 1] == '/') {
        strlcat(logical, "index.html", sizeof(logical));
    }
    const espos_ui_file_t *e = embedded_find(logical);
    if (e) {
        return send_embedded(req, e);
    }
    char fs_path[256];
    bool gz = false;
    if (resolve(logical, fs_path, sizeof(fs_path), &gz)) {
        return send_file(req, fs_path, logical, gz);
    }
    /* No dot in the last segment: an SPA route → index.html. */
    const char *last = strrchr(logical, '/');
    bool routeish = last && !strchr(last, '.');
    if (routeish || strcmp(logical, "/index.html") == 0) {
        e = embedded_find("/index.html");
        if (e) {
            return send_embedded(req, e);
        }
        if (resolve("/index.html", fs_path, sizeof(fs_path), &gz)) {
            return send_file(req, fs_path, "/index.html", gz);
        }
        return send_embedded_index(req);
    }
    return espos_httpd_send_error(req, "404 Not Found", "not_found", "no such resource");
}

static esp_err_t index_get(httpd_req_t *req)
{
    return espos_httpd_static_serve(req);
}

bool espos_httpd_static_mounted(void)
{
    return s_mounted;
}

const char *espos_httpd_ui_source(void)
{
    if (embedded_find("/index.html")) {
        return "embedded";
    }
    return s_mounted ? "storage" : "placeholder";
}

esp_err_t espos_httpd_register_static(void)
{
#if CONFIG_IDF_TARGET_LINUX
    /* Host: serve from a directory named by ESPOS_WWW_DIR (tests). */
    const char *dir = getenv("ESPOS_WWW_DIR");
    if (dir && *dir) {
        strlcpy(s_www, dir, sizeof(s_www));
        s_mounted = true;
        s_storage_first = true;
    }
#else
    esp_vfs_littlefs_conf_t conf = {
        .base_path = s_www,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_OK) {
        size_t total = 0, used = 0;
        esp_littlefs_info(conf.partition_label, &total, &used);
        /* Mounted but empty is the common miss: the partition was formatted
         * on first boot because the build never packed a UI image. Say so
         * loudly and report ui_storage=false, or "mounted" reads as "the UI
         * is there" while the device serves the embedded placeholder. */
        char index[80];
        snprintf(index, sizeof(index), "%s/index.html.gz", s_www);
        bool have_index = is_file(index);
        if (!have_index) {
            snprintf(index, sizeof(index), "%s/index.html", s_www);
            have_index = is_file(index);
        }
        if (have_index) {
            ESP_LOGI(TAG, "ui storage mounted at %s (%u/%u KiB used)", s_www, (unsigned)(used / 1024), (unsigned)(total / 1024));
            s_mounted = true;
        } else if (!embedded_find("/index.html")) {
            ESP_LOGW(TAG, "ui storage at %s has no index.html: serving the embedded page "
                          "(build the UI: cd ui && npm ci && npm run build)",
                     s_www);
        }
    } else if (!embedded_find("/index.html")) {
        ESP_LOGW(TAG, "ui storage not mounted (%s): serving the embedded page", esp_err_to_name(err));
    }
    if (embedded_find("/index.html")) {
        ESP_LOGI(TAG, "serving the web UI built into the firmware (%u files)", (unsigned)espos_ui_file_count);
    }
#endif
    /* The UI is public: the login page is part of it. (The 404 fallback that
     * serves the rest of the bundle never goes through a handler at all.) */
    static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = index_get };
    return espos_httpd_register_ex(&root, ESPOS_HTTPD_PUBLIC);
}
