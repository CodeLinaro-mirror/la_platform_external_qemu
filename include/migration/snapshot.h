/*
 * QEMU snapshots
 *
 * Copyright (c) 2004-2008 Fabrice Bellard
 * Copyright (c) 2009-2015 Red Hat Inc
 *
 * Authors:
 *  Juan Quintela <quintela@redhat.com>
 *
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_MIGRATION_SNAPSHOT_H
#define QEMU_MIGRATION_SNAPSHOT_H

#include "qapi/qapi-builtin-types.h"
#include "qapi/qapi-types-run-state.h"

/**
 * save_snapshot: Save an internal snapshot.
 * @name: name of internal snapshot
 * @overwrite: replace existing snapshot with @name
 * @vmstate: blockdev node name to store VM state in
 * @has_devices: whether to use explicit device list
 * @devices: explicit device list to snapshot
 * @errp: pointer to error object
 * On success, return %true.
 * On failure, store an error through @errp and return %false.
 */
bool save_snapshot(const char *name, bool overwrite,
                   const char *vmstate,
                   bool has_devices, strList *devices,
                   Error **errp);

/**
 * load_snapshot: Load an internal snapshot.
 * @name: name of internal snapshot
 * @vmstate: blockdev node name to load VM state from
 * @has_devices: whether to use explicit device list
 * @devices: explicit device list to snapshot
 * @errp: pointer to error object
 * On success, return %true.
 * On failure, store an error through @errp and return %false.
 */
bool load_snapshot(const char *name,
                   const char *vmstate,
                   bool has_devices, strList *devices,
                   Error **errp);

/**
 * delete_snapshot: Delete a snapshot.
 * @name: path to snapshot
 * @has_devices: whether to use explicit device list
 * @devices: explicit device list to snapshot
 * @errp: pointer to error object
 * On success, return %true.
 * On failure, store an error through @errp and return %false.
 */
bool delete_snapshot(const char *name,
                    bool has_devices, strList *devices,
                    Error **errp);

/**
 * load_snapshot_resume: Restore runstate after loading snapshot.
 * @state: state to restore
 */
void load_snapshot_resume(RunState state);

typedef enum QemuSnapshotOperation {
    QEMU_SNAPSHOT_OP_NONE = 0,
    QEMU_SNAPSHOT_OP_SAVE = 1,
    QEMU_SNAPSHOT_OP_LOAD = 2,
} QemuSnapshotOperation;

typedef enum QemuSnapshotPhase {
    QEMU_SNAPSHOT_PHASE_NONE = 0,
    QEMU_SNAPSHOT_PHASE_PREPARING = 1,
    QEMU_SNAPSHOT_PHASE_TRANSFERRING = 2,
    QEMU_SNAPSHOT_PHASE_COMPLETED = 3,
    QEMU_SNAPSHOT_PHASE_FAILED = 4,
} QemuSnapshotPhase;

/**
 * QemuSnapshotProgress: Progress of an internal snapshot operation.
 * @operation: QEMU_SNAPSHOT_OP_SAVE or QEMU_SNAPSHOT_OP_LOAD.
 * @phase: the phase of the operation, never QEMU_SNAPSHOT_PHASE_NONE.
 * @name: the snapshot name.
 * @active_component: the idstr of the VMState section started most recently,
 *     or NULL.  In a QEMU_SNAPSHOT_PHASE_FAILED report, it hints at where the
 *     operation failed but is not necessarily the failing section.
 * @error_message: the reason for a QEMU_SNAPSHOT_PHASE_FAILED report, or NULL.
 * @transferred_bytes: RAM bytes saved (save) or VM state bytes read (load).
 * @total_bytes: the value of @transferred_bytes on completion.
 * @disk_bytes: bytes written to (save) or read from (load) the VM state.
 * @completed_devices: non-iterable device states saved or loaded so far.
 * @total_devices: the value of @completed_devices on completion; an upper
 *     bound when loading, as device states that were not needed when saving
 *     are absent from the snapshot.
 *
 * Passed to a QemuSnapshotProgressHandler.  The strings are only valid for
 * the duration of the handler call.
 */
typedef struct QemuSnapshotProgress {
    QemuSnapshotOperation operation;
    QemuSnapshotPhase phase;
    const char *name;
    const char *active_component;
    const char *error_message;
    int64_t transferred_bytes;
    int64_t total_bytes;
    int64_t disk_bytes;
    int32_t completed_devices;
    int32_t total_devices;
} QemuSnapshotProgress;

typedef struct QemuSnapshotProgressEntry QemuSnapshotProgressEntry;
typedef void QemuSnapshotProgressHandler(void *opaque,
                                         const QemuSnapshotProgress *progress);

/**
 * add_snapshot_progress_handler: Register a snapshot progress handler.
 * @cb: the handler
 * @opaque: passed to @cb
 *
 * @cb is invoked for every progress report of save_snapshot() and
 * load_snapshot().  An operation reports QEMU_SNAPSHOT_PHASE_PREPARING once,
 * then QEMU_SNAPSHOT_PHASE_TRANSFERRING when the VM state transfer starts and
 * at the start of every VMState section, and finally either
 * QEMU_SNAPSHOT_PHASE_COMPLETED or QEMU_SNAPSHOT_PHASE_FAILED exactly once.
 * An operation that fails before the transfer starts goes from PREPARING
 * directly to FAILED; a call that fails before the operation starts reports
 * nothing.  There are no reports while the block layer deletes an overwritten
 * snapshot (before PREPARING), or reverts or creates the disk snapshots.
 *
 * @cb runs synchronously on the thread performing the operation, with the BQL
 * held, so it must not block: copy what is needed and return.  @cb must not
 * start a snapshot operation, and may unregister itself but no other handler.
 *
 * The BQL may be released while an operation is in flight, for example while
 * waiting for vCPUs, so handlers can be added or removed meanwhile.  A handler
 * added during an operation only receives the reports of the next one, and a
 * removed handler receives no further reports.
 *
 * Must be called with the BQL held.
 *
 * Returns: an entry to pass to del_snapshot_progress_handler().
 */
QemuSnapshotProgressEntry *add_snapshot_progress_handler(
    QemuSnapshotProgressHandler *cb, void *opaque);

/**
 * del_snapshot_progress_handler: Unregister a snapshot progress handler.
 * @e: the entry returned by add_snapshot_progress_handler(), or NULL
 *
 * Must be called with the BQL held.
 */
void del_snapshot_progress_handler(QemuSnapshotProgressEntry *e);

#endif
