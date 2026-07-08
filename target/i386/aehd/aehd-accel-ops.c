/*
 * QEMU AEHD support
 *
 * Copyright IBM, Corp. 2008
 *           Red Hat, Inc. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Glauber Costa     <gcosta@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "system/accel-ops.h"
#include "system/aehd.h"
#include "system/runstate.h"
#include "system/cpus.h"
#include "qemu/guest-random.h"
#include "qapi/error.h"

#include "aehd-accel-ops.h"

static void *aehd_vcpu_thread_fn(void *arg)
{
    CPUState *cpu = arg;
    int r;

    rcu_register_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    current_cpu = cpu;

    r = aehd_init_vcpu(cpu);
    if (r < 0) {
        fprintf(stderr, "aehd_init_vcpu failed: %s\n", strerror(-r));
        exit(1);
    }

    /* signal CPU creation */
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        if (cpu_can_run(cpu)) {
            r = aehd_cpu_exec(cpu);
            if (r == EXCP_DEBUG) {
                cpu_handle_guest_debug(cpu);
            }
        }
        qemu_wait_io_event(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));

    aehd_destroy_vcpu(cpu);
    cpu_thread_signal_destroyed(cpu);
    bql_unlock();
    rcu_unregister_thread();
    return NULL;
}

static void aehd_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/AEHD",
             cpu->cpu_index);
    qemu_thread_create(cpu->thread, thread_name, aehd_vcpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

static bool aehd_vcpu_thread_is_idle(CPUState *cpu)
{
    return false;
}

static void aehd_kick_vcpu_thread(CPUState *cpu)
{
    cpu_exit(cpu);
    aehd_raise_event(cpu);
}

static void aehd_accel_ops_class_init(ObjectClass *oc, void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    ops->create_vcpu_thread = aehd_start_vcpu_thread;
    ops->cpu_thread_is_idle = aehd_vcpu_thread_is_idle;
    ops->synchronize_post_reset = aehd_cpu_synchronize_post_reset;
    ops->synchronize_post_init = aehd_cpu_synchronize_post_init;
    ops->synchronize_state = aehd_cpu_synchronize_state;
    ops->synchronize_pre_loadvm = aehd_cpu_synchronize_pre_loadvm;
    ops->kick_vcpu_thread = aehd_kick_vcpu_thread;
}

static const TypeInfo aehd_accel_ops_type = {
    .name = ACCEL_OPS_NAME("aehd"),

    .parent = TYPE_ACCEL_OPS,
    .class_init = aehd_accel_ops_class_init,
    .abstract = true,
};

static void aehd_accel_ops_register_types(void)
{
    type_register_static(&aehd_accel_ops_type);
}
type_init(aehd_accel_ops_register_types);
