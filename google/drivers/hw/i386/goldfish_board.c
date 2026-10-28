// Copyright (C) 2023 The Android Open Source Project
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
#include "qemu/osdep.h"

#include "google/drivers/hw/acpi/goldfish_acpi.h"
#include "google/drivers/hw/acpi/goldfish_defs.h"
#include "google/drivers/hw/i386/goldfish_board.h"
#include "hw/core/boards.h"
#include "hw/char/parallel-isa.h"
#include "hw/cxl/cxl_host.h"
#include "hw/display/ramfb.h"
#include "hw/core/hotplug.h"
#include "hw/hyperv/vmbus-bridge.h"
#include "hw/i386/pc.h"
#include "hw/southbridge/piix.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/typedefs.h"

static void android_machine_options(MachineClass *m) {
  m->desc = "Android x86_64 (i440fx + PIIX)";
  m->default_machine_opts = "firmware=bios-256k.bin";
  m->default_machine_opts = "firmware=bios-256k.bin,suppress-vmdesc=on";
  m->default_display = "std";
  m->default_nic = "e1000";
  m->no_parallel = !module_object_class_by_name(TYPE_ISA_PARALLEL);
  machine_class_allow_dynamic_sysbus_dev(m, TYPE_RAMFB_DEVICE);
  machine_class_allow_dynamic_sysbus_dev(m, TYPE_VMBUS_BRIDGE);

  PCMachineClass *pcmc = PC_MACHINE_CLASS(m);
  pcmc->default_south_bridge = TYPE_PIIX3_DEVICE;
  pcmc->broken_32bit_mem_addr_check = true;
  pcmc->pci_root_uid = 0;
  pcmc->default_cpu_version = 1;
}

static MachineClass *find_machine(const char *name, GSList *machines) {
  GSList *el;

  for (el = machines; el; el = el->next) {
    MachineClass *mc = el->data;

    if (!strcmp(mc->name, name) || !g_strcmp0(mc->alias, name)) {
      return mc;
    }
  }

  return NULL;
}

static DeviceState *create_simple_device(const char *name,
                                         hwaddr addr, qemu_irq irq) {
  if (!module_object_class_by_name(name)) {
    error_report("QOM type '%s' is not registered (is libgoldfish loaded?)",
                 name);
    exit(1);
  }

  return sysbus_create_simple(name, addr, irq);
}

static void pc_init_goldfish(MachineState *machine) {
  // The goldfish board is basically a modified pc-i440fx board with a custom
  // acpi. So we are going to find the pc-i440fx machine, and call its
  // initialize function. This will give us a good baseline.
  GSList *machines = object_class_get_list(TYPE_MACHINE, false);
  MachineClass *machine_class;
  GoldfishMachineState *ams = ANDROID_MACHINE(machine);
  X86MachineState *x86ms = X86_MACHINE(machine);
  PCMachineState *pcms = PC_MACHINE(machine);

  machine_class = find_machine("pc", machines);
  machine_class->init(machine);

  // Add the battery device.
  sysbus_create_simple("goldfish_battery", GOLDFISH_BATTERY_IOMEM_BASE,
                       x86ms ->gsi[GOLDFISH_BATTERY_IRQ]);

  if (ams->enable_goldfish_pipe) {
    create_simple_device("goldfish_pipe", GOLDFISH_PIPE_IOMEM_BASE,
                         x86ms->gsi[GOLDFISH_PIPE_IRQ]);
  }

  if (ams->enable_goldfish_sync) {
    create_simple_device("goldfish_sync", GOLDFISH_SYNC_IOMEM_BASE,
                         x86ms->gsi[GOLDFISH_SYNC_IRQ]);
  }

  // Make sure we override dsdt function, as we want to configure the board
  // properly.
  pcms->add_goldfish_dsdt = add_goldfish_dsdt;
}

/** Various property setters */

#define goldfish_set_str(member)                                               \
  static void goldfish_set_##member(Object *obj, const char *value,            \
                                    Error **errp) {                            \
    GoldfishMachineState *ams = ANDROID_MACHINE(obj);                          \
    g_free(ams->member);                                                       \
    ams->member = g_strdup(value);                                             \
  }

goldfish_set_str(acpi_ini);
goldfish_set_str(system_device_in_guest);
goldfish_set_str(vendor_device_in_guest);

static bool goldfish_get_pipe(Object *obj, Error **errp) {
  GoldfishMachineState *ams = ANDROID_MACHINE(obj);
  return ams->enable_goldfish_pipe;
}

static void goldfish_set_pipe(Object *obj, bool value, Error **errp) {
  GoldfishMachineState *ams = ANDROID_MACHINE(obj);
  ams->enable_goldfish_pipe = value;
}

static bool goldfish_get_sync(Object *obj, Error **errp) {
  GoldfishMachineState *ams = ANDROID_MACHINE(obj);
  return ams->enable_goldfish_sync;
}

static void goldfish_set_sync(Object *obj, bool value, Error **errp) {
  GoldfishMachineState *ams = ANDROID_MACHINE(obj);
  ams->enable_goldfish_sync = value;
}

static int x86_kvm_type(MachineState *ms, const char *vm_type)
{
    // Should this use kvm_get_vm_type?
    return 0;
}

static void goldfish_machine_std_class_init(ObjectClass *oc, const void *data) {
  MachineClass *mc = MACHINE_CLASS(oc);

  // Enable the loading of the acpi ini file.
  object_class_property_add_str(oc, "acpi_ini", NULL, goldfish_set_acpi_ini);
  object_class_property_add_str(oc, "system", NULL,
                                goldfish_set_system_device_in_guest);
  object_class_property_add_str(oc, "vendor", NULL,
                                goldfish_set_vendor_device_in_guest);
  object_class_property_add_bool(oc, "goldfish-pipe", goldfish_get_pipe,
                                 goldfish_set_pipe);
  object_class_property_add_bool(oc, "goldfish-sync", goldfish_get_sync,
                                 goldfish_set_sync);

  android_machine_options(mc);
  mc->init = pc_init_goldfish;
  mc->kvm_type = x86_kvm_type;
}

static const TypeInfo goldfish_machine_type_std = {
    .name = "goldfish" TYPE_MACHINE_SUFFIX,
    .parent = TYPE_PC_MACHINE,
    .class_init = goldfish_machine_std_class_init,
    .instance_size = sizeof(GoldfishMachineState),
    .interfaces = (InterfaceInfo[]){{TYPE_HOTPLUG_HANDLER}, {}},
};

static void goldfish_machine_init_std(void) {
  type_register_static(&goldfish_machine_type_std);
}
type_init(goldfish_machine_init_std)
