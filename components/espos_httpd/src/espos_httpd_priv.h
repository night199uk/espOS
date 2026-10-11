/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "espos_httpd.h"

/* Built-in endpoints; each registers through espos_httpd_register_ex() once
 * the server exists, so the auth trampoline covers them too. */
esp_err_t espos_httpd_register_config_api(void);
esp_err_t espos_httpd_register_system_api(void);
esp_err_t espos_httpd_register_static(void);
esp_err_t espos_httpd_static_serve(httpd_req_t *req);
bool espos_httpd_static_mounted(void);
/* Where GET / comes from: "embedded" (built into the app), "storage" or "placeholder". */
const char *espos_httpd_ui_source(void);
esp_err_t espos_httpd_register_logs_api(void);
esp_err_t espos_httpd_register_coredump_api(void);
esp_err_t espos_httpd_register_health_api(void);
esp_err_t espos_httpd_register_sse(void);
void espos_httpd_sse_shutdown(void);
esp_err_t espos_httpd_register_auth_api(void);

/* auth.c: load the key and session lifetime from config (before the first
 * handler is registered), follow their changes, and the check the trampoline
 * runs for a protected endpoint — false means a 401/403/429 has been sent. */
esp_err_t espos_httpd_auth_init(void);
void espos_httpd_auth_config_changed(const char *ns, const char *key);
bool espos_httpd_auth_enforce(httpd_req_t *req);
/* A credential is needed for protected endpoints (a key is set, or the build requires one). */
bool espos_httpd_auth_required(void);

/* true once a reboot/factory-reset has been accepted; writes are refused. */
bool espos_httpd_restart_pending(void);
