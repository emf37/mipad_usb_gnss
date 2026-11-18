/*
 * NDK compatibility shim: <android-base/logging.h> is an AOSP platform header
 * the ported sources include only for ALOG* definitions; those come from the
 * log/log.h shim instead.
 */
#pragma once

#include <log/log.h>
