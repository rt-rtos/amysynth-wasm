#pragma once
/* Host shim: no IDF on the host. */
static inline const char *esp_get_idf_version(void) { return "host"; }
