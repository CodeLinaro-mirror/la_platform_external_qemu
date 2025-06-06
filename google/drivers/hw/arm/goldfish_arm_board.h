#ifndef HW_ARM_GOLDFISH_BOARD_H
#define HW_ARM_GOLDFISH_BOARD_H

#include "qemu/osdep.h"
#include "qemu/units.h"

#include "hw/boards.h"
#include "hw/arm/virt.h"

typedef struct GoldfishMachineState {
  /*< private >*/
  VirtMachineState pc;

  /* <public> */

  /* True if we should use virtio */
  bool use_virtio_console;

  /* True if we should use a dynamic partition */
  bool dynamic_partition;

  /* The path of system.img in the guest */
  char* system_device_in_guest;

  /* The path of the vendor.img in the guest (if any) */
  char* vendor_device_in_guest;
} GoldfishMachineState;

#define TYPE_ANDROID_MACHINE "goldfish-arm-machine"
OBJECT_DECLARE_TYPE(GoldfishMachineState, AndroidMachineClass, ANDROID_MACHINE)

#endif