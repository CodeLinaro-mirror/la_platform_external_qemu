/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "trace.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "hw/virtio/virtio-iommu.h"
#include "migration/qemu-file-types.h"
#include "migration/snapshot.h"
#include "monitor/monitor.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "ui/console.h"

#include <glib/gmem.h>
#include <glib/gstdio.h>
#include <rutabaga_gfx/rutabaga_gfx_ffi.h>

/*
 * 1 -> 2: the renderer state directory is keyed by snapshot name
 *         (see virtio_gpu_rutabaga_renderer_dir()); the per-save
 *         "rutabaga-<timestamp>" id string is no longer part of the stream.
 */
/* 2 -> 3: in-flight fenced commands follow the resource list. */
#define VIRTIO_GPU_RUTABAGA_VM_VERSION 3

#define CHECK(condition, cmd)                                                 \
    do {                                                                      \
        if (!(condition)) {                                                   \
            error_report("CHECK failed in %s() %s:" "%d", __func__,           \
                         __FILE__, __LINE__);                                 \
            (cmd)->error = VIRTIO_GPU_RESP_ERR_UNSPEC;                        \
            return;                                                           \
       }                                                                      \
    } while (0)

struct virtio_gpu_rutabaga_async_signaled_fence {
    struct rutabaga_fence fence;
    QSLIST_ENTRY(virtio_gpu_rutabaga_async_signaled_fence) next;
};

static void
virtio_gpu_rutabaga_update_cursor(VirtIOGPU *g, struct virtio_gpu_scanout *s,
                                  uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;
    struct rutabaga_transfer transfer = { 0 };
    struct iovec transfer_iovec;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    res = virtio_gpu_find_resource(g, resource_id);
    if (!res) {
        return;
    }

    if (res->width != s->current_cursor->width ||
        res->height != s->current_cursor->height) {
        return;
    }

    transfer.x = 0;
    transfer.y = 0;
    transfer.z = 0;
    transfer.w = res->width;
    transfer.h = res->height;
    transfer.d = 1;

    transfer_iovec.iov_base = s->current_cursor->data;
    transfer_iovec.iov_len = res->width * res->height * 4;

    rutabaga_resource_transfer_read(vr->rutabaga, 0,
                                    resource_id, &transfer,
                                    &transfer_iovec);
}

static void
virtio_gpu_rutabaga_gl_flushed(VirtIOGPUBase *b)
{
    VirtIOGPU *g = VIRTIO_GPU(b);
    virtio_gpu_process_cmdq(g);
}

static void
rutabaga_cmd_create_resource_2d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct rutabaga_create_3d rc_3d = { 0 };
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_create_2d c2d;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(c2d);
    trace_virtio_gpu_cmd_res_create_2d(c2d.resource_id, c2d.format,
                                       c2d.width, c2d.height);

    rc_3d.target = 2;
    rc_3d.format = c2d.format;
    rc_3d.bind = (1 << 1);
    rc_3d.width = c2d.width;
    rc_3d.height = c2d.height;
    rc_3d.depth = 1;
    rc_3d.array_size = 1;
    rc_3d.last_level = 0;
    rc_3d.nr_samples = 0;
    rc_3d.flags = VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP;

    result = rutabaga_resource_create_3d(vr->rutabaga, c2d.resource_id, &rc_3d);
    CHECK(!result, cmd);

    res = g_new0(struct virtio_gpu_simple_resource, 1);
    res->width = c2d.width;
    res->height = c2d.height;
    res->format = c2d.format;
    res->resource_id = c2d.resource_id;

    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
}

static void
rutabaga_cmd_create_resource_3d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct rutabaga_create_3d rc_3d = { 0 };
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_create_3d c3d;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(c3d);

    trace_virtio_gpu_cmd_res_create_3d(c3d.resource_id, c3d.format,
                                       c3d.width, c3d.height, c3d.depth);

    rc_3d.target = c3d.target;
    rc_3d.format = c3d.format;
    rc_3d.bind = c3d.bind;
    rc_3d.width = c3d.width;
    rc_3d.height = c3d.height;
    rc_3d.depth = c3d.depth;
    rc_3d.array_size = c3d.array_size;
    rc_3d.last_level = c3d.last_level;
    rc_3d.nr_samples = c3d.nr_samples;
    rc_3d.flags = c3d.flags;

    result = rutabaga_resource_create_3d(vr->rutabaga, c3d.resource_id, &rc_3d);
    CHECK(!result, cmd);

    res = g_new0(struct virtio_gpu_simple_resource, 1);
    res->width = c3d.width;
    res->height = c3d.height;
    res->format = c3d.format;
    res->resource_id = c3d.resource_id;

    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
}

static void
rutabaga_unmap_slot(VirtIOGPUBase *vb, struct MemoryRegionInfo *slot)
{
    if (slot->mr.container == &vb->hostmem) {
        memory_region_del_subregion(&vb->hostmem, &slot->mr);
        object_unparent(OBJECT(&slot->mr));
    }
    slot->resource_id = 0;
}

static void
rutabaga_unmap_all_slots(VirtIOGPURutabaga *vgr)
{
    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(vgr);

    memory_region_transaction_begin();
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        rutabaga_unmap_slot(vb, &vgr->memory_regions[slot]);
    }
    memory_region_transaction_commit();
}

static bool
rutabaga_unmap_resource_slot(VirtIOGPURutabaga *vgr, uint32_t resource_id,
                             bool unmap_in_rutabaga)
{
    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(vgr);

    g_assert(resource_id != 0);

    for (uint32_t slot = 0; slot < MAX_SLOTS; slot++) {
        if (vgr->memory_regions[slot].resource_id != resource_id) {
            continue;
        }
        rutabaga_unmap_slot(vb, &vgr->memory_regions[slot]);
        if (unmap_in_rutabaga && vgr->rutabaga) {
            int32_t unmap_res = rutabaga_resource_unmap(vgr->rutabaga,
                                                        resource_id);
            if (unmap_res != 0) {
                warn_report("%s: rutabaga_resource_unmap(%"PRIu32") returned "
                            "%"PRIi32, __func__, resource_id, unmap_res);
            }
        }
        return true;
    }
    return false;
}

/*
 * Drops every scanout and cursor reference to `res`.  res->scanout_bitmask
 * has exactly one bit per scanout that currently shows the resource, so the
 * scanouts detached here must be precisely the bits that were set.
 */
static void
rutabaga_detach_resource_from_scanouts(VirtIOGPUBase *vb,
                                       struct virtio_gpu_simple_resource *res)
{
    uint32_t detached = 0;

    g_assert(res->resource_id != 0);

    for (uint32_t i = 0; i < vb->conf.max_outputs; i++) {
        struct virtio_gpu_scanout *scanout = &vb->scanout[i];
        if (scanout->resource_id == res->resource_id) {
            scanout->resource_id = 0;
            scanout->ds = NULL;
            if (scanout->con) {
                dpy_gfx_replace_surface(scanout->con, NULL);
            }
            detached |= 1u << i;
        }
        if (scanout->cursor.resource_id == res->resource_id) {
            scanout->cursor.resource_id = 0;
        }
    }
    g_assert(res->scanout_bitmask == detached);
    res->scanout_bitmask = 0;
}

static void
virtio_gpu_rutabaga_resource_unref(VirtIOGPU *g,
                                   struct virtio_gpu_simple_resource *res,
                                   Error **errp)
{
    int32_t result;
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    /*
     * Drop the BAR window and the guest DMA mapping before the backend
     * object goes away, so that nothing can still address the memory a blob
     * resource backed.  Also covers a guest that unrefs a mapped blob without
     * sending UNMAP_BLOB first.
     */
    rutabaga_unmap_resource_slot(vr, res->resource_id, true);
    virtio_gpu_cleanup_mapping(g, res);
    rutabaga_detach_resource_from_scanouts(VIRTIO_GPU_BASE(g), res);

    if (vr->rutabaga) {
        result = rutabaga_resource_unref(vr->rutabaga, res->resource_id);
        if (result) {
            error_setg_errno(errp, (int)result,
                             "%s: rutabaga_resource_unref returned %"PRIi32
                             " for resource_id = %"PRIu32, __func__, result,
                             res->resource_id);
        }
    }

    g_clear_pointer(&res->image, pixman_image_unref);

    QTAILQ_REMOVE(&g->reslist, res, next);
    g_free(res);
}

static void
rutabaga_cmd_resource_unref(VirtIOGPU *g,
                            struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result = 0;
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_unref unref;
    Error *local_err = NULL;

    VIRTIO_GPU_FILL_CMD(unref);

    trace_virtio_gpu_cmd_res_unref(unref.resource_id);

    res = virtio_gpu_find_resource(g, unref.resource_id);
    CHECK(res, cmd);

    virtio_gpu_rutabaga_resource_unref(g, res, &local_err);
    if (local_err) {
        error_report_err(local_err);
        /* local_err was freed, do not reuse it. */
        local_err = NULL;
        result = 1;
    }
    CHECK(!result, cmd);
}

static struct virtio_gpu_rutabaga_context *
virtio_gpu_rutabaga_find_context(VirtIOGPURutabaga *vr, uint32_t ctx_id)
{
    struct virtio_gpu_rutabaga_context *ctx;

    QTAILQ_FOREACH(ctx, &vr->contexts, next) {
        if (ctx->context_id == ctx_id) {
            return ctx;
        }
    }
    return NULL;
}

static void
rutabaga_cmd_context_create(VirtIOGPU *g,
                            struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_ctx_create cc;
    struct virtio_gpu_rutabaga_context *ctx;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(cc);
    trace_virtio_gpu_cmd_ctx_create(cc.hdr.ctx_id,
                                    cc.debug_name);

    result = rutabaga_context_create(vr->rutabaga, cc.hdr.ctx_id,
                                     cc.context_init, cc.debug_name, cc.nlen);
    CHECK(!result, cmd);

    ctx = g_new0(struct virtio_gpu_rutabaga_context, 1);
    ctx->context_id = cc.hdr.ctx_id;
    /*
     * Cached in memory so CTX-SNAPSHOTTABLE can inspect the capset before
     * save. Not serialized to the QEMUFile stream because any snapshot that
     * succeeded had no cross-domain contexts, and gfxstream persists its own
     * capset_id in gfxstream_frontend.txtproto.
     */
    ctx->capset_id = cc.context_init & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;
    QTAILQ_INIT(&ctx->reslist);
    QTAILQ_INSERT_TAIL(&vr->contexts, ctx, next);
}

static void
rutabaga_cmd_context_destroy(VirtIOGPU *g,
                             struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_ctx_destroy cd;
    struct virtio_gpu_rutabaga_context *ctx;
    struct virtio_gpu_rutabaga_resource *res, *tmp;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(cd);
    trace_virtio_gpu_cmd_ctx_destroy(cd.hdr.ctx_id);

    result = rutabaga_context_destroy(vr->rutabaga, cd.hdr.ctx_id);
    CHECK(!result, cmd);

    ctx = virtio_gpu_rutabaga_find_context(vr, cd.hdr.ctx_id);
    CHECK(ctx, cmd);

    QTAILQ_FOREACH_SAFE(res, &ctx->reslist, next, tmp) {
        QTAILQ_REMOVE(&ctx->reslist, res, next);
        g_free(res);
    }

    QTAILQ_REMOVE(&vr->contexts, ctx, next);
    g_free(ctx);
}

static void
rutabaga_setup_full_res_transfer(struct virtio_gpu_simple_resource *res,
                                 struct rutabaga_transfer *transfer,
                                 struct iovec *transfer_iovec)
{
    transfer->x = 0;
    transfer->y = 0;
    transfer->z = 0;
    transfer->w = res->width;
    transfer->h = res->height;
    transfer->d = 1;

    transfer_iovec->iov_base = pixman_image_get_data(res->image);
    transfer_iovec->iov_len = pixman_image_get_height(res->image) * pixman_image_get_stride(res->image);
}

static void virtio_gpu_rutabaga_update_display(VirtIOGPUBase *vb)
{
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(vb);
    VirtIOGPU* gpu = VIRTIO_GPU(vb);
    struct virtio_gpu_simple_resource *res;
    struct rutabaga_transfer transfer = { 0 };
    struct iovec transfer_iovec;
    struct virtio_gpu_scanout *scanout;
    int32_t i;

    if (vr->headless) {
        return;
    }

    for (i = 0; i < vb->conf.max_outputs; i++) {
        scanout = &vb->scanout[i];
        if (!scanout->con || !qemu_console_is_visible(scanout->con) || !scanout->resource_id) {
            continue;
        }
        res = virtio_gpu_find_resource(gpu, scanout->resource_id);
        if (!res || !res->image) {
            continue;
        }

        rutabaga_setup_full_res_transfer(res, &transfer, &transfer_iovec);
        if (rutabaga_resource_transfer_read(vr->rutabaga, 0, scanout->resource_id,
                                            &transfer, &transfer_iovec) == 0) {
            dpy_gfx_update_full(scanout->con);
        }
    }
}

static void
rutabaga_cmd_resource_flush(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result, i;
    struct virtio_gpu_scanout *scanout = NULL;
    struct virtio_gpu_simple_resource *res;
    struct rutabaga_transfer transfer = { 0 };
    struct iovec transfer_iovec;
    struct virtio_gpu_resource_flush rf;
    bool found = false;

    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);
    if (vr->headless) {
        return;
    }

    VIRTIO_GPU_FILL_CMD(rf);
    trace_virtio_gpu_cmd_res_flush(rf.resource_id,
                                   rf.r.width, rf.r.height, rf.r.x, rf.r.y);

    res = virtio_gpu_find_resource(g, rf.resource_id);
    CHECK(res, cmd);
    /*
     * res->image is created lazily by SET_SCANOUT.  A flush of a resource
     * that was never shown on a scanout is legal and has nothing to update.
     */
    if (!res->image) {
        return;
    }

    for (i = 0; i < vb->conf.max_outputs; i++) {
        scanout = &vb->scanout[i];
        if ((res->scanout_bitmask & (1u << i)) &&
            scanout->resource_id == rf.resource_id &&
            qemu_console_is_visible(scanout->con)) {
            found = true;
            break;
        }
    }

    if (!found) {
        return;
    }

    rutabaga_setup_full_res_transfer(res, &transfer, &transfer_iovec);

    trace_virtio_gpu_cmd_res_flush_read_start(rf.resource_id);
    result = rutabaga_resource_transfer_read(vr->rutabaga, 0,
                                             rf.resource_id, &transfer,
                                             &transfer_iovec);
    trace_virtio_gpu_cmd_res_flush_read_end(rf.resource_id, result);
    CHECK(!result, cmd);

    /* One read-back, then repaint every visible scanout showing the resource. */
    for (i = 0; i < vb->conf.max_outputs; i++) {
        scanout = &vb->scanout[i];
        if ((res->scanout_bitmask & (1u << i)) &&
            scanout->resource_id == rf.resource_id &&
            qemu_console_is_visible(scanout->con)) {
            dpy_gfx_update_full(scanout->con);
        }
    }
    trace_virtio_gpu_cmd_res_flush_done(rf.resource_id);
}

/* Clears the bit for `scanout_id` in whichever resource it currently shows. */
static void
rutabaga_clear_scanout_bit(VirtIOGPU *g, uint32_t scanout_id)
{
    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    struct virtio_gpu_scanout *scanout = &vb->scanout[scanout_id];
    struct virtio_gpu_simple_resource *old_res;

    if (scanout->resource_id == 0) {
        return;
    }
    old_res = virtio_gpu_find_resource(g, scanout->resource_id);
    if (old_res) {
        old_res->scanout_bitmask &= ~(1u << scanout_id);
    }
}

static void
rutabaga_cmd_set_scanout(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_scanout *scanout = NULL;
    struct virtio_gpu_set_scanout ss;

    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);
    if (vr->headless) {
        return;
    }

    VIRTIO_GPU_FILL_CMD(ss);
    trace_virtio_gpu_cmd_set_scanout(ss.scanout_id, ss.resource_id,
                                     ss.r.width, ss.r.height, ss.r.x, ss.r.y);

    /* Only scanouts that exist may be addressed; the bitmask has that many bits. */
    CHECK(ss.scanout_id < vb->conf.max_outputs, cmd);
    scanout = &vb->scanout[ss.scanout_id];

    if (ss.resource_id == 0) {
        rutabaga_clear_scanout_bit(g, ss.scanout_id);
        scanout->resource_id = 0;
        scanout->ds = NULL;
        dpy_gfx_replace_surface(scanout->con, NULL);
        dpy_gl_scanout_disable(scanout->con);
        return;
    }

    res = virtio_gpu_find_resource(g, ss.resource_id);
    CHECK(res, cmd);

    if (!res->image) {
        pixman_format_code_t pformat;
        pformat = virtio_gpu_get_pixman_format(res->format);
        CHECK(pformat, cmd);

        res->image = pixman_image_create_bits(pformat,
                                              res->width,
                                              res->height,
                                              NULL, 0);
        CHECK(res->image, cmd);
    }

    vb->enable = 1;

    /* realloc the surface ptr */
    scanout->ds = qemu_create_displaysurface_pixman(res->image);
    dpy_gfx_replace_surface(scanout->con, scanout->ds);
    /* The old resource's bit and the scanout slot always change together. */
    rutabaga_clear_scanout_bit(g, ss.scanout_id);
    scanout->resource_id = ss.resource_id;
    res->scanout_bitmask |= (1u << ss.scanout_id);
}

static void
rutabaga_cmd_submit_3d(VirtIOGPU *g,
                       struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_cmd_submit cs;
    struct rutabaga_command rutabaga_cmd = { 0 };
    g_autofree uint8_t *buf = NULL;
    size_t s;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(cs);
    trace_virtio_gpu_cmd_ctx_submit(cs.hdr.ctx_id, cs.size);

    buf = g_new0(uint8_t, cs.size);
    s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num,
                   sizeof(cs), buf, cs.size);
    CHECK(s == cs.size, cmd);

    rutabaga_cmd.ctx_id = cs.hdr.ctx_id;
    rutabaga_cmd.cmd = buf;
    rutabaga_cmd.cmd_size = cs.size;

    result = rutabaga_submit_command(vr->rutabaga, &rutabaga_cmd);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_transfer_to_host_2d(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct rutabaga_transfer transfer = { 0 };
    struct virtio_gpu_transfer_to_host_2d t2d;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(t2d);
    trace_virtio_gpu_cmd_res_xfer_toh_2d(t2d.resource_id);

    transfer.x = t2d.r.x;
    transfer.y = t2d.r.y;
    transfer.z = 0;
    transfer.w = t2d.r.width;
    transfer.h = t2d.r.height;
    transfer.d = 1;

    result = rutabaga_resource_transfer_write(vr->rutabaga, 0, t2d.resource_id,
                                              &transfer);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_transfer_to_host_3d(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct rutabaga_transfer transfer = { 0 };
    struct virtio_gpu_transfer_host_3d t3d;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(t3d);
    trace_virtio_gpu_cmd_res_xfer_toh_3d(t3d.resource_id);

    transfer.x = t3d.box.x;
    transfer.y = t3d.box.y;
    transfer.z = t3d.box.z;
    transfer.w = t3d.box.w;
    transfer.h = t3d.box.h;
    transfer.d = t3d.box.d;
    transfer.level = t3d.level;
    transfer.stride = t3d.stride;
    transfer.layer_stride = t3d.layer_stride;
    transfer.offset = t3d.offset;

    result = rutabaga_resource_transfer_write(vr->rutabaga, t3d.hdr.ctx_id,
                                              t3d.resource_id, &transfer);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_transfer_from_host_3d(VirtIOGPU *g,
                                   struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct rutabaga_transfer transfer = { 0 };
    struct virtio_gpu_transfer_host_3d t3d;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(t3d);
    trace_virtio_gpu_cmd_res_xfer_fromh_3d(t3d.resource_id);

    transfer.x = t3d.box.x;
    transfer.y = t3d.box.y;
    transfer.z = t3d.box.z;
    transfer.w = t3d.box.w;
    transfer.h = t3d.box.h;
    transfer.d = t3d.box.d;
    transfer.level = t3d.level;
    transfer.stride = t3d.stride;
    transfer.layer_stride = t3d.layer_stride;
    transfer.offset = t3d.offset;

    result = rutabaga_resource_transfer_read(vr->rutabaga, t3d.hdr.ctx_id,
                                             t3d.resource_id, &transfer, NULL);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_attach_backing(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct rutabaga_iovecs vecs = { 0 };
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_attach_backing att_rb;
    int ret;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(att_rb);
    trace_virtio_gpu_cmd_res_back_attach(att_rb.resource_id);

    res = virtio_gpu_find_resource(g, att_rb.resource_id);
    CHECK(res, cmd);
    CHECK(!res->iov, cmd);

    ret =
        virtio_gpu_create_mapping_iov(g, att_rb.nr_entries, sizeof(att_rb), cmd,
                                      &res->addrs, &res->iov, &res->iov_cnt);
    CHECK(!ret, cmd);

    vecs.iovecs = res->iov;
    vecs.num_iovecs = res->iov_cnt;

    ret = rutabaga_resource_attach_backing(vr->rutabaga, att_rb.resource_id,
                                           &vecs);
    if (ret != 0) {
        virtio_gpu_cleanup_mapping(g, res);
    }

    CHECK(!ret, cmd);
}

static void
rutabaga_cmd_detach_backing(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_detach_backing detach_rb;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(detach_rb);
    trace_virtio_gpu_cmd_res_back_detach(detach_rb.resource_id);

    res = virtio_gpu_find_resource(g, detach_rb.resource_id);
    CHECK(res, cmd);

    rutabaga_resource_detach_backing(vr->rutabaga,
                                     detach_rb.resource_id);

    virtio_gpu_cleanup_mapping(g, res);
}

static void
rutabaga_cmd_ctx_attach_resource(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_ctx_resource att_res;
    struct virtio_gpu_rutabaga_context *ctx;
    struct virtio_gpu_rutabaga_resource *res;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(att_res);
    trace_virtio_gpu_cmd_ctx_res_attach(att_res.hdr.ctx_id,
                                        att_res.resource_id);

    result = rutabaga_context_attach_resource(vr->rutabaga, att_res.hdr.ctx_id,
                                              att_res.resource_id);
    CHECK(!result, cmd);

    ctx = virtio_gpu_rutabaga_find_context(vr, att_res.hdr.ctx_id);
    CHECK(ctx, cmd);

    res = g_new0(struct virtio_gpu_rutabaga_resource, 1);
    res->resource_id = att_res.resource_id;
    QTAILQ_INSERT_TAIL(&ctx->reslist, res, next);
}

static void
rutabaga_cmd_ctx_detach_resource(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_ctx_resource det_res;
    struct virtio_gpu_rutabaga_context *ctx;
    struct virtio_gpu_rutabaga_resource *res;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(det_res);
    trace_virtio_gpu_cmd_ctx_res_detach(det_res.hdr.ctx_id,
                                        det_res.resource_id);

    result = rutabaga_context_detach_resource(vr->rutabaga, det_res.hdr.ctx_id,
                                              det_res.resource_id);
    CHECK(!result, cmd);

    ctx = virtio_gpu_rutabaga_find_context(vr, det_res.hdr.ctx_id);
    CHECK(ctx, cmd);

    QTAILQ_FOREACH(res, &ctx->reslist, next) {
        if (res->resource_id == det_res.resource_id) {
            QTAILQ_REMOVE(&ctx->reslist, res, next);
            g_free(res);
            break;
        }
    }
}

static void
rutabaga_cmd_get_capset_info(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_get_capset_info info;
    struct virtio_gpu_resp_capset_info resp;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(info);

    result = rutabaga_get_capset_info(vr->rutabaga, info.capset_index,
                                      &resp.capset_id, &resp.capset_max_version,
                                      &resp.capset_max_size);
    CHECK(!result, cmd);

    resp.hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void
rutabaga_cmd_get_capset(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_get_capset gc;
    struct virtio_gpu_resp_capset *resp;
    uint32_t capset_size, capset_version;
    uint32_t current_id, i;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(gc);
    for (i = 0; i < vr->num_capsets; i++) {
        result = rutabaga_get_capset_info(vr->rutabaga, i,
                                          &current_id, &capset_version,
                                          &capset_size);
        CHECK(!result, cmd);

        if (current_id == gc.capset_id) {
            break;
        }
    }

    CHECK(i < vr->num_capsets, cmd);

    resp = g_malloc0(sizeof(*resp) + capset_size);
    resp->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    rutabaga_get_capset(vr->rutabaga, gc.capset_id, gc.capset_version,
                        resp->capset_data, capset_size);

    virtio_gpu_ctrl_response(g, cmd, &resp->hdr, sizeof(*resp) + capset_size);
    g_free(resp);
}

static void
rutabaga_cmd_resource_create_blob(VirtIOGPU *g,
                                  struct virtio_gpu_ctrl_command *cmd)
{
    int result;
    struct rutabaga_iovecs vecs = { 0 };
    g_autofree struct virtio_gpu_simple_resource *res = NULL;
    struct virtio_gpu_resource_create_blob cblob;
    struct rutabaga_create_blob rc_blob = { 0 };

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(cblob);
    trace_virtio_gpu_cmd_res_create_blob(cblob.resource_id, cblob.size);

    CHECK(cblob.resource_id != 0, cmd);

    res = g_new0(struct virtio_gpu_simple_resource, 1);

    res->resource_id = cblob.resource_id;
    res->blob_size = cblob.size;

    if (cblob.blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D) {
        result = virtio_gpu_create_mapping_iov(g, cblob.nr_entries,
                                               sizeof(cblob), cmd, &res->addrs,
                                               &res->iov, &res->iov_cnt);
        CHECK(!result, cmd);
    }

    rc_blob.blob_id = cblob.blob_id;
    rc_blob.blob_mem = cblob.blob_mem;
    rc_blob.blob_flags = cblob.blob_flags;
    rc_blob.size = cblob.size;

    vecs.iovecs = res->iov;
    vecs.num_iovecs = res->iov_cnt;

    result = rutabaga_resource_create_blob(vr->rutabaga, cblob.hdr.ctx_id,
                                           cblob.resource_id, &rc_blob, &vecs,
                                           NULL);

    if (result && cblob.blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D) {
        virtio_gpu_cleanup_mapping(g, res);
    }

    CHECK(!result, cmd);

    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
    res = NULL;
}

static void
rutabaga_cmd_resource_map_blob(VirtIOGPU *g,
                               struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    uint32_t map_info = 0;
    uint32_t slot = 0;
    struct virtio_gpu_simple_resource *res;
    struct rutabaga_mapping mapping = { 0 };
    struct virtio_gpu_resource_map_blob mblob;
    struct virtio_gpu_resp_map_info resp = { 0 };

    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(mblob);

    CHECK(mblob.resource_id != 0, cmd);

    res = virtio_gpu_find_resource(g, mblob.resource_id);
    CHECK(res, cmd);

    result = rutabaga_resource_map_info(vr->rutabaga, mblob.resource_id,
                                        &map_info);
    CHECK(!result, cmd);

    /*
     * RUTABAGA_MAP_ACCESS_* flags are not part of the virtio-gpu spec, but do
     * exist to potentially allow the hypervisor to restrict write access to
     * memory. QEMU does not need to use this functionality at the moment.
     */
    resp.map_info = map_info & RUTABAGA_MAP_CACHE_MASK;

    result = rutabaga_resource_map(vr->rutabaga, mblob.resource_id, &mapping);
    CHECK(!result, cmd);

    /*
     * There is small risk of the MemoryRegion dereferencing the pointer after
     * rutabaga unmaps it. Please see discussion here:
     *
     * https://lists.gnu.org/archive/html/qemu-devel/2023-09/msg05141.html
     *
     * It is highly unlikely to happen in practice and doesn't affect known
     * use cases. However, it should be fixed and is noted here for posterity.
     */
    for (slot = 0; slot < MAX_SLOTS; slot++) {
        if (vr->memory_regions[slot].resource_id != 0) {
            continue;
        }

        g_assert(mblob.resource_id != 0);
        MemoryRegion *mr = &(vr->memory_regions[slot].mr);
        memory_region_init_ram_ptr(mr, OBJECT(vr), "blob", mapping.size,
                                   mapping.ptr);
        memory_region_add_subregion(&vb->hostmem, mblob.offset, mr);
        vr->memory_regions[slot].resource_id = mblob.resource_id;
        vr->memory_regions[slot].offset = mblob.offset;
        break;
    }

    if (slot >= MAX_SLOTS) {
        result = rutabaga_resource_unmap(vr->rutabaga, mblob.resource_id);
        CHECK(!result, cmd);
    }

    CHECK(slot < MAX_SLOTS, cmd);

    resp.hdr.type = VIRTIO_GPU_RESP_OK_MAP_INFO;
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void
rutabaga_cmd_resource_unmap_blob(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_unmap_blob ublob;
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(ublob);

    CHECK(ublob.resource_id != 0, cmd);

    res = virtio_gpu_find_resource(g, ublob.resource_id);
    CHECK(res, cmd);

    bool unmapped_slot = rutabaga_unmap_resource_slot(vr, ublob.resource_id, false);
    CHECK(unmapped_slot, cmd);

    result = rutabaga_resource_unmap(vr->rutabaga, res->resource_id);
    CHECK(!result, cmd);
}

static void
virtio_gpu_rutabaga_process_cmd(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    struct rutabaga_fence fence = { 0 };
    int32_t result;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(cmd->cmd_hdr);

    switch (cmd->cmd_hdr.type) {
    case VIRTIO_GPU_CMD_CTX_CREATE:
        rutabaga_cmd_context_create(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        rutabaga_cmd_context_destroy(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
        rutabaga_cmd_create_resource_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        rutabaga_cmd_create_resource_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        rutabaga_cmd_submit_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        rutabaga_cmd_transfer_to_host_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        rutabaga_cmd_transfer_to_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
        rutabaga_cmd_transfer_from_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        rutabaga_cmd_attach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        rutabaga_cmd_detach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        rutabaga_cmd_set_scanout(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        rutabaga_cmd_resource_flush(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        rutabaga_cmd_resource_unref(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        rutabaga_cmd_ctx_attach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        rutabaga_cmd_ctx_detach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        rutabaga_cmd_get_capset_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET:
        rutabaga_cmd_get_capset(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
        virtio_gpu_get_display_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_EDID:
        virtio_gpu_get_edid(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        rutabaga_cmd_resource_create_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        rutabaga_cmd_resource_map_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        rutabaga_cmd_resource_unmap_blob(g, cmd);
        break;
    default:
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }

    if (cmd->finished) {
        return;
    }
    if (cmd->error) {
        error_report("%s: ctrl 0x%x, error 0x%x", __func__,
                     cmd->cmd_hdr.type, cmd->error);
        virtio_gpu_ctrl_response_nodata(g, cmd, cmd->error);
        return;
    }
    if (!(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE)) {
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        return;
    }

    fence.flags = cmd->cmd_hdr.flags;
    fence.ctx_id = cmd->cmd_hdr.ctx_id;
    fence.fence_id = cmd->cmd_hdr.fence_id;
    fence.ring_idx = cmd->cmd_hdr.ring_idx;

    trace_virtio_gpu_fence_ctrl(cmd->cmd_hdr.fence_id, cmd->cmd_hdr.type);

    result = rutabaga_create_fence(vr->rutabaga, &fence);
    CHECK(!result, cmd);
}

static bool
rutabaga_fence_matches_cmd(const struct virtio_gpu_ctrl_command *cmd,
                           const struct rutabaga_fence *fence)
{
    uint32_t signaled_ctx_specific = fence->flags & RUTABAGA_FLAG_INFO_RING_IDX;
    uint32_t target_ctx_specific = cmd->cmd_hdr.flags & RUTABAGA_FLAG_INFO_RING_IDX;

    /* Due to context specific timelines. */
    if (signaled_ctx_specific != target_ctx_specific) {
        return false;
    }
    /*
     * Context specific timelines are per (ctx_id, ring_idx). Fence ids
     * are allocated from a single guest-wide counter, so a completion
     * from one context must not retire the pending fences of another.
     */
    if (signaled_ctx_specific &&
        (cmd->cmd_hdr.ring_idx != fence->ring_idx ||
         cmd->cmd_hdr.ctx_id != fence->ctx_id)) {
        return false;
    }
    return cmd->cmd_hdr.fence_id <= fence->fence_id;
}

static void
rutabaga_retire_matching_fences(VirtIOGPU *g,
                                const struct rutabaga_fence *fence)
{
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        if (!rutabaga_fence_matches_cmd(cmd, fence)) {
            continue;
        }
        trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        QTAILQ_REMOVE(&g->fenceq, cmd, next);
        g_free(cmd);
        g->inflight--;
    }
}

/*
 * Fences the backend has signaled (from its own worker threads) sit in
 * vgr->async_signaled_fenceq until the main thread does one of two things:
 *
 *  - CONSUME (rutabaga_process_async_signaled_fences): match each one
 *    against g->fenceq and answer the guest's commands.  Used while a
 *    session is live: from async_signaled_fence_bh in normal operation, and
 *    directly from virtio_gpu_rutabaga_wait_for_idle() during a snapshot save.
 *
 *  - DISCARD (virtio_gpu_rutabaga_discard_async_signaled_fences): free them
 *    without answering anything.  Used at reset / load boundaries, where the
 *    commands they refer to are being thrown away and acting on them would
 *    retire the next session's fences (b/575541006).
 *
 * Both run on the main thread with the BQL held, which is also the only
 * thread that dispatches async_signaled_fence_bh (it lives on the main
 * AioContext), so neither can race the bottom half.  The only other party is
 * virtio_gpu_rutabaga_fence_cb() on the renderer's worker threads, which is
 * why the queue uses the atomic QSLIST operations.
 */
static void
rutabaga_process_async_signaled_fences(VirtIOGPU *g)
{
    VirtIOGPURutabaga *vgr = VIRTIO_GPU_RUTABAGA(g);
    QSLIST_HEAD(, virtio_gpu_rutabaga_async_signaled_fence) drained_fences;

    assert(bql_locked());
    QSLIST_MOVE_ATOMIC(&drained_fences, &vgr->async_signaled_fenceq);

    while (!QSLIST_EMPTY(&drained_fences)) {
        struct virtio_gpu_rutabaga_async_signaled_fence *f =
            QSLIST_FIRST(&drained_fences);
        QSLIST_REMOVE_HEAD(&drained_fences, next);
        rutabaga_retire_matching_fences(g, &f->fence);
        g_free(f);
    }
}

static void
virtio_gpu_rutabaga_async_signaled_fence_bh(void *opaque)
{
    rutabaga_process_async_signaled_fences(VIRTIO_GPU(opaque));
}

/*
 * Discards all pending signaled-fence entries (without answering the guest)
 * and cancels the bottom half.  Instant; never waits.
 * Note: if backend worker threads are still running, a worker may push an
 * entry and reschedule async_signaled_fence_bh concurrently.  Callers tearing
 * down or restoring the backend must therefore call this again after
 * rutabaga_finish() or rutabaga_restore() has stopped producer threads.
 */
static void
virtio_gpu_rutabaga_discard_async_signaled_fences(VirtIOGPURutabaga *vgr)
{
    QSLIST_HEAD(, virtio_gpu_rutabaga_async_signaled_fence) discarded;

    assert(bql_locked());
    if (vgr->async_signaled_fence_bh) {
        qemu_bh_cancel(vgr->async_signaled_fence_bh);
    }
    QSLIST_MOVE_ATOMIC(&discarded, &vgr->async_signaled_fenceq);

    while (!QSLIST_EMPTY(&discarded)) {
        struct virtio_gpu_rutabaga_async_signaled_fence *f =
            QSLIST_FIRST(&discarded);
        QSLIST_REMOVE_HEAD(&discarded, next);
        g_free(f);
    }
}

static void
virtio_gpu_rutabaga_fence_cb(uint64_t user_data,
                             const struct rutabaga_fence *fence)
{
    VirtIOGPU *g = (VirtIOGPU *)user_data;
    VirtIOGPURutabaga *vgr = VIRTIO_GPU_RUTABAGA(g);
    struct virtio_gpu_rutabaga_async_signaled_fence *entry;

    /*
     * gfxstream and both cross-domain (and even newer versions virglrenderer:
     * see VIRGL_RENDERER_ASYNC_FENCE_CB) like to signal fence completion on
     * threads ("callback threads") that are different from the thread that
     * processes the command queue ("main thread").
     *
     * crosvm and other virtio-gpu 1.1 implementations enable callback threads
     * via locking.  However, on QEMU a deadlock is observed if
     * virtio_gpu_ctrl_response_nodata(..) [used in the fence callback] is used
     * from a thread that is not the main thread.
     *
     * The reason is QEMU's internal locking is designed to work with QEMU
     * threads (see rcu_register_thread()) and not generic C/C++/Rust threads.
     * So queue the completion and hand it to the main thread through a
     * bottom half the device owns, which the main thread can drain, cancel or
     * discard as a unit.
     */
    entry = g_new0(struct virtio_gpu_rutabaga_async_signaled_fence, 1);
    entry->fence = *fence;
    QSLIST_INSERT_HEAD_ATOMIC(&vgr->async_signaled_fenceq, entry, next);
    qemu_bh_schedule(vgr->async_signaled_fence_bh);
}

static void
virtio_gpu_rutabaga_debug_cb(uint64_t user_data,
                             const struct rutabaga_debug *debug)
{
    switch (debug->debug_type) {
    case RUTABAGA_DEBUG_ERROR:
        error_report("%s", debug->message);
        break;
    case RUTABAGA_DEBUG_WARN:
        warn_report("%s", debug->message);
        break;
    case RUTABAGA_DEBUG_INFO:
        info_report("%s", debug->message);
        break;
    default:
        error_report("unknown debug type: %u", debug->debug_type);
    }
}

static void
virtio_gpu_rutabaga_debug_cb_ex(uint64_t user_data,
                                const struct rutabaga_debug_ex *debug)
{
    Location loc;
    loc_push_none(&loc);
    loc_set_file(debug->file, debug->line);

    switch (debug->type) {
    case RUTABAGA_DEBUG_ERROR:
        error_report("%s", debug->message);
        break;
    case RUTABAGA_DEBUG_WARN:
        warn_report("%s", debug->message);
        break;
    case RUTABAGA_DEBUG_INFO:
        info_report("%s", debug->message);
        break;
    default:
        error_report("unknown debug type: %u", debug->type);
    }

    loc_pop(&loc);
}

static bool virtio_gpu_rutabaga_init(VirtIOGPU *g, Error **errp)
{
    int result;
    struct rutabaga_builder builder = { 0 };
    struct rutabaga_channel channel = { 0 };
    struct rutabaga_channels channels = { 0 };

    VirtIOGPUBase *base = VIRTIO_GPU_BASE(g);
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);
    vr->rutabaga = NULL;
    vr->restore_incomplete = false;
    QTAILQ_INIT(&vr->contexts);

    builder.wsi = RUTABAGA_WSI_SURFACELESS;
    /*
     * Currently, if WSI is specified, the only valid strings are "surfaceless"
     * or "headless".  Surfaceless doesn't create a native window surface, but
     * does copy from the render target to the Pixman buffer if a virtio-gpu
     * 2D hypercall is issued.  Surfacless is the default.
     *
     * Headless is like surfaceless, but doesn't copy to the Pixman buffer. The
     * use case is automated testing environments where there is no need to view
     * results.
     *
     * In the future, more performant virtio-gpu 2D UI integration may be added.
     */
    if (vr->wsi) {
        if (g_str_equal(vr->wsi, "surfaceless")) {
            vr->headless = false;
        } else if (g_str_equal(vr->wsi, "headless")) {
            vr->headless = true;
        } else {
            error_setg(errp, "invalid wsi option selected");
            return false;
        }
    }

    builder.fence_cb = virtio_gpu_rutabaga_fence_cb;
    builder.debug_cb_ex = virtio_gpu_rutabaga_debug_cb_ex;
    builder.capset_mask = vr->capset_mask;
    builder.user_data = (uint64_t)g;
    builder.display_width = base->conf.xres;
    builder.display_height = base->conf.yres;
    builder.renderer_features = vr->renderer_features;

    /*
     * If the user doesn't specify the wayland socket path, we try to infer
     * the socket via a process similar to the one used by libwayland.
     * libwayland does the following:
     *
     * 1) If $WAYLAND_DISPLAY is set, attempt to connect to
     *    $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY
     * 2) Otherwise, attempt to connect to $XDG_RUNTIME_DIR/wayland-0
     * 3) Otherwise, don't pass a wayland socket to rutabaga. If a guest
     *    wayland proxy is launched, it will fail to work.
     */
    channel.channel_type = RUTABAGA_CHANNEL_TYPE_WAYLAND;
    g_autofree gchar *path = NULL;
    if (!vr->wayland_socket_path) {
        const gchar *runtime_dir = g_get_user_runtime_dir();
        const gchar *display = g_getenv("WAYLAND_DISPLAY");
        if (!display) {
            display = "wayland-0";
        }

        if (runtime_dir) {
            path = g_build_filename(runtime_dir, display, NULL);
            channel.channel_name = path;
        }
    } else {
        channel.channel_name = vr->wayland_socket_path;
    }

    if ((builder.capset_mask & (1 << RUTABAGA_CAPSET_CROSS_DOMAIN))) {
        if (channel.channel_name) {
            channels.channels = &channel;
            channels.num_channels = 1;
            builder.channels = &channels;
        }
    }

    result = rutabaga_init(&builder, &vr->rutabaga);
    if (result) {
        error_setg_errno(errp, -result, "Failed to init rutabaga");
        return false;
    }

    return true;
}

static bool
virtio_gpu_rutabaga_get_num_capsets(VirtIOGPU *g, uint32_t *num_capsets, Error **errp)
{
    int result;
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    result = rutabaga_get_num_capsets(vr->rutabaga, num_capsets);
    if (result) {
        error_setg_errno(errp, -result, "Failed to get num_capsets");
        return false;
    }
    vr->num_capsets = *num_capsets;
    return true;
}

static void virtio_gpu_rutabaga_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    struct virtio_gpu_ctrl_command *cmd;

    if (!virtio_queue_ready(vq)) {
        return;
    }

    cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    while (cmd) {
        cmd->vq = vq;
        cmd->error = 0;
        cmd->finished = false;
        QTAILQ_INSERT_TAIL(&g->cmdq, cmd, next);
        cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    }

    virtio_gpu_process_cmdq(g);
}

static void virtio_gpu_rutabaga_vm_state_change(void* opaque, bool running,
                                                RunState state);

static void virtio_gpu_rutabaga_realize(DeviceState *qdev, Error **errp)
{
    uint32_t num_capsets;
    VirtIOGPUBase *bdev = VIRTIO_GPU_BASE(qdev);
    VirtIOGPU *gpudev = VIRTIO_GPU(qdev);
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(qdev);
    Error *local_err = NULL;

#if HOST_BIG_ENDIAN
    error_setg(errp, "rutabaga is not supported on bigendian platforms");
    return;
#endif

    QSLIST_INIT(&vr->async_signaled_fenceq);
    /*
     * The fence BH must live on the main AioContext, not the iohandler
     * context that virtio_bh_io_new_guarded() uses for ctrl_bh/cursor_bh.
     * virtio_gpu_rutabaga_wait_for_idle() spins in
     * aio_poll(qemu_get_aio_context()), which never dispatches the iohandler
     * context; a BH there would stay pending for the whole wait, the queue
     * would never empty and every save with an in-flight fence would hit the
     * timeout + force-retire path.  virtio_bh_new_guarded() keeps the
     * transport's reentrancy guard.
     */
    vr->async_signaled_fence_bh = virtio_bh_new_guarded(
        qdev, virtio_gpu_rutabaga_async_signaled_fence_bh, gpudev);

    if (!virtio_gpu_rutabaga_init(gpudev, errp)) {
        goto err_bh;
    }

    if (!virtio_gpu_rutabaga_get_num_capsets(gpudev, &num_capsets, errp)) {
        goto err_rutabaga;
    }

    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_RUTABAGA_ENABLED);
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_BLOB_ENABLED);
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_CONTEXT_INIT_ENABLED);

    bdev->virtio_config.num_capsets = num_capsets;
    virtio_gpu_device_realize(qdev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        goto err_rutabaga;
    }

    vr->vm_state_entry = qemu_add_vm_change_state_handler(
        virtio_gpu_rutabaga_vm_state_change, gpudev);
    return;

err_rutabaga:
    rutabaga_finish(&vr->rutabaga);
    virtio_gpu_rutabaga_discard_async_signaled_fences(vr);
err_bh:
    g_clear_pointer(&vr->async_signaled_fence_bh, qemu_bh_delete);
}

static void virtio_gpu_rutabaga_unrealize(DeviceState *qdev)
{
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(qdev);
    /*
     * Resolve against TYPE_VIRTIO_GPU statically rather than the dynamic class
     * of qdev so any future subclass of TYPE_VIRTIO_GPU_RUTABAGA does not
     * recurse back into virtio_gpu_rutabaga_unrealize.
     */
    VirtioDeviceClass *parent_vdc =
        VIRTIO_DEVICE_CLASS(object_class_by_name(TYPE_VIRTIO_GPU));

    if (vr->vm_state_entry) {
        qemu_del_vm_change_state_handler(vr->vm_state_entry);
        vr->vm_state_entry = NULL;
    }
    if (vr->rutabaga) {
        rutabaga_finish(&vr->rutabaga);
    }
    /* rutabaga_finish() joined the worker threads: nothing can be queued now. */
    virtio_gpu_rutabaga_discard_async_signaled_fences(vr);
    g_clear_pointer(&vr->async_signaled_fence_bh, qemu_bh_delete);

    if (parent_vdc->unrealize) {
        parent_vdc->unrealize(qdev);
    }
}

static const Property virtio_gpu_rutabaga_properties[] = {
    DEFINE_PROP_BIT64("gfxstream-vulkan", VirtIOGPURutabaga, capset_mask,
                      RUTABAGA_CAPSET_GFXSTREAM_VULKAN, false),
    DEFINE_PROP_BIT64("cross-domain", VirtIOGPURutabaga, capset_mask,
                      RUTABAGA_CAPSET_CROSS_DOMAIN, false),
    DEFINE_PROP_BIT64("x-gfxstream-gles", VirtIOGPURutabaga, capset_mask,
                      RUTABAGA_CAPSET_GFXSTREAM_GLES, false),
    DEFINE_PROP_BIT64("x-gfxstream-composer", VirtIOGPURutabaga, capset_mask,
                      RUTABAGA_CAPSET_GFXSTREAM_COMPOSER, false),
    DEFINE_PROP_STRING("wayland-socket-path", VirtIOGPURutabaga,
                       wayland_socket_path),
    DEFINE_PROP_STRING("wsi", VirtIOGPURutabaga, wsi),
    DEFINE_PROP_STRING("renderer_features", VirtIOGPURutabaga, renderer_features),
    DEFINE_PROP_STRING("snapshot_directory", VirtIOGPURutabaga, snapshot_directory),
};

static const VMStateDescription vmstate_virtio_gpu_scanout = {
    .name = "virtio-gpu-one-scanout",
    .version_id = 1,
    .fields =
        (const VMStateField[]){
            VMSTATE_UINT32(resource_id, struct virtio_gpu_scanout),
            VMSTATE_UINT32(width, struct virtio_gpu_scanout),
            VMSTATE_UINT32(height, struct virtio_gpu_scanout),
            VMSTATE_INT32(x, struct virtio_gpu_scanout),
            VMSTATE_INT32(y, struct virtio_gpu_scanout),
            VMSTATE_UINT32(cursor.resource_id, struct virtio_gpu_scanout),
            VMSTATE_UINT32(cursor.hot_x, struct virtio_gpu_scanout),
            VMSTATE_UINT32(cursor.hot_y, struct virtio_gpu_scanout),
            VMSTATE_UINT32(cursor.pos.x, struct virtio_gpu_scanout),
            VMSTATE_UINT32(cursor.pos.y, struct virtio_gpu_scanout),
            VMSTATE_END_OF_LIST()},
};

static const VMStateDescription vmstate_virtio_gpu_scanouts = {
    .name = "virtio-gpu-scanouts",
    .version_id = 1,
    .fields =
        (const VMStateField[]){
            VMSTATE_INT32(parent_obj.enable, struct VirtIOGPU),
            VMSTATE_UINT32_EQUAL(parent_obj.conf.max_outputs, struct VirtIOGPU,
                                 NULL),
            VMSTATE_STRUCT_VARRAY_UINT32(parent_obj.scanout, struct VirtIOGPU,
                                         parent_obj.conf.max_outputs, 1,
                                         vmstate_virtio_gpu_scanout,
                                         struct virtio_gpu_scanout),
            VMSTATE_END_OF_LIST()},
};

static bool virtio_gpu_rutabaga_instance_in_use(VirtIOGPURutabaga *vgr)
{
    /*
     * True if the guest has put anything into the current rutabaga instance:
     * contexts, resources, mapped blobs, or work that is queued or completed
     * but not yet delivered.  Reset uses the answer (captured *before* the
     * parent reset empties these lists) to decide whether the instance must
     * be torn down and recreated rather than reused.
     */
    VirtIOGPU *g = &(vgr->parent_obj);
    if (!QTAILQ_EMPTY(&vgr->contexts) ||
        !QTAILQ_EMPTY(&g->reslist) ||
        !QTAILQ_EMPTY(&g->cmdq) ||
        !QTAILQ_EMPTY(&g->fenceq) ||
        !QSLIST_EMPTY(&vgr->async_signaled_fenceq)) {
        return true;
    }
    for (int slot = 0; slot < MAX_SLOTS; slot++) {
        if (vgr->memory_regions[slot].resource_id != 0) {
            return true;
        }
    }
    return false;
}

static void rutabaga_free_all_contexts(VirtIOGPURutabaga *vgr)
{
    struct virtio_gpu_rutabaga_context *ctx, *next_ctx;

    QTAILQ_FOREACH_SAFE(ctx, &vgr->contexts, next, next_ctx) {
        struct virtio_gpu_rutabaga_resource *res, *next_res;
        QTAILQ_FOREACH_SAFE(res, &ctx->reslist, next, next_res) {
            QTAILQ_REMOVE(&ctx->reslist, res, next);
            g_free(res);
        }
        QTAILQ_REMOVE(&vgr->contexts, ctx, next);
        g_free(ctx);
    }
}

static void virtio_gpu_rutabaga_reinit(VirtIOGPU *g)
{
    VirtIOGPURutabaga *vgr = VIRTIO_GPU_RUTABAGA(g);
    Error *local_err = NULL;

    if (vgr->rutabaga) {
        rutabaga_finish(&vgr->rutabaga);
    }
    /* Drop any fence completions emitted while rutabaga_finish tore down workers. */
    virtio_gpu_rutabaga_discard_async_signaled_fences(vgr);

    if (!virtio_gpu_rutabaga_init(g, &local_err)) {
        error_report_err(local_err);
    }
}

/*
 * What virtio_gpu_rutabaga_verify() does when it finds a violation.  Set
 * through the "x-verify-state" property; see the lookup table below for the
 * spelling on the command line.
 */
typedef enum {
    RUTABAGA_VERIFY_MODE_OFF,     /* skip all checks */
    RUTABAGA_VERIFY_MODE_LOG,     /* default: error_report() and carry on */
    RUTABAGA_VERIFY_MODE_ENFORCE, /* fail the save/load (-EBUSY / -EINVAL) */
    RUTABAGA_VERIFY_MODE_STRICT,  /* abort() */
    RUTABAGA_VERIFY_MODE__MAX,
} RutabagaVerifyMode;

static const QEnumLookup rutabaga_verify_mode_lookup = {
    .array = (const char *const[]) {
        [RUTABAGA_VERIFY_MODE_OFF]     = "off",
        [RUTABAGA_VERIFY_MODE_LOG]     = "log",
        [RUTABAGA_VERIFY_MODE_ENFORCE] = "enforce",
        [RUTABAGA_VERIFY_MODE_STRICT]  = "strict",
    },
    .size = RUTABAGA_VERIFY_MODE__MAX,
};

static int virtio_gpu_rutabaga_get_verify_mode(Object *obj, Error **errp)
{
    return VIRTIO_GPU_RUTABAGA(obj)->verify_state_mode;
}

static void virtio_gpu_rutabaga_set_verify_mode(Object *obj, int mode,
                                                Error **errp)
{
    VIRTIO_GPU_RUTABAGA(obj)->verify_state_mode = mode;
}

/*
 * Verification phases, one pair per lifecycle transition:
 *   RESET      - virtio_gpu_rutabaga_reset()
 *   WAIT_IDLE  - virtio_gpu_rutabaga_wait_for_idle(); runs on every vm_stop
 *                (save, migrate, pause, shutdown), not only before a save
 *   SAVE       - virtio_gpu_rutabaga_save()
 *   LOAD       - virtio_gpu_pre_load() .. virtio_gpu_post_load()
 */
typedef enum {
    RUTABAGA_VERIFY_PRE_RESET,
    RUTABAGA_VERIFY_POST_RESET,
    RUTABAGA_VERIFY_PRE_WAIT_IDLE,
    RUTABAGA_VERIFY_POST_WAIT_IDLE,
    RUTABAGA_VERIFY_PRE_SAVE,
    RUTABAGA_VERIFY_POST_SAVE,
    RUTABAGA_VERIFY_PRE_LOAD,
    RUTABAGA_VERIFY_POST_LOAD,
} RutabagaVerifyPhase;

static const char *rutabaga_phase_name(RutabagaVerifyPhase phase)
{
    switch (phase) {
    case RUTABAGA_VERIFY_PRE_RESET:        return "PRE_RESET";
    case RUTABAGA_VERIFY_POST_RESET:       return "POST_RESET";
    case RUTABAGA_VERIFY_PRE_WAIT_IDLE:    return "PRE_WAIT_IDLE";
    case RUTABAGA_VERIFY_POST_WAIT_IDLE:   return "POST_WAIT_IDLE";
    case RUTABAGA_VERIFY_PRE_SAVE:         return "PRE_SAVE";
    case RUTABAGA_VERIFY_POST_SAVE:        return "POST_SAVE";
    case RUTABAGA_VERIFY_PRE_LOAD:         return "PRE_LOAD";
    case RUTABAGA_VERIFY_POST_LOAD:        return "POST_LOAD";
    default:                               return "UNKNOWN";
    }
}

static bool rutabaga_phase_expects_empty(RutabagaVerifyPhase phase)
{
    return phase == RUTABAGA_VERIFY_POST_RESET ||
           phase == RUTABAGA_VERIFY_PRE_LOAD;
}

/*
 * Phases at which no command may be queued.  Fences may still be in flight
 * around a save (they travel with the snapshot, see
 * virtio_gpu_rutabaga_save_inflight_fences), so the SAVE phases only require
 * an empty command queue; see rutabaga_phase_expects_no_fences().
 */
static bool rutabaga_phase_expects_quiesced(RutabagaVerifyPhase phase)
{
    return phase == RUTABAGA_VERIFY_POST_WAIT_IDLE ||
           phase == RUTABAGA_VERIFY_PRE_SAVE ||
           phase == RUTABAGA_VERIFY_POST_SAVE ||
           phase == RUTABAGA_VERIFY_POST_LOAD;
}

static bool rutabaga_phase_expects_no_fences(RutabagaVerifyPhase phase)
{
    return rutabaga_phase_expects_empty(phase) ||
           phase == RUTABAGA_VERIFY_POST_WAIT_IDLE ||
           phase == RUTABAGA_VERIFY_POST_LOAD;
}

static bool rutabaga_phase_expects_steady(RutabagaVerifyPhase phase)
{
    return phase == RUTABAGA_VERIFY_PRE_RESET ||
           phase == RUTABAGA_VERIFY_PRE_WAIT_IDLE ||
           phase == RUTABAGA_VERIFY_PRE_SAVE ||
           phase == RUTABAGA_VERIFY_POST_SAVE ||
           phase == RUTABAGA_VERIFY_POST_LOAD;
}

static G_GNUC_PRINTF(4, 5) void
rutabaga_report_violation(RutabagaVerifyPhase phase, size_t *violations,
                          const char *id, const char *fmt, ...)
{
    va_list ap;
    g_autofree char *detail = NULL;

    va_start(ap, fmt);
    detail = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    (*violations)++;
    error_report("virtio-gpu-rutabaga: verify[%s] %s: %s",
                 rutabaga_phase_name(phase), id, detail);
}

static void rutabaga_verify_lifetime(VirtIOGPURutabaga *vgr,
                                     RutabagaVerifyPhase phase,
                                     size_t *violations)
{
    if (!vgr->rutabaga) {
        rutabaga_report_violation(phase, violations, "LIFE-RUTABAGA",
                                  "vgr->rutabaga is NULL");
    }
    if (rutabaga_phase_expects_empty(phase)) {
        if (virtio_gpu_rutabaga_instance_in_use(vgr)) {
            rutabaga_report_violation(phase, violations, "LIFE-RUTABAGA-UNUSED",
                                      "rutabaga instance still holds guest objects or work");
        }
        if (vgr->restore_incomplete) {
            rutabaga_report_violation(phase, violations, "LIFE-RESTORE-COMPLETE",
                                      "a snapshot restore never completed and rutabaga was not reinitialized");
        }
    }
}

static void rutabaga_verify_queues_and_fences(VirtIOGPU *g,
                                              VirtIOGPURutabaga *vgr,
                                              RutabagaVerifyPhase phase,
                                              size_t *violations)
{
    bool need_empty_or_quiesced = rutabaga_phase_expects_empty(phase) ||
                                  rutabaga_phase_expects_quiesced(phase);
    uint32_t fenceq_len = 0;
    struct virtio_gpu_ctrl_command *cmd;

    if (g->ctrl_vq && virtio_queue_ready(g->ctrl_vq) &&
        (!g->ctrl_bh || !g->cursor_bh)) {
        rutabaga_report_violation(phase, violations, "VQ-BH",
                                  "ctrl_vq ready but BHs are NULL");
    }
    if (g->processing_cmdq) {
        rutabaga_report_violation(phase, violations, "CMDQ-NOREENTRY",
                                  "called while processing_cmdq is true");
    }
    if (need_empty_or_quiesced && !QTAILQ_EMPTY(&g->cmdq)) {
        rutabaga_report_violation(phase, violations, "CMDQ-EMPTY",
                                  "g->cmdq is not empty");
    }

    QTAILQ_FOREACH(cmd, &g->fenceq, next) {
        fenceq_len++;
        if (!(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE) || cmd->finished) {
            rutabaga_report_violation(phase, violations, "FENCE-FLAGS",
                                      "cmd in fenceq missing FENCE flag or already finished");
        }
    }
    if (g->inflight != fenceq_len) {
        rutabaga_report_violation(phase, violations, "FENCE-COUNT",
                                  "g->inflight (%u) != fenceq length (%u)",
                                  g->inflight, fenceq_len);
    }
    if (rutabaga_phase_expects_no_fences(phase) &&
        (fenceq_len > 0 || g->inflight != 0)) {
        rutabaga_report_violation(phase, violations, "FENCE-EMPTY",
                                  "fenceq_len=%u inflight=%u",
                                  fenceq_len, g->inflight);
    }
    if (rutabaga_phase_expects_no_fences(phase) &&
        qatomic_load_acquire(&vgr->async_signaled_fenceq.slh_first) != NULL) {
        rutabaga_report_violation(phase, violations, "ASYNC-SIGNALED-EMPTY",
                                  "async_signaled_fenceq is not empty");
    }
    if (!vgr->async_signaled_fence_bh) {
        rutabaga_report_violation(phase, violations, "ASYNC-SIGNALED-BH",
                                  "async_signaled_fence_bh is NULL");
    }
}

static bool rutabaga_has_gfxstream_capset(const VirtIOGPURutabaga *vgr)
{
    uint64_t gfxstream_mask = (1ULL << RUTABAGA_CAPSET_GFXSTREAM_VULKAN) |
                              (1ULL << RUTABAGA_CAPSET_GFXSTREAM_GLES) |
                              (1ULL << RUTABAGA_CAPSET_GFXSTREAM_COMPOSER);
    return (vgr->capset_mask & gfxstream_mask) != 0;
}

static void rutabaga_verify_contexts(VirtIOGPU *g, VirtIOGPURutabaga *vgr,
                                     RutabagaVerifyPhase phase, size_t *violations)
{
    struct virtio_gpu_rutabaga_context *ctx, *other_ctx;
    struct virtio_gpu_rutabaga_resource *r1, *r2;

    if (rutabaga_phase_expects_empty(phase) && !QTAILQ_EMPTY(&vgr->contexts)) {
        rutabaga_report_violation(phase, violations, "CTX-EMPTY",
                                  "vgr->contexts is not empty");
    }

    QTAILQ_FOREACH(ctx, &vgr->contexts, next) {
        for (other_ctx = QTAILQ_NEXT(ctx, next); other_ctx;
             other_ctx = QTAILQ_NEXT(other_ctx, next)) {
            if (ctx->context_id == other_ctx->context_id) {
                rutabaga_report_violation(phase, violations, "CTX-UNIQUE",
                                          "duplicate context_id %u",
                                          ctx->context_id);
            }
        }
        if (phase == RUTABAGA_VERIFY_PRE_SAVE &&
            ctx->capset_id == RUTABAGA_CAPSET_CROSS_DOMAIN) {
            rutabaga_report_violation(phase, violations, "CTX-SNAPSHOTTABLE",
                                      "context %u uses unsnapshottable cross-domain capset",
                                      ctx->context_id);
        }

        QTAILQ_FOREACH(r1, &ctx->reslist, next) {
            if (!virtio_gpu_find_resource(g, r1->resource_id)) {
                rutabaga_report_violation(phase, violations, "CTX-RES-EXISTS",
                                          "ctx %u references missing resource %u",
                                          ctx->context_id, r1->resource_id);
            }
            for (r2 = QTAILQ_NEXT(r1, next); r2; r2 = QTAILQ_NEXT(r2, next)) {
                if (r1->resource_id == r2->resource_id) {
                    rutabaga_report_violation(phase, violations, "CTX-UNIQUE",
                                              "ctx %u has duplicate resource %u",
                                              ctx->context_id, r1->resource_id);
                }
            }
        }
    }
}

static void rutabaga_verify_resources_and_dma(VirtIOGPU *g,
                                              RutabagaVerifyPhase phase,
                                              size_t *violations)
{
    struct virtio_gpu_simple_resource *res, *other_res;
    bool check_mapped = rutabaga_phase_expects_steady(phase);

    if (rutabaga_phase_expects_empty(phase) && !QTAILQ_EMPTY(&g->reslist)) {
        rutabaga_report_violation(phase, violations, "RES-EMPTY",
                                  "g->reslist is not empty");
    }

    QTAILQ_FOREACH(res, &g->reslist, next) {
        bool is_2d_or_3d = (res->width > 0 && res->height > 0 && res->format != 0);
        bool is_blob = (res->blob_size > 0);

        if (res->resource_id == 0) {
            rutabaga_report_violation(phase, violations, "RES-UNIQUE",
                                      "resource_id is 0");
        }
        for (other_res = QTAILQ_NEXT(res, next); other_res;
             other_res = QTAILQ_NEXT(other_res, next)) {
            if (res->resource_id == other_res->resource_id) {
                rutabaga_report_violation(phase, violations, "RES-UNIQUE",
                                          "duplicate resource_id %u",
                                          res->resource_id);
            }
        }

        /* Every resource is either a 2D/3D image, a blob, or (host3d) both. */
        if (!is_2d_or_3d && !is_blob) {
            rutabaga_report_violation(phase, violations, "RES-SHAPE",
                                      "resource %u has neither dimensions nor a blob size (w=%u h=%u fmt=%u blob=%"PRIu64")",
                                      res->resource_id, res->width, res->height,
                                      res->format, (uint64_t)res->blob_size);
        }

        /*
         * Every resource with guest backing in this device goes through
         * virtio_gpu_create_mapping_iov() with &res->addrs (ATTACH_BACKING
         * and guest-memory blobs alike), so iov, iov_cnt and addrs are set
         * and cleared together.
         */
        if (((res->iov == NULL) != (res->iov_cnt == 0)) ||
            ((res->addrs == NULL) != (res->iov == NULL))) {
            rutabaga_report_violation(phase, violations, "DMA-FIELDS-CONSISTENT",
                                      "resource %u inconsistent DMA fields",
                                      res->resource_id);
        }
        if (res->iov_cnt > 16384) {
            rutabaga_report_violation(phase, violations, "DMA-BOUND",
                                      "resource %u iov_cnt %u > 16384",
                                      res->resource_id, res->iov_cnt);
        }
        if (check_mapped && res->iov) {
            for (uint32_t i = 0; i < res->iov_cnt; i++) {
                if (!res->iov[i].iov_base || res->iov[i].iov_len == 0) {
                    rutabaga_report_violation(phase, violations, "DMA-MAPPED",
                                              "resource %u iov[%u] unmapped or empty",
                                              res->resource_id, i);
                    break;
                }
            }
        }
    }
}

/*
 * Scans all MAX_SLOTS entries and checks pairwise overlap among used slots in
 * O(used * MAX_SLOTS) time. Intended for lifecycle transitions and HMP rather
 * than per-command hot paths.
 */
static void rutabaga_verify_memory_slots(VirtIOGPU *g, VirtIOGPURutabaga *vgr,
                                         RutabagaVerifyPhase phase,
                                         size_t *violations)
{
    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    bool check_steady = rutabaga_phase_expects_steady(phase);
    bool check_empty = rutabaga_phase_expects_empty(phase);

    for (int i = 0; i < MAX_SLOTS; i++) {
        struct MemoryRegionInfo *s1 = &vgr->memory_regions[i];
        if (s1->resource_id == 0) {
            if (s1->mr.container != NULL) {
                rutabaga_report_violation(phase, violations, "SLOT-UNUSED-CLEAN",
                                          "unused slot %d still attached to container",
                                          i);
            }
            continue;
        }
        if (check_empty) {
            rutabaga_report_violation(phase, violations, "SLOT-ALL-FREE",
                                      "slot %d still used in EMPTY phase", i);
        }
        if (check_steady) {
            struct virtio_gpu_simple_resource *res =
                virtio_gpu_find_resource(g, s1->resource_id);
            if (!res || res->blob_size == 0 || s1->mr.container != &vb->hostmem) {
                rutabaga_report_violation(phase, violations, "SLOT-USED-VALID",
                                          "used slot %d invalid (res=%u)",
                                          i, s1->resource_id);
            }
        }
        for (int j = i + 1; j < MAX_SLOTS; j++) {
            struct MemoryRegionInfo *s2 = &vgr->memory_regions[j];
            uint64_t s1_end, s2_end;

            if (s2->resource_id == 0) {
                continue;
            }
            if (s1->resource_id == s2->resource_id) {
                rutabaga_report_violation(phase, violations, "SLOT-NO-OVERLAP",
                                          "slots %d and %d share resource %u",
                                          i, j, s1->resource_id);
            }
            s1_end = s1->offset + memory_region_size(&s1->mr);
            s2_end = s2->offset + memory_region_size(&s2->mr);
            if (s1->offset < s2_end && s2->offset < s1_end) {
                rutabaga_report_violation(phase, violations, "SLOT-NO-OVERLAP",
                                          "slots %d and %d overlap in BAR", i, j);
            }
        }
    }
}

static void rutabaga_verify_scanouts_and_cursors(VirtIOGPU *g,
                                                 VirtIOGPURutabaga *vgr,
                                                 RutabagaVerifyPhase phase,
                                                 size_t *violations)
{
    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    bool check_steady = rutabaga_phase_expects_steady(phase);
    bool check_empty = rutabaga_phase_expects_empty(phase);
    /*
     * scanout_bitmask is 32 bits wide and max_outputs <= VIRTIO_GPU_MAX_SCANOUTS,
     * so the shift below is always < 32.
     */
    QEMU_BUILD_BUG_ON(VIRTIO_GPU_MAX_SCANOUTS >= 32);
    uint32_t valid_bits = (1u << vb->conf.max_outputs) - 1;
    struct virtio_gpu_simple_resource *res;

    for (uint32_t i = 0; i < vb->conf.max_outputs; i++) {
        struct virtio_gpu_scanout *scanout = &vb->scanout[i];
        struct virtio_gpu_simple_resource *cres;

        if (check_empty && (scanout->resource_id != 0 || scanout->ds != NULL)) {
            rutabaga_report_violation(phase, violations, "SCAN-ALL-OFF",
                                      "scanout %u active in EMPTY phase", i);
        }
        if (check_steady && scanout->resource_id != 0 && !vgr->headless) {
            res = virtio_gpu_find_resource(g, scanout->resource_id);
            if (!res || !res->image ||
                !(res->scanout_bitmask & (1u << i)) ||
                (uint32_t)pixman_image_get_width(res->image) != res->width ||
                (uint32_t)pixman_image_get_height(res->image) != res->height) {
                rutabaga_report_violation(phase, violations, "SCAN-RES-IMAGE",
                                          "scanout %u resource %u missing valid image/bitmask",
                                          i, scanout->resource_id);
            } else if (scanout->con) {
                DisplaySurface *ds = qemu_console_surface(scanout->con);
                if (!ds ||
                    surface_data(ds) != (void *)pixman_image_get_data(res->image)) {
                    rutabaga_report_violation(phase, violations, "SCAN-SURFACE-BACKED",
                                              "scanout %u DisplaySurface not backed by res->image",
                                              i);
                }
            }
        }
        if (check_steady && scanout->cursor.resource_id != 0) {
            cres = virtio_gpu_find_resource(g, scanout->cursor.resource_id);
            if (!cres ||
                (scanout->current_cursor &&
                 (cres->width != scanout->current_cursor->width ||
                  cres->height != scanout->current_cursor->height))) {
                rutabaga_report_violation(phase, violations, "CUR-RES-EXISTS",
                                          "scanout %u cursor resource %u invalid or missing",
                                          i, scanout->cursor.resource_id);
            }
        }
    }

    QTAILQ_FOREACH(res, &g->reslist, next) {
        if (res->scanout_bitmask & ~valid_bits) {
            rutabaga_report_violation(phase, violations, "SCAN-REVERSE-MAP",
                                      "resource %u scanout_bitmask 0x%x has bits beyond max_outputs %u",
                                      res->resource_id, res->scanout_bitmask,
                                      vb->conf.max_outputs);
        }
        for (uint32_t i = 0; i < vb->conf.max_outputs; i++) {
            bool bit_set = (res->scanout_bitmask & (1u << i)) != 0;
            bool scanout_matches = (vb->scanout[i].resource_id == res->resource_id);
            if (bit_set != scanout_matches) {
                rutabaga_report_violation(phase, violations, "SCAN-REVERSE-MAP",
                                          "resource %u scanout_bitmask 0x%x disagrees with scanout[%u].resource_id %u",
                                          res->resource_id, res->scanout_bitmask,
                                          i, vb->scanout[i].resource_id);
            }
        }
    }
}

static void rutabaga_verify_disk_snapshot(VirtIOGPURutabaga *vgr,
                                          const char *renderer_dir,
                                          RutabagaVerifyPhase phase,
                                          size_t *violations)
{
    g_autofree char *rutabaga_snap = NULL;

    if (phase != RUTABAGA_VERIFY_POST_SAVE || !renderer_dir) {
        return;
    }
    rutabaga_snap = g_build_filename(renderer_dir, "rutabaga_snapshot", NULL);
    if (!g_file_test(rutabaga_snap, G_FILE_TEST_IS_REGULAR)) {
        rutabaga_report_violation(phase, violations, "X-DISK-COMPLETE",
                                  "missing %s", rutabaga_snap);
    }

    if (rutabaga_has_gfxstream_capset(vgr)) {
        static const char *const k_gfxstream_files[] = {
            "gfxstream_renderer.bin",
            "gfxstream_frontend.txtproto",
            "gfxstream_asg.bin",
        };
        for (size_t i = 0; i < G_N_ELEMENTS(k_gfxstream_files); i++) {
            g_autofree char *path = g_build_filename(
                renderer_dir, "gfxstream", k_gfxstream_files[i], NULL);
            if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
                rutabaga_report_violation(phase, violations, "X-DISK-COMPLETE",
                                          "missing %s", path);
            }
        }
    }
}

/*
 * Runs every invariant check for `phase` and acts on the result according to
 * vgr->verify_state_mode (see RutabagaVerifyMode).  The checks around reset
 * and the pre-stop idle wait are always advisory: those paths cannot fail,
 * so a violation there is only reported.
 */
static int virtio_gpu_rutabaga_verify(VirtIOGPU *g, RutabagaVerifyPhase phase,
                                      const char *renderer_dir)
{
    VirtIOGPURutabaga *vgr = VIRTIO_GPU_RUTABAGA(g);
    size_t violations = 0;
    int64_t start_us;
    bool advisory_only;

    if (vgr->verify_state_mode == RUTABAGA_VERIFY_MODE_OFF) {
        return 0;
    }

    start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    rutabaga_verify_lifetime(vgr, phase, &violations);
    rutabaga_verify_queues_and_fences(g, vgr, phase, &violations);
    rutabaga_verify_contexts(g, vgr, phase, &violations);
    rutabaga_verify_resources_and_dma(g, phase, &violations);
    rutabaga_verify_memory_slots(g, vgr, phase, &violations);
    rutabaga_verify_scanouts_and_cursors(g, vgr, phase, &violations);
    rutabaga_verify_disk_snapshot(vgr, renderer_dir, phase, &violations);

    trace_virtio_gpu_rutabaga_verify(rutabaga_phase_name(phase), violations,
                                     qemu_clock_get_us(QEMU_CLOCK_REALTIME) - start_us);

    if (violations == 0) {
        return 0;
    }

    advisory_only = (phase == RUTABAGA_VERIFY_PRE_RESET ||
                     phase == RUTABAGA_VERIFY_POST_RESET ||
                     phase == RUTABAGA_VERIFY_PRE_WAIT_IDLE ||
                     phase == RUTABAGA_VERIFY_POST_WAIT_IDLE);
    if (advisory_only) {
        return 0;
    }
    switch (vgr->verify_state_mode) {
    case RUTABAGA_VERIFY_MODE_STRICT:
        abort();
    case RUTABAGA_VERIFY_MODE_ENFORCE:
        return (phase == RUTABAGA_VERIFY_PRE_SAVE) ? -EBUSY : -EINVAL;
    default:
        return 0;
    }
}

/*
 * The renderer (gfxstream) state of a snapshot does not go through the VM
 * state stream; rutabaga writes it to its own directory.  Keep that directory
 * per snapshot, next to the other per-snapshot metadata the launcher keeps:
 *
 *   <snapshot_directory>/<snapshot name>/renderer/
 *
 * where <snapshot name> is the internal snapshot being saved or loaded.  The
 * launcher owns <snapshot_directory>/<snapshot name>/ itself; this device only
 * ever creates or removes the renderer/ subdirectory.
 *
 * Returns NULL (with an error reported) when snapshot_directory is not set or
 * no internal snapshot operation is in flight, which happens for migration.
 */
static char *virtio_gpu_rutabaga_renderer_dir(VirtIOGPURutabaga *vgr,
                                              const char *func)
{
    const char *snapshot_name;

    if (!vgr->snapshot_directory) {
        error_report("%s: snapshot_directory not configured", func);
        return NULL;
    }
    snapshot_name = qemu_snapshot_in_flight_name();
    if (!snapshot_name || !*snapshot_name) {
        error_report("%s: no internal snapshot in flight; renderer state can "
                     "only be saved to or loaded from a named snapshot", func);
        return NULL;
    }
    return g_build_filename(vgr->snapshot_directory, snapshot_name,
                            "renderer", NULL);
}

/*
 * Fenced commands the renderer had not signaled when the VM stopped.  They
 * travel with the snapshot as virtqueue elements, exactly like virtio-snd
 * carries its in-flight buffers, so the guest-visible ring state and the
 * device agree without answering anything on the guest's behalf at save
 * time.  The renderer snapshot taken just before waits for the GPU itself,
 * so by the time the snapshot is on disk the work behind these fences is
 * complete; on load they are completed from virtio_gpu_post_load().
 */
static void virtio_gpu_rutabaga_save_inflight_fences(QEMUFile *f, VirtIOGPU *g)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(g);
    struct virtio_gpu_ctrl_command *cmd;

    qemu_put_be32(f, g->inflight);
    QTAILQ_FOREACH(cmd, &g->fenceq, next) {
        qemu_put_virtqueue_element(vdev, f, &cmd->elem);
        qemu_put_be32(f, cmd->cmd_hdr.type);
        qemu_put_be32(f, cmd->cmd_hdr.flags);
        qemu_put_be64(f, cmd->cmd_hdr.fence_id);
        qemu_put_be32(f, cmd->cmd_hdr.ctx_id);
        qemu_put_byte(f, cmd->cmd_hdr.ring_idx);
    }
    if (g->inflight) {
        warn_report("virtio-gpu-rutabaga: %u fence(s) still in flight at save; "
                    "they will be completed on load", g->inflight);
    }
}

static int virtio_gpu_rutabaga_load_inflight_fences(QEMUFile *f, VirtIOGPU *g)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(g);
    uint32_t n = qemu_get_be32(f);

    if (n > VIRTQUEUE_MAX_SIZE) {
        error_report("%s: implausible in-flight fence count %u", __func__, n);
        return -EINVAL;
    }
    for (; n; --n) {
        struct virtio_gpu_ctrl_command *cmd =
            qemu_get_virtqueue_element(vdev, f, sizeof(*cmd));
        if (!cmd) {
            error_report("%s: failed to read an in-flight fenced command",
                         __func__);
            return -EINVAL;
        }
        cmd->vq = g->ctrl_vq;
        cmd->error = 0;
        cmd->finished = false;
        cmd->cmd_hdr.type = qemu_get_be32(f);
        cmd->cmd_hdr.flags = qemu_get_be32(f);
        cmd->cmd_hdr.fence_id = qemu_get_be64(f);
        cmd->cmd_hdr.ctx_id = qemu_get_be32(f);
        cmd->cmd_hdr.ring_idx = qemu_get_byte(f);
        QTAILQ_INSERT_TAIL(&g->fenceq, cmd, next);
        g->inflight++;
    }
    return 0;
}

/*
 * Answers every fence restored by virtio_gpu_rutabaga_load_inflight_fences():
 * the renderer snapshot they were saved with was taken after the GPU went
 * idle, so their work is done.
 */
static void virtio_gpu_rutabaga_complete_restored_fences(VirtIOGPU *g)
{
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        QTAILQ_REMOVE(&g->fenceq, cmd, next);
        g_free(cmd);
        g->inflight--;
    }
}

static int virtio_gpu_rutabaga_load(QEMUFile *f, void *opaque, size_t size,
                           const VMStateField *field) {
    VirtIOGPURutabaga *vgr = opaque;
    VirtIOGPU *g = &(vgr->parent_obj);
    struct virtio_gpu_simple_resource *res;
    uint32_t resource_id;
    int i;
    g_autofree char *renderer_dir = NULL;

    renderer_dir = virtio_gpu_rutabaga_renderer_dir(vgr, __func__);
    if (!renderer_dir) {
        return -EINVAL;
    }

    /*
     * Drop whatever the pre-restore session still had queued before the
     * restored renderer can start signaling; nothing it emits afterwards
     * may be thrown away.
     */
    virtio_gpu_rutabaga_discard_async_signaled_fences(vgr);
    /*
     * Mark the restore as incomplete before calling rutabaga_restore() so
     * that if rutabaga_restore(), the stream parsing below, or any later
     * device's post_load fails, the fallback cold-boot reset still tears
     * down and re-initializes the Rutabaga instance.  Cleared only once
     * virtio_gpu_post_load() has finished.
     */
    vgr->restore_incomplete = true;
    if (rutabaga_restore(vgr->rutabaga, renderer_dir)) {
        error_report("failed to restore rutabaga from %s", renderer_dir);
        return -EINVAL;
    }

    uint32_t ctx_id = qemu_get_be32(f);
    while (ctx_id != 0) {
        struct virtio_gpu_rutabaga_context *ctx;
        ctx = g_new0(struct virtio_gpu_rutabaga_context, 1);
        ctx->context_id = ctx_id;
        QTAILQ_INIT(&ctx->reslist);

        uint32_t res_id = qemu_get_be32(f);
        while (res_id != 0) {
            struct virtio_gpu_rutabaga_resource *res;
            res = g_new0(struct virtio_gpu_rutabaga_resource, 1);
            res->resource_id = res_id;
            QTAILQ_INSERT_TAIL(&ctx->reslist, res, next);
            res_id = qemu_get_be32(f);
        }

        QTAILQ_INSERT_TAIL(&vgr->contexts, ctx, next);
        ctx_id = qemu_get_be32(f);
    }

    resource_id = qemu_get_be32(f);
    while (resource_id != 0) {
        res = virtio_gpu_find_resource(g, resource_id);
        if (res) {
          error_report("duplicate resource 0x%llx",
                       (unsigned long long)resource_id);
          return -EINVAL;
        }

        res = g_new0(struct virtio_gpu_simple_resource, 1);
        res->resource_id = resource_id;
        res->width = qemu_get_be32(f);
        res->height = qemu_get_be32(f);
        res->format = qemu_get_be32(f);
        res->blob_size = qemu_get_be32(f);
        res->iov_cnt = qemu_get_be32(f);

        if (res->iov_cnt > 16384) {
            error_report("iov_cnt %u exceeds maximum of 16384", res->iov_cnt);
            g_free(res);
            return -EINVAL;
        }

        int has_addrs = qemu_get_be32(f);
        if (res->iov_cnt) {
            res->iov = g_new0(struct iovec, res->iov_cnt);

            for (i = 0; i < res->iov_cnt; i++) {
                res->iov[i].iov_len = qemu_get_be32(f);
            }
            if (has_addrs) {
              res->addrs = g_new0(uint64_t, res->iov_cnt);
              for (i = 0; i < res->iov_cnt; i++) {
                res->addrs[i] = qemu_get_be64(f);
              }
            }
        }

        QTAILQ_INSERT_HEAD(&g->reslist, res, next);
        resource_id = qemu_get_be32(f);
    }

    if (virtio_gpu_rutabaga_load_inflight_fences(f, g)) {
        return -EINVAL;
    }

    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    for (i = 0; i < vb->conf.max_outputs; i++) {
        uint32_t w = qemu_get_be32(f);
        if (w > 0) {
            uint32_t h = qemu_get_be32(f);
            uint32_t stride = qemu_get_be32(f);
            uint32_t format = qemu_get_be32(f);
            uint8_t flags = qemu_get_byte(f);

            if (w > 16384 || h > 16384) {
                error_report("%s: invalid surface dimensions %ux%u", __func__, w, h);
                return -EINVAL;
            }

            uint32_t bpp = PIXMAN_FORMAT_BPP(format);
            if (!bpp) {
                error_report("%s: invalid format %u", __func__, format);
                return -EINVAL;
            }

            uint32_t bytes_pp = DIV_ROUND_UP(bpp, 8);
            if (stride < w * bytes_pp) {
                error_report("%s: invalid stride %u for width %u and bytes_pp %u",
                             __func__, stride, w, bytes_pp);
                return -EINVAL;
            }

            uint64_t surface_size = (uint64_t)h * stride;
            if (surface_size > 256 * 1024 * 1024) {
                error_report("%s: surface size %llu exceeds maximum",
                             __func__, (unsigned long long)surface_size);
                return -EINVAL;
            }

            if (vb->scanout[i].con) {
                DisplaySurface *surface = qemu_create_displaysurface_from(w, h, format, stride, NULL);
                if (!surface_data(surface)) {
                    error_report("%s: failed to allocate surface data", __func__);
                    return -EINVAL;
                }
                qemu_get_buffer(f, surface_data(surface), h * stride);
                surface->flags = flags;
                dpy_gfx_replace_surface(vb->scanout[i].con, surface);
                QemuUIInfo info = {
                    .width = w,
                    .height = h,
                };
                dpy_set_ui_info(vb->scanout[i].con, &info, false);
            } else {
                error_report("%s %s %d: corrupted rutagaba surface data", __FILE__, __func__, __LINE__);
                return -EINVAL;
            }
        }
    }

    vmstate_load_state(f, &vmstate_virtio_gpu_scanouts, g, 1, NULL);

    /*
     * The scanout vmstate restores scanout[i].resource_id but the per-resource
     * bitmask is not serialized.  Rebuild it right here so the two never
     * disagree, not even if a later step of the load fails and the resources
     * are unreferenced by the fallback reset.
     */
    for (i = 0; i < vb->conf.max_outputs; i++) {
        if (vb->scanout[i].resource_id == 0) {
            continue;
        }
        res = virtio_gpu_find_resource(g, vb->scanout[i].resource_id);
        if (!res) {
            error_report("%s: scanout %d references missing resource %u",
                         __func__, i, vb->scanout[i].resource_id);
            return -EINVAL;
        }
        res->scanout_bitmask |= 1u << i;
    }
    return 0;
}

static void virtio_gpu_rutabaga_remove_dir_recursive(const char *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    const char *name;

    if (!dir) {
        g_remove(path);
        return;
    }

    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree char *child = g_build_filename(path, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
            !g_file_test(child, G_FILE_TEST_IS_SYMLINK)) {
            virtio_gpu_rutabaga_remove_dir_recursive(child);
        } else {
            g_remove(child);
        }
    }
    g_dir_close(dir);
    g_rmdir(path);
}

static int rutabaga_save_renderer_staged(VirtIOGPURutabaga *vgr,
                                         const char *renderer_dir)
{
    g_autofree char *staging_dir = g_strdup_printf("%s.tmp", renderer_dir);

    virtio_gpu_rutabaga_remove_dir_recursive(staging_dir);
    if (g_mkdir_with_parents(staging_dir, 0755) == -1) {
        error_report("Failed to create staging directory %s: %s",
                     staging_dir, g_strerror(errno));
        return -EINVAL;
    }

    if (rutabaga_snapshot(vgr->rutabaga, staging_dir)) {
        error_report("Failed to save graphics to %s", staging_dir);
        virtio_gpu_rutabaga_remove_dir_recursive(staging_dir);
        return -EINVAL;
    }

    /* Only the renderer state of *this* snapshot is replaced. */
    virtio_gpu_rutabaga_remove_dir_recursive(renderer_dir);
    if (g_rename(staging_dir, renderer_dir) != 0) {
        error_report("Failed to rename %s to %s: %s",
                     staging_dir, renderer_dir, g_strerror(errno));
        virtio_gpu_rutabaga_remove_dir_recursive(staging_dir);
        return -EINVAL;
    }
    return 0;
}

static int virtio_gpu_rutabaga_save(QEMUFile *f, void *opaque, size_t size,
                           const VMStateField *field, JSONWriter *vmdesc)
{
    VirtIOGPURutabaga *vgr = opaque;
    VirtIOGPU *g = &(vgr->parent_obj);
    struct virtio_gpu_simple_resource *res = NULL;
    int i, ret;
    g_autofree char *renderer_dir = NULL;

    renderer_dir = virtio_gpu_rutabaga_renderer_dir(vgr, __func__);
    if (!renderer_dir) {
        return -EINVAL;
    }

    if (!QTAILQ_EMPTY(&g->cmdq)) {
        error_report("%s: command queue is not empty", __func__);
        return -EBUSY;
    }

    ret = virtio_gpu_rutabaga_verify(g, RUTABAGA_VERIFY_PRE_SAVE, NULL);
    if (ret != 0) {
        return ret;
    }

    ret = rutabaga_save_renderer_staged(vgr, renderer_dir);
    if (ret != 0) {
        return ret;
    }

    struct virtio_gpu_rutabaga_context *ctx;
    struct virtio_gpu_rutabaga_resource *ctx_res;
    QTAILQ_FOREACH(ctx, &vgr->contexts, next) {
        qemu_put_be32(f, ctx->context_id);
        QTAILQ_FOREACH(ctx_res, &ctx->reslist, next) {
            qemu_put_be32(f, ctx_res->resource_id);
        }
        qemu_put_be32(f, 0); /* end of resource list */
    }
    qemu_put_be32(f, 0); /* end of context list */

    QTAILQ_FOREACH(res, &g->reslist, next) {
        qemu_put_be32(f, res->resource_id);
        qemu_put_be32(f, res->width);
        qemu_put_be32(f, res->height);
        qemu_put_be32(f, res->format);
        qemu_put_be32(f, res->blob_size);
        qemu_put_be32(f, res->iov_cnt);
        qemu_put_be32(f, res->addrs ? 1 : 0);
        for (i = 0; i < res->iov_cnt; i++) {
            qemu_put_be32(f, res->iov[i].iov_len);
        }
        if (res->addrs) {
          for (i = 0; i < res->iov_cnt; i++) {
            qemu_put_be64(f, res->addrs[i]);
          }
        }
    }
    qemu_put_be32(f, 0); /* end of list */

    virtio_gpu_rutabaga_save_inflight_fences(f, g);

    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    for (i = 0; i < vb->conf.max_outputs; i++) {
        if (vb->scanout[i].con) {
            DisplaySurface *surface;

            /*
             * Read the active scanout back from the renderer first: the
             * console surface is only refreshed on RESOURCE_FLUSH while a
             * window is attached, so with no window (headless CI, minimized)
             * it would otherwise be serialized black or stale.
             */
            if (!vgr->headless && vb->scanout[i].resource_id) {
                res = virtio_gpu_find_resource(g, vb->scanout[i].resource_id);
                if (res && res->image) {
                    struct rutabaga_transfer transfer = { 0 };
                    struct iovec transfer_iovec;
                    int32_t read_ret;

                    rutabaga_setup_full_res_transfer(res, &transfer,
                                                     &transfer_iovec);
                    read_ret = rutabaga_resource_transfer_read(
                        vgr->rutabaga, 0, vb->scanout[i].resource_id,
                        &transfer, &transfer_iovec);
                    if (read_ret != 0) {
                        warn_report("%s: read-back of scanout %d resource %u "
                                    "failed: %"PRIi32"; saving the last "
                                    "flushed frame", __func__, i,
                                    vb->scanout[i].resource_id, read_ret);
                    }
                }
            }
            surface = qemu_console_surface(vb->scanout[i].con);
            if (surface && surface_data(surface)) {
                int w = surface_width(surface);
                int h = surface_height(surface);
                int stride = surface_stride(surface);
                int format = surface_format(surface);
                qemu_put_be32(f, w);
                qemu_put_be32(f, h);
                qemu_put_be32(f, stride);
                qemu_put_be32(f, format);
                qemu_put_byte(f, surface->flags);
                qemu_put_buffer(f, surface_data(surface), h * stride);
            } else {
                qemu_put_be32(f, 0);
            }
        } else {
            qemu_put_be32(f, 0);
        }
    }

    ret = vmstate_save_state(f, &vmstate_virtio_gpu_scanouts, g, NULL, NULL);
    if (ret != 0) {
        error_report("%s: failed to save scanout state: %d", __func__, ret);
        virtio_gpu_rutabaga_remove_dir_recursive(renderer_dir);
        return ret;
    }
    ret = virtio_gpu_rutabaga_verify(g, RUTABAGA_VERIFY_POST_SAVE, renderer_dir);
    if (ret != 0) {
        /* In enforce mode the save fails; do not leave the half-trusted state behind. */
        virtio_gpu_rutabaga_remove_dir_recursive(renderer_dir);
    }
    return ret;
}

/*
 * Restore all persistent MemoryRegion mappings (slots) for blob resources.
 * This must be called AFTER the vmstate for the slots has been loaded and
 * AFTER the Rutabaga backend has been initialized.
 */
static int virtio_gpu_rutabaga_restore_mappings(VirtIOGPU* g) {
  VirtIOGPUBase* vb = VIRTIO_GPU_BASE(g);
  VirtIOGPURutabaga* vr = VIRTIO_GPU_RUTABAGA(g);
  struct virtio_gpu_simple_resource* res;
  int slot;
  int32_t result;

  memory_region_transaction_begin();
  for (slot = 0; slot < MAX_SLOTS; slot++) {
    uint32_t resource_id = vr->memory_regions[slot].resource_id;
    if (resource_id == 0) {
      continue;
    }
    hwaddr saved_offset = vr->memory_regions[slot].offset;

    // 1. Find the RutabagaResource metadata (must be restored first)
    res = virtio_gpu_find_resource(g, resource_id);
    if (!res) {
      error_report(
          "Failed to find metadata for mapped resource ID %u at slot %d.",
          resource_id, slot);
      memory_region_transaction_commit();
      return -EINVAL;
    }

    // 2. Request a NEW host pointer from the Rutabaga backend
    struct rutabaga_mapping mapping = {0};
    result = rutabaga_resource_map(vr->rutabaga, resource_id, &mapping);
    if (result != 0) {
      error_report("Failed to re-map Rutabaga resource %u: %d", resource_id,
                   result);
      memory_region_transaction_commit();
      return -EINVAL;
    }

    // 3. Re-initialize the MemoryRegion with the NEW pointer
    MemoryRegion* mr = &(vr->memory_regions[slot].mr);

    // Note: The MR must be re-initialized because the host pointer
    // (mapping.ptr) and often the size (mapping.size) are unstable across
    // migration.
    memory_region_init_ram_ptr(mr, OBJECT(vr), "blob", mapping.size,
                               mapping.ptr);

    // 4. Re-add the MemoryRegion to the Host Memory BAR at the saved offset
    // This re-establishes the MMIO path for the guest.
    memory_region_add_subregion(&vb->hostmem, saved_offset, mr);

    // The slot tracking fields are already correctly loaded by VMState,
    // so we don't need to rewrite resource_id or used.

  }

  memory_region_transaction_commit();
  return 0;
}

/* Helper to perform DMA mapping and Rutabaga attach */
static int virtio_gpu_rutabaga_load_backing(
    VirtIOGPU* g, struct virtio_gpu_simple_resource* res) {
  VirtIOGPURutabaga* vr = VIRTIO_GPU_RUTABAGA(g);
  struct rutabaga_iovecs vecs = {0};
  int i, ret;
  hwaddr len;

  // 1. Map GPAs to new Host Pointers
  for (i = 0; i < res->iov_cnt; i++) {
    len = res->iov[i].iov_len;

    // This call translates the saved GPA (res->addrs[i]) into a new,
    // valid host pointer (res->iov[i].iov_base).
    res->iov[i].iov_base =
        dma_memory_map(VIRTIO_DEVICE(g)->dma_as, res->addrs[i], &len,
                       DMA_DIRECTION_TO_DEVICE, MEMTXATTRS_UNSPECIFIED);

    if (!res->iov[i].iov_base || len != res->iov[i].iov_len) {
      error_report("Failed to restore DMA mapping for resource %u",
                   res->resource_id);
      // Cleanup partial mappings and fail
      virtio_gpu_cleanup_mapping(g, res);
      return -EINVAL;
    }
  }

  // 2. Notify Rutabaga Backend
  vecs.iovecs = res->iov;
  vecs.num_iovecs = res->iov_cnt;

  // Tell Rutabaga to re-attach the backing using the new host pointers
  ret = rutabaga_resource_attach_backing(vr->rutabaga, res->resource_id, &vecs);
  if (ret != 0) {
    error_report("Rutabaga failed to re-attach backing for resource %u: %d",
                 res->resource_id, ret);
    virtio_gpu_cleanup_mapping(g, res);
    return -EINVAL;
  }

  return 0;
}

/*
 * Rebuilds each scanout's display surface from its restored resource.  The
 * generic scanout vmstate restores scanout->resource_id but not the pixman
 * image behind it or the console surface.
 * The saved surface pixels (if any) are copied in first so there is
 * something to show immediately, then the renderer's contents are read back
 * on top.
 */
static int rutabaga_restore_scanout_surfaces(VirtIOGPU *g)
{
    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    VirtIOGPURutabaga *vgr = VIRTIO_GPU_RUTABAGA(g);

    if (vgr->headless) {
        return 0;
    }

    for (uint32_t i = 0; i < vb->conf.max_outputs; i++) {
        struct virtio_gpu_scanout *scanout = &vb->scanout[i];
        struct virtio_gpu_simple_resource *res;
        struct rutabaga_transfer transfer = { 0 };
        struct iovec transfer_iovec;
        DisplaySurface *restored_ds;
        uint8_t saved_flags;
        int32_t read_ret;

        if (scanout->resource_id == 0) {
            continue;
        }

        res = virtio_gpu_find_resource(g, scanout->resource_id);
        if (!res) {
            error_report("Scanout %u references missing resource %u",
                         i, scanout->resource_id);
            return -EINVAL;
        }

        if (!res->image) {
            pixman_format_code_t pformat =
                virtio_gpu_get_pixman_format(res->format);
            if (!pformat) {
                error_report("Invalid pixman format %u for scanout resource %u",
                             res->format, res->resource_id);
                return -EINVAL;
            }
            res->image = pixman_image_create_bits(pformat, res->width,
                                                  res->height, NULL, 0);
            if (!res->image) {
                error_report("Failed to create a %ux%u pixman image for "
                             "scanout resource %u",
                             res->width, res->height, res->resource_id);
                return -EINVAL;
            }
        }

        /* The bitmask was rebuilt when the scanout vmstate was loaded. */
        g_assert(res->scanout_bitmask & (1u << i));

        if (!scanout->con) {
            continue;
        }

        restored_ds = qemu_console_surface(scanout->con);
        saved_flags = restored_ds ? restored_ds->flags : 0;

        if (restored_ds && surface_data(restored_ds)) {
            if (surface_format(restored_ds) ==
                pixman_image_get_format(res->image)) {
                int copy_h = MIN(surface_height(restored_ds),
                                 pixman_image_get_height(res->image));
                int src_stride = surface_stride(restored_ds);
                int dst_stride = pixman_image_get_stride(res->image);
                size_t row_bytes = MIN((size_t)src_stride,
                                       (size_t)dst_stride);
                const uint8_t *src = surface_data(restored_ds);
                uint8_t *dst = (uint8_t *)pixman_image_get_data(res->image);
                for (int y = 0; y < copy_h; y++) {
                    memcpy(dst + (size_t)y * dst_stride,
                           src + (size_t)y * src_stride, row_bytes);
                }
            } else {
                warn_report("Scanout %u saved surface format 0x%x does not "
                            "match resource %u pixman format 0x%x; skipping "
                            "pixel copy",
                            i, surface_format(restored_ds),
                            res->resource_id,
                            pixman_image_get_format(res->image));
            }
        }

        rutabaga_setup_full_res_transfer(res, &transfer, &transfer_iovec);
        read_ret = rutabaga_resource_transfer_read(vgr->rutabaga, 0,
                                                   scanout->resource_id,
                                                   &transfer, &transfer_iovec);
        if (read_ret != 0) {
            warn_report("%s: read-back of scanout %u resource %u failed: "
                        "%"PRIi32"; showing the saved frame until the next "
                        "flush", __func__, i, scanout->resource_id, read_ret);
        }

        scanout->ds = qemu_create_displaysurface_pixman(res->image);
        scanout->ds->flags = saved_flags;
        dpy_gfx_replace_surface(scanout->con, scanout->ds);
        dpy_gfx_update_full(scanout->con);
    }

    return 0;
}

#define VIRTIO_GPU_RUTABAGA_IDLE_TIMEOUT_MS 5000

static void virtio_gpu_rutabaga_wait_for_idle_wakeup(void *opaque)
{
}

/*
 * Wait (up to VIRTIO_GPU_RUTABAGA_IDLE_TIMEOUT_MS) until the device has no
 * work in flight: g->cmdq, g->fenceq and vgr->async_signaled_fenceq all
 * empty.  Runs the main loop and CONSUMES completions while waiting --
 * nothing is thrown away.  Called from the vm_state_change handler once the
 * vCPUs are stopped.
 *
 * On timeout the fences still in g->fenceq are left alone: a save carries
 * them in the snapshot (virtio_gpu_rutabaga_save_inflight_fences) and
 * completes them on load, and a plain pause or shutdown lets the renderer
 * finish them when the VM resumes.
 */
static void virtio_gpu_rutabaga_wait_for_idle(VirtIOGPU *g)
{
    VirtIOGPURutabaga *vgr = VIRTIO_GPU_RUTABAGA(g);
    VirtIOGPUClass *vgc = VIRTIO_GPU_GET_CLASS(g);
    AioContext *ctx = qemu_get_aio_context();
    QEMUTimer wakeup;
    int64_t deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                       VIRTIO_GPU_RUTABAGA_IDLE_TIMEOUT_MS;

    virtio_gpu_rutabaga_verify(g, RUTABAGA_VERIFY_PRE_WAIT_IDLE, NULL);

    /* Since vCPU stopped, pop all remaining commands off ctrl_vq. */
    vgc->handle_ctrl(VIRTIO_DEVICE(g), g->ctrl_vq);
    /* Retire anything the backend already completed, without waiting for the BH. */
    rutabaga_process_async_signaled_fences(g);

    if (QTAILQ_EMPTY(&g->fenceq) && QTAILQ_EMPTY(&g->cmdq) &&
        QSLIST_EMPTY(&vgr->async_signaled_fenceq)) {
        virtio_gpu_rutabaga_verify(g, RUTABAGA_VERIFY_POST_WAIT_IDLE, NULL);
        return;
    }

    aio_timer_init(ctx, &wakeup, QEMU_CLOCK_REALTIME, SCALE_MS,
                   virtio_gpu_rutabaga_wait_for_idle_wakeup, NULL);
    timer_mod(&wakeup, deadline);

    while (!QTAILQ_EMPTY(&g->fenceq) || !QTAILQ_EMPTY(&g->cmdq) ||
           !QSLIST_EMPTY(&vgr->async_signaled_fenceq)) {
        if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) >= deadline) {
            warn_report("virtio-gpu-rutabaga: wait-for-idle timed out with "
                        "%u fence(s) in flight", g->inflight);
            break;
        }
        aio_poll(ctx, true);
        /*
         * Do not rely on async_signaled_fence_bh having run inside
         * aio_poll(): retire signaled fences directly so the loop
         * terminates as soon as the backend is done.
         */
        rutabaga_process_async_signaled_fences(g);
        virtio_gpu_process_cmdq(g);
    }

    timer_del(&wakeup);
    virtio_gpu_rutabaga_verify(g, RUTABAGA_VERIFY_POST_WAIT_IDLE, NULL);
}

static void virtio_gpu_rutabaga_vm_state_change(void *opaque, bool running,
                                                RunState state)
{
    if (!running &&
        (state == RUN_STATE_SAVE_VM || state == RUN_STATE_FINISH_MIGRATE ||
         state == RUN_STATE_SHUTDOWN || state == RUN_STATE_PAUSED)) {
        virtio_gpu_rutabaga_wait_for_idle(VIRTIO_GPU(opaque));
    }
}

static int virtio_gpu_pre_load(void *opaque)
{
    VirtIOGPURutabaga *vgr = opaque;
    VirtIOGPU *g = &(vgr->parent_obj);
    struct virtio_gpu_simple_resource *res, *tmp;

    /*
     * load_snapshot() runs qemu_system_reset() before qemu_loadvm_state(), so
     * these tables are normally already empty. Clean up any residual state
     * defensively so pre_load is self-healing on any load entry path.
     */
    virtio_gpu_rutabaga_discard_async_signaled_fences(vgr);
    rutabaga_unmap_all_slots(vgr);
    QTAILQ_FOREACH_SAFE(res, &g->reslist, next, tmp) {
        Error *local_err = NULL;
        virtio_gpu_rutabaga_resource_unref(g, res, &local_err);
        if (local_err) {
            error_report_err(local_err);
        }
    }
    rutabaga_free_all_contexts(vgr);
    if (!vgr->rutabaga || vgr->restore_incomplete) {
        virtio_gpu_rutabaga_reinit(g);
    }

    return virtio_gpu_rutabaga_verify(g, RUTABAGA_VERIFY_PRE_LOAD, NULL);
}

static int virtio_gpu_post_load(void *opaque, int version_id)
{
    VirtIOGPURutabaga *vgr = opaque;
    VirtIOGPU *g = &(vgr->parent_obj);
    struct virtio_gpu_simple_resource *res = NULL;
    int ret;

    QTAILQ_FOREACH(res, &g->reslist, next) {
        if (res->addrs) {
            ret = virtio_gpu_rutabaga_load_backing(g, res);
            if (ret != 0) {
                return ret;
            }
        }
    }

    ret = virtio_gpu_rutabaga_restore_mappings(g);
    if (ret != 0) {
        return ret;
    }

    /*
     * Context/resource attachments are not replayed here: gfxstream restores
     * them itself as part of rutabaga_restore(), and replaying them on top
     * clobbered its state.
     */
    ret = rutabaga_restore_scanout_surfaces(g);
    if (ret != 0) {
        return ret;
    }

    virtio_gpu_rutabaga_complete_restored_fences(g);

    ret = virtio_gpu_rutabaga_verify(g, RUTABAGA_VERIFY_POST_LOAD, NULL);
    if (ret != 0) {
        return ret;
    }
    vgr->restore_incomplete = false;

    /*
     * Re-kick both queues so commands that were still in the vrings at save
     * time are picked up; the guest believes it already submitted them.
     */
    qemu_bh_schedule(g->ctrl_bh);
    qemu_bh_schedule(g->cursor_bh);
    return 0;
}

static int virtio_gpu_blob_load(QEMUFile *f, void *opaque, size_t size,
                                const VMStateField *field) {
    return 0;
}

static int virtio_gpu_blob_save(QEMUFile *f, void *opaque, size_t size,
                                const VMStateField *field, JSONWriter *vmdesc) {
    return 0;
}

const VMStateDescription vmstate_virtio_gpu_rutabaga_blob_state = {
    .name = "virtio-gpu-rutabaga/blob",
    .minimum_version_id = VIRTIO_GPU_RUTABAGA_VM_VERSION,
    .version_id = VIRTIO_GPU_RUTABAGA_VM_VERSION,
    .needed = NULL,
    .fields = (const VMStateField[]){
        {
            .name = "virtio-gpu-rutabaga/blob",
            .info = &(const VMStateInfo) {
                .name = "blob",
                .get = virtio_gpu_blob_load,
                .put = virtio_gpu_blob_save,
            },
            .flags = VMS_SINGLE,
        } /* device */,
        VMSTATE_END_OF_LIST()
    },
};

/* VMState for a single slot in vr->memory_regions array */
static const VMStateDescription vmstate_rutabaga_slot_info = {
    .name = "virtio-gpu-rutabaga/slot",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]){
        VMSTATE_UINT32(resource_id, struct MemoryRegionInfo),
        VMSTATE_UINT64(offset,
                       struct MemoryRegionInfo),  // Assuming hwaddr is 64-bit
        VMSTATE_END_OF_LIST()}};

static const VMStateDescription vmstate_rutabaga_slots = {
    .name = "virtio-gpu-rutabaga/slots",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields =
        (const VMStateField[]){
            VMSTATE_STRUCT_ARRAY(memory_regions, VirtIOGPURutabaga, MAX_SLOTS,
                                 0, vmstate_rutabaga_slot_info,
                                 struct MemoryRegionInfo),
            VMSTATE_END_OF_LIST()},
};

/* Step 1: VMState for the array element (virtio_gpu_rutabaga_context) */

static const VMStateDescription vmstate_virtio_gpu_rutabaga = {
    .name = "virtio-gpu-rutabaga",
    .minimum_version_id = VIRTIO_GPU_RUTABAGA_VM_VERSION,
    .version_id = VIRTIO_GPU_RUTABAGA_VM_VERSION,
    .fields =
        (const VMStateField[]){
            VMSTATE_VIRTIO_DEVICE,
            {
                                   .name = "virtio-gpu-rutabaga-1",
                                   .info =
                                       &(const VMStateInfo){
                                           .name = "virtio-gpu-rutabaga-2",
                                           .get = virtio_gpu_rutabaga_load,
                                           .put = virtio_gpu_rutabaga_save,
                                       },
                                   .flags = VMS_SINGLE,
            } /* device */,
            VMSTATE_END_OF_LIST()
        },
    .subsections =
        (const VMStateDescription* const[]){
            &vmstate_rutabaga_slots,  // Pointer to the VMState that defines the
                                      // slot array
            NULL                      // Must be NULL-terminated
        },
    .post_load = virtio_gpu_post_load,
    .pre_load = virtio_gpu_pre_load,
};

static void virtio_gpu_rutabaga_reset(VirtIODevice *vdev)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    VirtIOGPURutabaga *vgr = VIRTIO_GPU_RUTABAGA(g);
    bool was_in_use;

    virtio_gpu_rutabaga_verify(g, RUTABAGA_VERIFY_PRE_RESET, NULL);

    /*
     * 1. Capture whether the rutabaga instance was in use (including
     *    undelivered signaled fences) BEFORE discarding queues or resetting
     *    parent VirtIOGPU state, which empties the lists this looks at.
     */
    was_in_use = virtio_gpu_rutabaga_instance_in_use(vgr);

    /*
     * 2. Discard pending signaled fences before touching device state.
     *    Signals for commands the reset is about to throw away must not
     *    retire the next session's fences; reinit() discards again after
     *    rutabaga_finish() has stopped the producer threads.
     */
    virtio_gpu_rutabaga_discard_async_signaled_fences(vgr);

    /* 3. Unmap BAR slots BEFORE virtio_gpu_reset unrefs resources. */
    rutabaga_unmap_all_slots(vgr);

    /*
     * 4. Reset parent VirtIOGPU state (calls resource_destroy /
     *    rutabaga_resource_unref per resource while vgr->rutabaga is still
     *    live, and empties cmdq/fenceq).
     */
    virtio_gpu_reset(vdev);

    /* 5. Free all 3D contexts and their attached resource lists. */
    rutabaga_free_all_contexts(vgr);

    /*
     * 6. Recreate the rutabaga instance if there is none, a restore never
     *    completed, or it was in use; otherwise keep the pristine one.
     */
    if (!vgr->rutabaga || vgr->restore_incomplete || was_in_use) {
        virtio_gpu_rutabaga_reinit(g);
    }

    virtio_gpu_rutabaga_verify(g, RUTABAGA_VERIFY_POST_RESET, NULL);
}

static void hmp_info_virtio_gpu_rutabaga(Monitor *mon, const QDict *qdict)
{
    Object *obj = object_resolve_path_type("", TYPE_VIRTIO_GPU_RUTABAGA, NULL);
    VirtIOGPU *g;
    VirtIOGPURutabaga *vgr;
    uint32_t num_ctx = 0, num_res = 0, num_cmdq = 0, num_fenceq = 0;
    uint32_t num_async = 0, num_slots = 0;
    size_t violations = 0;
    struct virtio_gpu_rutabaga_context *ctx;
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_ctrl_command *cmd;
    struct virtio_gpu_rutabaga_async_signaled_fence *af;

    if (!obj) {
        monitor_printf(mon, "No virtio-gpu-rutabaga device found\n");
        return;
    }
    g = VIRTIO_GPU(obj);
    vgr = VIRTIO_GPU_RUTABAGA(obj);

    QTAILQ_FOREACH(ctx, &vgr->contexts, next) {
        num_ctx++;
    }
    QTAILQ_FOREACH(res, &g->reslist, next) {
        num_res++;
    }
    QTAILQ_FOREACH(cmd, &g->cmdq, next) {
        num_cmdq++;
    }
    QTAILQ_FOREACH(cmd, &g->fenceq, next) {
        num_fenceq++;
    }
    /*
     * Renderer threads push to the head of this list concurrently; walk a
     * snapshot of the head taken with acquire semantics (the publishing
     * cmpxchg in QSLIST_INSERT_HEAD_ATOMIC orders each entry's `next` before
     * it).  Only the main thread ever unlinks, so the walk cannot loop.
     */
    for (af = qatomic_load_acquire(&vgr->async_signaled_fenceq.slh_first);
         af; af = af->next.sle_next) {
        num_async++;
    }
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (vgr->memory_regions[i].resource_id != 0) {
            num_slots++;
        }
    }

    rutabaga_verify_lifetime(vgr, RUTABAGA_VERIFY_PRE_RESET, &violations);
    rutabaga_verify_queues_and_fences(g, vgr, RUTABAGA_VERIFY_PRE_RESET, &violations);
    rutabaga_verify_contexts(g, vgr, RUTABAGA_VERIFY_PRE_RESET, &violations);
    rutabaga_verify_resources_and_dma(g, RUTABAGA_VERIFY_PRE_RESET, &violations);
    rutabaga_verify_memory_slots(g, vgr, RUTABAGA_VERIFY_PRE_RESET, &violations);
    rutabaga_verify_scanouts_and_cursors(g, vgr, RUTABAGA_VERIFY_PRE_RESET, &violations);

    monitor_printf(mon, "virtio-gpu-rutabaga: rutabaga=%p contexts=%u "
                        "resources=%u slots=%u cmdq=%u fenceq=%u (inflight=%u) "
                        "async_signaled_fenceq=%u violations=%zu\n",
                   vgr->rutabaga, num_ctx, num_res, num_slots,
                   num_cmdq, num_fenceq, g->inflight, num_async, violations);
}

static void virtio_gpu_rutabaga_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    VirtIOGPUBaseClass *vbc = VIRTIO_GPU_BASE_CLASS(klass);
    VirtIOGPUClass *vgc = VIRTIO_GPU_CLASS(klass);
    ObjectProperty *prop;

    vbc->gl_flushed = virtio_gpu_rutabaga_gl_flushed;
    vbc->update_display = virtio_gpu_rutabaga_update_display;
    vgc->handle_ctrl = virtio_gpu_rutabaga_handle_ctrl;
    vgc->process_cmd = virtio_gpu_rutabaga_process_cmd;
    vgc->update_cursor_data = virtio_gpu_rutabaga_update_cursor;
    vgc->resource_destroy = virtio_gpu_rutabaga_resource_unref;
    vdc->realize = virtio_gpu_rutabaga_realize;
    vdc->unrealize = virtio_gpu_rutabaga_unrealize;
    vdc->reset = virtio_gpu_rutabaga_reset;
    dc->vmsd = &vmstate_virtio_gpu_rutabaga;
    device_class_set_props(dc, virtio_gpu_rutabaga_properties);
    prop = object_class_property_add_enum(klass, "x-verify-state",
                                          "RutabagaVerifyMode",
                                          &rutabaga_verify_mode_lookup,
                                          virtio_gpu_rutabaga_get_verify_mode,
                                          virtio_gpu_rutabaga_set_verify_mode);
    object_property_set_default_str(prop, "log");
    object_class_property_set_description(klass, "x-verify-state",
            "What to do when a device invariant check fails: "
            "off, log (default), enforce (fail the save/load) or strict (abort)");
}

static const TypeInfo virtio_gpu_rutabaga_info[] = {
    {
        .name = TYPE_VIRTIO_GPU_RUTABAGA,
        .parent = TYPE_VIRTIO_GPU,
        .instance_size = sizeof(VirtIOGPURutabaga),
        .class_init = virtio_gpu_rutabaga_class_init,
    },
};

DEFINE_TYPES(virtio_gpu_rutabaga_info)

static void virtio_gpu_rutabaga_register_hmp(void)
{
    monitor_register_hmp("virtio-gpu-rutabaga", true,
                         hmp_info_virtio_gpu_rutabaga);
}

type_init(virtio_gpu_rutabaga_register_hmp)

module_obj(TYPE_VIRTIO_GPU_RUTABAGA);
module_kconfig(VIRTIO_GPU);
module_dep("hw-display-virtio-gpu");
