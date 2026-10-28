// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Stand-in librutabaga_ffi DSO for rutabaga_ffi_lazy unit tests.

#define RUTABAGA_GFX_FFI_UNSTABLE 1
#include "rutabaga_gfx/rutabaga_gfx_ffi.h"
#include "rutabaga_gfx/rutabaga_gfx_ffi_goldfish.h"

#include <stddef.h>
#include <string.h>

static int s_fake_instance_cookie = 0x52555441;

int32_t rutabaga_calculate_capset_mask(const char* capset_names, uint64_t* capset_mask) {
    if (!capset_names || !capset_mask) {
        return -22;
    }
    *capset_mask = (uint64_t)strlen(capset_names);
    return 0;
}

int32_t rutabaga_init(const struct rutabaga_builder* builder, struct rutabaga** ptr) {
    (void)builder;
    if (!ptr) {
        return -22;
    }
    *ptr = (struct rutabaga*)&s_fake_instance_cookie;
    return 0;
}

int32_t rutabaga_finish(struct rutabaga** ptr) {
    if (!ptr || *ptr != (struct rutabaga*)&s_fake_instance_cookie) {
        return -22;
    }
    *ptr = NULL;
    return 0;
}

int32_t rutabaga_get_num_capsets(struct rutabaga* ptr, uint32_t* num_capsets) {
    if (ptr != (struct rutabaga*)&s_fake_instance_cookie || !num_capsets) {
        return -22;
    }
    *num_capsets = 3;
    return 0;
}

int32_t rutabaga_get_capset_info(struct rutabaga* ptr, uint32_t capset_index, uint32_t* capset_id,
                                 uint32_t* capset_version, uint32_t* capset_size) {
    (void)ptr;
    *capset_id = capset_index + 1;
    *capset_version = 1;
    *capset_size = 64;
    return 0;
}

int32_t rutabaga_get_capset(struct rutabaga* ptr, uint32_t capset_id, uint32_t version,
                            uint8_t* capset, uint32_t capset_size) {
    (void)ptr;
    (void)capset_id;
    (void)version;
    if (capset && capset_size > 0) {
        memset(capset, 0xAB, capset_size);
    }
    return 0;
}

int32_t rutabaga_context_create(struct rutabaga* ptr, uint32_t ctx_id, uint32_t context_init,
                                const char* context_name, uint32_t context_name_len) {
    (void)ptr;
    (void)ctx_id;
    (void)context_init;
    (void)context_name;
    (void)context_name_len;
    return 0;
}

int32_t rutabaga_context_destroy(struct rutabaga* ptr, uint32_t ctx_id) {
    (void)ptr;
    (void)ctx_id;
    return 0;
}

int32_t rutabaga_context_attach_resource(struct rutabaga* ptr, uint32_t ctx_id,
                                         uint32_t resource_id) {
    (void)ptr;
    (void)ctx_id;
    (void)resource_id;
    return 0;
}

int32_t rutabaga_context_detach_resource(struct rutabaga* ptr, uint32_t ctx_id,
                                         uint32_t resource_id) {
    (void)ptr;
    (void)ctx_id;
    (void)resource_id;
    return 0;
}

int32_t rutabaga_resource_create_3d(struct rutabaga* ptr, uint32_t resource_id,
                                    const struct rutabaga_create_3d* create_3d) {
    (void)ptr;
    (void)resource_id;
    (void)create_3d;
    return 0;
}

int32_t rutabaga_resource_attach_backing(struct rutabaga* ptr, uint32_t resource_id,
                                         const struct rutabaga_iovecs* iovecs) {
    (void)ptr;
    (void)resource_id;
    (void)iovecs;
    return 0;
}

int32_t rutabaga_resource_detach_backing(struct rutabaga* ptr, uint32_t resource_id) {
    (void)ptr;
    (void)resource_id;
    return 0;
}

int32_t rutabaga_resource_transfer_read(struct rutabaga* ptr, uint32_t ctx_id, uint32_t resource_id,
                                        const struct rutabaga_transfer* transfer,
                                        const struct iovec* iovec) {
    (void)ptr;
    (void)ctx_id;
    (void)resource_id;
    (void)transfer;
    (void)iovec;
    return 0;
}

int32_t rutabaga_resource_transfer_write(struct rutabaga* ptr, uint32_t ctx_id,
                                         uint32_t resource_id,
                                         const struct rutabaga_transfer* transfer) {
    (void)ptr;
    (void)ctx_id;
    (void)resource_id;
    (void)transfer;
    return 0;
}

int32_t rutabaga_resource_create_blob(struct rutabaga* ptr, uint32_t ctx_id, uint32_t resource_id,
                                      const struct rutabaga_create_blob* create_blob,
                                      const struct rutabaga_iovecs* iovecs,
                                      const struct rutabaga_handle* handle) {
    (void)ptr;
    (void)ctx_id;
    (void)resource_id;
    (void)create_blob;
    (void)iovecs;
    (void)handle;
    return 0;
}

int32_t rutabaga_resource_unref(struct rutabaga* ptr, uint32_t resource_id) {
    (void)ptr;
    (void)resource_id;
    return 0;
}

int32_t rutabaga_resource_export_blob(struct rutabaga* ptr, uint32_t resource_id,
                                      struct rutabaga_handle* handle) {
    (void)ptr;
    (void)resource_id;
    (void)handle;
    return 0;
}

int32_t rutabaga_resource_map(struct rutabaga* ptr, uint32_t resource_id,
                              struct rutabaga_mapping* mapping) {
    (void)ptr;
    (void)resource_id;
    (void)mapping;
    return 0;
}

int32_t rutabaga_resource_unmap(struct rutabaga* ptr, uint32_t resource_id) {
    (void)ptr;
    (void)resource_id;
    return 0;
}

int32_t rutabaga_resource_map_info(struct rutabaga* ptr, uint32_t resource_id, uint32_t* map_info) {
    (void)ptr;
    (void)resource_id;
    (void)map_info;
    return 0;
}

int32_t rutabaga_submit_command(struct rutabaga* ptr, struct rutabaga_command* cmd) {
    (void)ptr;
    (void)cmd;
    return 0;
}

int32_t rutabaga_create_fence(struct rutabaga* ptr, const struct rutabaga_fence* fence) {
    (void)ptr;
    (void)fence;
    return 0;
}

int32_t rutabaga_snapshot(struct rutabaga* ptr, const char* dir) {
    (void)ptr;
    (void)dir;
    return 0;
}

int32_t rutabaga_restore(struct rutabaga* ptr, const char* dir) {
    (void)ptr;
    (void)dir;
    return 0;
}

#if !defined(RUTABAGA_FAKE_OMIT_REQUIRED_SYMBOL)
int32_t rutabaga_resource_import(struct rutabaga* ptr, uint32_t resource_id,
                                 const struct rutabaga_handle* import_handle,
                                 const struct rutabaga_import_data* import_data) {
    (void)ptr;
    (void)resource_id;
    (void)import_handle;
    (void)import_data;
    return 0;
}
#endif

#if !defined(RUTABAGA_FAKE_OMIT_GOLDFISH_SYMBOL)
int32_t rutabaga_resource_transfer_write_goldfish(struct rutabaga* ptr, uint32_t ctx_id,
                                                  uint32_t resource_id,
                                                  const struct rutabaga_transfer* transfer,
                                                  const struct iovec* iovec) {
    (void)ptr;
    (void)transfer;
    (void)iovec;
    return (int32_t)(ctx_id + resource_id);
}
#endif
