/*
 * MIPI HCI I3C controller
 *
 * Copyright (C) 2025 Google, LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/registerfields.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "hw/i3c/hci-core.h"
#include "hci-core-internal.h"
#include "hw/i3c/hci-dma.h"
#include "hci-dma-internal.h"
#include "hw/i3c/hci-ext.h"
#include "hci-ext-internal.h"
#include "hci-dat-internal.h"
#include "hci-dct-internal.h"
#include "trace.h"
#include "hw/i3c/i3c.h"
#include "hw/i3c/mipi-hci.h"
#include "hw/core/irq.h"

static const MemoryRegionOps hci_core_ops = {
    .read = hci_core_read,
    .write = hci_core_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps hci_dma_ops = {
    .read = hci_dma_read,
    .write = hci_dma_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps hci_dma_header_ops = {
    .read = hci_dma_header_read,
    .write = hci_dma_header_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps hci_ext_caps_ops = {
    .read = hci_ext_read,
    .write = hci_ext_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps hci_dat_ops = {
    .read = hci_dat_read,
    .write = hci_dat_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const MemoryRegionOps hci_dct_ops = {
    .read = hci_dct_read,
    .write = hci_dct_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void mipi_hci_instance_init(Object *obj)
{
}

static const Property mipi_hci_properties[] = {
    DEFINE_PROP_UINT32("ring-header-section-offset", MIPIHCIState,
                       core.cfg.ring_header_section_offset, 0),
    DEFINE_PROP_ARRAY("ring-offsets", MIPIHCIState,
                      dma.cfg.num_ring_offsets, dma.cfg.ring_offsets,
                      qdev_prop_uint32, uint32_t),
    DEFINE_PROP_ARRAY("ext-capabilities", MIPIHCIState,
                      ext_cap.num_ext_capabilities, ext_cap.ext_capabilities,
                      qdev_prop_uint32, uint32_t),
    DEFINE_PROP_UINT32("ext-caps-section-offset", MIPIHCIState,
                       core.cfg.ext_caps_section_offset, 0),
    DEFINE_PROP_UINT32("dat-table-size", MIPIHCIState, core.cfg.dat_table_size,
                       0),
    DEFINE_PROP_UINT32("dat-table-offset", MIPIHCIState,
                       core.cfg.dat_table_offset, 0),
    DEFINE_PROP_UINT32("dct-table-size", MIPIHCIState, core.cfg.dct_table_size,
                        0),
    DEFINE_PROP_UINT32("dct-table-offset", MIPIHCIState,
                        core.cfg.dct_table_offset, 0),
    DEFINE_PROP_UINT32("hci-version", MIPIHCIState,
                       core.cfg.hci_version, 0),
    DEFINE_PROP_UINT32("hc-capabilities", MIPIHCIState,
                       core.cfg.hc_capabilities, 0),
    DEFINE_PROP_UINT32("int-ctrl-cmds-en", MIPIHCIState,
                        core.cfg.int_ctrl_cmds_en, 0),
    DEFINE_PROP_UINT32("preamble-size", MIPIHCIState,
                       dma.cfg.preamble_size, 0),
    DEFINE_PROP_UINT32("header-size", MIPIHCIState,
                       dma.cfg.header_size, 0),
    DEFINE_PROP_UINT32("xfer-struct-size", MIPIHCIState,
                       dma.cfg.xfer_struct_size, 0),
    DEFINE_PROP_UINT32("resp-struct-size", MIPIHCIState,
                       dma.cfg.resp_struct_size, 0),
    DEFINE_PROP_UINT32("ibi-stat", MIPIHCIState,
                       dma.cfg.ibi_status_struct_size, 0),
};

static void mipi_hci_realize(DeviceState *dev, Error **errp)
{
    MIPIHCIState *s = MIPI_HCI(dev);
    HCICoreState *core = &s->core;
    HCIDMAState *dma = &s->dma;
    HCIExtCapState *ext_caps = &s->ext_cap;
    HCIDATState *dat = &s->dat;
    HCIDCTState *dct = &s->dct;

    memory_region_init(&s->iomem, OBJECT(s), TYPE_MIPI_HCI"-mmio",
                       MIPI_HCI_MMIO_SIZE);
    memory_region_init_io(&core->iomem, OBJECT(s), &hci_core_ops, s,
                          TYPE_MIPI_HCI"-core-mmio",
                          HCI_CORE_NUM_REGS * sizeof(uint32_t));
    memory_region_add_subregion(&s->iomem, HCI_CORE_MMIO_OFFSET,
                                &core->iomem);
    dma->rh_mmio = g_new0(MemoryRegion, dma->cfg.num_ring_offsets);
    memory_region_init_io(&dma->header_mmio, OBJECT(s), &hci_dma_header_ops, s,
                           TYPE_MIPI_HCI"-dma-header-mmio",
                           HCI_DMA_HEADER_NUM_REGS * sizeof(uint32_t));
    memory_region_add_subregion(&s->iomem,
                                core->cfg.ring_header_section_offset,
                                &dma->header_mmio);
    for (int i = 0; i < dma->cfg.num_ring_offsets; ++i) {
        g_autofree char *mr_name = g_strdup_printf("%s-dma-%d-mmio",
                                                   TYPE_MIPI_HCI, i);
        memory_region_init_io(&dma->rh_mmio[i], OBJECT(s), &hci_dma_ops, s,
                              mr_name, HCI_DMA_NUM_REGS * sizeof(uint32_t));
        memory_region_add_subregion(&s->iomem, dma->cfg.ring_offsets[i],
                                    &dma->rh_mmio[i]);
    }
    memory_region_init_io(&ext_caps->mmio, OBJECT(s), &hci_ext_caps_ops, s,
                                 TYPE_MIPI_HCI"-ext-caps-mmio",
                                 ext_caps->num_ext_capabilities *
                                 sizeof(uint32_t));
    memory_region_add_subregion(&s->iomem, core->cfg.ext_caps_section_offset,
                                        &ext_caps->mmio);
    memory_region_init_io(&dat->mmio, OBJECT(s), &hci_dat_ops, s,
                          TYPE_MIPI_HCI"-dat-mmio",
                          core->cfg.dat_table_size * sizeof(uint32_t) *
                          HCI_DAT_ENTRY_SIZE);
    memory_region_add_subregion(&s->iomem, core->cfg.dat_table_offset,
                                &dat->mmio);
    memory_region_init_io(&dct->mmio, OBJECT(s), &hci_dct_ops, s,
                          TYPE_MIPI_HCI"-dct-mmio",
                          core->cfg.dct_table_size * sizeof(uint32_t));
    memory_region_add_subregion(&s->iomem, core->cfg.dct_table_offset,
                                &dct->mmio);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->bus = i3c_init_bus(DEVICE(s), NULL);
}

static void mipi_hci_enter_reset(Object *obj, ResetType type)
{
    MIPIHCIState *s = MIPI_HCI(obj);

    hci_core_reset(&s->core);
    hci_dma_reset(&s->dma);
    hci_dat_reset(&s->dat, s->core.cfg.dat_table_size);
    hci_dct_reset(&s->dct, s->core.cfg.dct_table_size);
}

static void mipi_hci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = mipi_hci_enter_reset;
    dc->realize = mipi_hci_realize;
    dc->desc = "MIPI HCI I3C Controller";
    device_class_set_props(dc, mipi_hci_properties);
}

static const TypeInfo mipi_hci_info = {
    .name = TYPE_MIPI_HCI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = mipi_hci_instance_init,
    .instance_size = sizeof(MIPIHCIState),
    .class_init = mipi_hci_class_init,
    .class_size = sizeof(MIPIHCIClass),
};

static void mipi_hci_register_types(void)
{
    type_register_static(&mipi_hci_info);
}

type_init(mipi_hci_register_types);
