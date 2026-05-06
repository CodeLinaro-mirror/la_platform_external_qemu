// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file goldfish.h
 * @brief Generic Goldfish tracing backend interface for QEMU.
 *
 * This header defines the structured callback mechanism used by the 'goldfish'
 * tracing backend. This backend allows dynamically loaded plugins to receive
 * trace events from QEMU by registering a callback.
 *
 * Usage:
 * 1. Implement a callback matching the GoldfishTraceCallback signature.
 * 2. During plugin initialization, call qemu_goldfish_set_trace_callback().
 *
 * Event Sequence:
 * For every trace event, the callback is invoked in the following order:
 * - GOLDFISH_TRACE_EVENT_START (name contains the event name)
 * - GOLDFISH_TRACE_ARG_* (zero or more times; name contains the argument name)
 * - GOLDFISH_TRACE_EVENT_END (name contains the event name)
 */

#ifndef TRACE_GOLDFISH_H
#define TRACE_GOLDFISH_H

#include <stdint.h>
#include <stdbool.h>
#include "qemu/osdep.h"

/**
 * @brief Identifies the type of data being passed to the callback.
 */
typedef enum {
    GOLDFISH_TRACE_EVENT_START,      /**< Indicates the start of a trace event. */
    GOLDFISH_TRACE_EVENT_END,        /**< Indicates the end of a trace event. */
    GOLDFISH_TRACE_ARG_BOOL,         /**< Argument is a boolean. */
    GOLDFISH_TRACE_ARG_INT64,        /**< Argument is a signed 64-bit integer. */
    GOLDFISH_TRACE_ARG_UINT64,       /**< Argument is an unsigned 64-bit integer. */
    GOLDFISH_TRACE_ARG_STRING,       /**< Argument is a null-terminated string. */
    GOLDFISH_TRACE_ARG_POINTER,      /**< Argument is a generic pointer. */
    GOLDFISH_TRACE_ARG_STRING_ARRAY, /**< Argument is a null-terminated array of strings (char**). */
} GoldfishTraceType;

/**
 * @brief Structured data passed to the goldfish tracing callback.
 */
typedef struct {
    GoldfishTraceType type; /**< The type of this specific invocation. */
    const char* name;       /**< The name of the event or the argument. */
    union {
        bool b;                   /**< Valid if type == GOLDFISH_TRACE_ARG_BOOL. */
        int64_t i;                /**< Valid if type == GOLDFISH_TRACE_ARG_INT64. */
        uint64_t u;               /**< Valid if type == GOLDFISH_TRACE_ARG_UINT64. */
        const void* p;            /**< Valid if type == GOLDFISH_TRACE_ARG_POINTER. */
        const char* s;            /**< Valid if type == GOLDFISH_TRACE_ARG_STRING. */
        const char* const* sa;    /**< Valid if type == GOLDFISH_TRACE_ARG_STRING_ARRAY. */
    } value;
} GoldfishTraceData;

/**
 * @brief Signature of the callback function to be implemented by tracing plugins.
 * @param data Pointer to the structured trace data. This pointer is only valid
 *             for the duration of the callback.
 */
typedef void (*GoldfishTraceCallback)(const GoldfishTraceData* data);

/**
 * @brief The global callback pointer used by the goldfish backend.
 *
 * This should generally not be modified directly; use
 * qemu_goldfish_set_trace_callback() instead.
 */
extern GoldfishTraceCallback goldfish_trace_callback;

/**
 * @brief Registers a tracing callback with the goldfish backend.
 *
 * This function is exported from the QEMU binary and should be called by
 * plugins during their initialization phase to hook into the tracing system.
 *
 * @param cb The callback function to register, or NULL to disable tracing.
 * @return The previous callback that was registered.
 */
GOLDFISH_EXPORTED_VARIABLE GoldfishTraceCallback qemu_goldfish_set_trace_callback(GoldfishTraceCallback cb);

#endif /* TRACE_GOLDFISH_H */
