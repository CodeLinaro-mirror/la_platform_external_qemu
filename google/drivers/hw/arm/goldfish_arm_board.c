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

#include "google/drivers/hw/arm/goldfish_arm_board.h"

#include "hw/arm/fdt.h"
#include "hw/arm/virt.h"
#include "hw/pci/pci.h"
#include "qemu/error-report.h"
#include "sysemu/device_tree.h"

static const int a15irqmap[] = {
    [VIRT_UART] = 1,
    [VIRT_RTC] = 2,
    [VIRT_PCIE] = 3, /* ... to 6 */
    [VIRT_GPIO] = 7,
    [VIRT_SECURE_UART] = 8,
    [VIRT_ACPI_GED] = 9,
    //[RANCHU_GOLDFISH_FB] = 16,
    [RANCHU_GOLDFISH_BATTERY] = 17,
    //[RANCHU_GOLDFISH_AUDIO] = 18,
    //[RANCHU_GOLDFISH_EVDEV] = 19,
    //[RANCHU_GOLDFISH_PIPE] = 20,
    //[RANCHU_GOLDFISH_SYNC] = 21,
    [VIRT_MMIO] = 32,          /* ...to 16 + NUM_VIRTIO_TRANSPORTS - 1 */
    [VIRT_GIC_V2M] = 64,       /* ...to 48 + NUM_GICV2M_SPIS - 1 */
    [VIRT_PLATFORM_BUS] = 128, /* ...to 112 + PLATFORM_BUS_NUM_IRQS -1 */
};

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


static void init_simple_device(DeviceState *dev, const VirtMachineState *vms,
                               int devid, const char *sysbus_name,
                               const char *compat, int num_compat_strings,
                               const char *clocks, int num_clocks) {
  MachineState *ms = MACHINE(vms);
  int irq = vms->irqmap[devid];
  hwaddr base = vms->memmap[devid].base;
  hwaddr size = vms->memmap[devid].size;
  char *nodename;
  int i;
  int compat_sz = 0;
  int clocks_sz = 0;

  sysbus_create_simple(sysbus_name, base, qdev_get_gpio_in(vms->gic, irq));

  for (i = 0; i < num_compat_strings; i++) {
    compat_sz += strlen(compat + compat_sz) + 1;
  }

  for (i = 0; i < num_clocks; i++) {
    clocks_sz += strlen(clocks + clocks_sz) + 1;
  }

  nodename = g_strdup_printf("/%s@%" PRIx64, sysbus_name, base);
  qemu_fdt_add_subnode(ms->fdt, nodename);
  qemu_fdt_setprop(ms->fdt, nodename, "compatible", compat, compat_sz);
  qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "reg", 2, base, 2, size);
  if (irq) {
    qemu_fdt_setprop_cells(ms->fdt, nodename, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, irq,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
  }
  if (num_clocks) {
    qemu_fdt_setprop_cells(ms->fdt, nodename, "clocks", vms->clock_phandle,
                           vms->clock_phandle);
    qemu_fdt_setprop(ms->fdt, nodename, "clock-names", clocks, clocks_sz);
  }
  g_free(nodename);
}

static void create_simple_device(const VirtMachineState *vms, int devid,
                                 const char *sysbus_name, const char *compat,
                                 int num_compat_strings, const char *clocks,
                                 int num_clocks) {
  DeviceState *dev = qdev_new(sysbus_name);
  init_simple_device(dev, vms, devid, sysbus_name, compat, num_compat_strings,
                     clocks, num_clocks);
}

static void arm_init_goldfish(MachineState *machine) {
  GSList *machines = object_class_get_list(TYPE_MACHINE, false);
  MachineClass *machine_class;
  machine_class = find_machine("virt-8.2", machines);
  machine_class->init(machine);

  VirtMachineState *vms = VIRT_MACHINE(machine);

  if (machine_usb(machine)) {
    PCIBus *pci_bus =
        (PCIBus *)object_resolve_path_type("", TYPE_PCI_BUS, NULL);
    if (!pci_bus)
      error_report("No PCI bus available to add USB OHCI controller to.");
    else
      pci_create_simple(pci_bus, -1, "pci-ohci");
  }

  void *fdt = machine->fdt;
  qemu_fdt_setprop_string(fdt, "/", "compatible", "linux,ranchu");
  qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);
  qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);

  /* Firmware node */
  qemu_fdt_add_subnode(fdt, "/firmware");
  qemu_fdt_add_subnode(fdt, "/firmware/android");
  qemu_fdt_setprop_string(fdt, "/firmware/android", "compatible",
                          "android,firmware");
  qemu_fdt_setprop_string(fdt, "/firmware/android", "hardware", "ranchu");


  // Device tree fstab seems not to be needed with the dynamic partition boot method.
  // if (!ams->dynamic_partition) {
  //   goldfish_add_fstab(fdt, ams->system_device_in_guest,
  //                      ams->vendor_device_in_guest);
  // }

  create_simple_device(vms, RANCHU_GOLDFISH_BATTERY, "goldfish_battery",
                       "google,goldfish-battery\0"
                       "generic,goldfish-battery",
                       2, 0, 0);

#if defined(__linux__) && defined(__aarch64__)
  /* Default GIC type is host: use the same version as the host */
  machine->gic_version = 0;
#endif
}

static void goldfish_instance_init(Object *obj)
{
  VirtMachineState *vms = VIRT_MACHINE(obj);
  vms->irqmap = a15irqmap;
}

static void goldfish_machine_options(MachineClass *mc) {
  mc->desc = "Android Arm (Ranchu + virt-8.2) Device";
  mc->init = arm_init_goldfish;
}

static void goldfish_set_system_device_in_guest(Object *obj, const char *value,
                                                Error **errp) {
  GoldfishMachineState *ams = ANDROID_MACHINE(obj);

  g_free(ams->system_device_in_guest);
  ams->system_device_in_guest = g_strdup(value);
}

static void goldfish_set_vendor_device_in_guest(Object *obj, const char *value,
                                                Error **errp) {
  GoldfishMachineState *ams = ANDROID_MACHINE(obj);

  g_free(ams->vendor_device_in_guest);
  ams->vendor_device_in_guest = g_strdup(value);
}

static void goldfish_machine_std_class_init(ObjectClass *oc, void *data) {
  MachineClass *mc = MACHINE_CLASS(oc);
  object_class_property_add_str(oc, "system", NULL,
                                goldfish_set_system_device_in_guest);
  object_class_property_add_str(oc, "vendor", NULL,
                                goldfish_set_vendor_device_in_guest);
  goldfish_machine_options(mc);
}

static const TypeInfo goldfish_machine_type_std = {
    .name = TYPE_ANDROID_MACHINE,
    .parent = TYPE_VIRT_MACHINE,
    .class_init = goldfish_machine_std_class_init,
    .instance_size = sizeof(GoldfishMachineState),
    .instance_init = goldfish_instance_init,
    .interfaces = (InterfaceInfo[]) {
         { TYPE_HOTPLUG_HANDLER },
         { }
    },
};

static void goldfish_machine_init_std(void) {
  type_register(&goldfish_machine_type_std);
}
type_init(goldfish_machine_init_std)
