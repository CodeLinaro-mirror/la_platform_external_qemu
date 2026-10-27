#ifndef AEMU_FUNCS_DEFS_H
#define AEMU_FUNCS_DEFS_H

#ifdef __cplusplus
extern "C" {
#endif

#define GF_REGISTER_TYPES_FUNC goldfish_register_types
#define GF_STARTUP_FUNC        goldfish_plugin_startup
#define GF_SHUTDOWN_FUNC       goldfish_plugin_shutdown

struct AddressSpace;

struct AddressSpace *get_qemu_system_address_space(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* AEMU_FUNCS_DEFS_H */
