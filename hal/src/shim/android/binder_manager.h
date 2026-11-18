/*
 * Minimal vendored copy of the public libbinder_ndk C API used by this HAL.
 * The NDK does not ship android/binder_manager.h in its sysroot (libbinder_ndk
 * is not an official NDK library), while the import stub libbinder_ndk.so IS
 * provided. Declarations match AOSP system/core/libbinder_ndk (API 29+).
 */
#pragma once

#include <android/binder_ibinder.h>

__BEGIN_DECLS

/**
 * Registers a binder object with the service manager under the given instance
 * name, e.g. "android.hardware.gnss.IGnss/default".
 *
 * \param binder the binder object to register.
 * \param instance the NUL-terminated instance name.
 *
 * \return STATUS_OK on success.
 */
binder_status_t AServiceManager_addService(AIBinder* binder, const char* instance)
        __INTRODUCED_IN(29);

__END_DECLS
