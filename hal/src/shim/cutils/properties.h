/*
 * NDK compatibility shim: <cutils/properties.h> subset backed by the public
 * bionic <sys/system_properties.h>. property_set() here is synchronous
 * (__system_property_set), which is what libcutils does as well.
 */
#pragma once

#include <sys/system_properties.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int property_get(const char* key, char* value, const char* default_value) {
    int len = __system_property_get(key, value);
    if (len > 0) return len;
    if (default_value != NULL) {
        strncpy(value, default_value, PROP_VALUE_MAX - 1);
        value[PROP_VALUE_MAX - 1] = '\0';
        return (int)strlen(value);
    }
    value[0] = '\0';
    return 0;
}

static inline int property_set(const char* key, const char* value) {
    return __system_property_set(key, value);
}

#ifdef __cplusplus
}
#endif
