/*
 * Minimal vendored copy of the public libbinder_ndk C API used by this HAL.
 * The NDK does not ship android/binder_process.h in its sysroot (see
 * binder_manager.h note). Declarations match AOSP system/core/libbinder_ndk
 * (API 29+).
 */
#pragma once

#include <stdint.h>

#include <android/binder_ibinder.h>

__BEGIN_DECLS

/**
 * Sets the maximum number of threads that the associated threadpool will
 * spawn to service incoming binder transactions.
 */
void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads) __INTRODUCED_IN(29);

/**
 * Blocks to join the associated threadpool, servicing incoming binder
 * transactions. Never returns.
 */
void ABinderProcess_joinThreadPool(void) __INTRODUCED_IN(29);

__END_DECLS
