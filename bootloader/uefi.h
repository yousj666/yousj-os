/* Yousj OS - minimal UEFI definitions for the bootloader.
 *
 * Written from scratch against the UEFI specification's published
 * layouts (structure offsets and GUID values are facts of the UEFI
 * interface, not code taken from any existing implementation).
 * Deliberately the smallest subset the loader needs: system and
 * boot-services tables, console output, LoadedImage, the simple
 * file system, and the memory descriptor. No external headers, no
 * gnu-efi: the from-scratch rule applies to the bootloader too.
 *
 * ABI warning: UEFI services use the Microsoft x64 calling
 * convention, NOT the System V convention the rest of Yousj is
 * built with. Every firmware callable below is therefore declared
 * EFIAPI (ms_abi). Forgetting that on one prototype means garbage
 * arguments at runtime, so there are no unannotated firmware
 * pointers in this file.
 */
#ifndef YOUSJ_BOOTLOADER_UEFI_H
#define YOUSJ_BOOTLOADER_UEFI_H

#include <yousj/types.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t efi_status_t;
typedef void    *efi_handle_t;
typedef uint16_t efi_char16_t;      /* one UCS-2 code unit */

#define EFI_SUCCESS              0ULL
#define EFI_ERROR_BIT            (1ULL << 63)
#define EFI_ERROR(s)             (((s) & EFI_ERROR_BIT) != 0)
#define EFI_LOAD_ERROR           (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER    (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED          (EFI_ERROR_BIT | 3)
#define EFI_BUFFER_TOO_SMALL     (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND            (EFI_ERROR_BIT | 14)

struct efi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
};

/* GUIDs (UEFI specification assigned values). */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11D2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964E5B22, 0x6459, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }

/* ---- memory ---------------------------------------------------- */

/* EFI_MEMORY_DESCRIPTOR (version 1). Field offsets are fixed by
 * the spec: note the 4 bytes of padding after `type` so that
 * physical_start lands on an 8-byte boundary. The stride between
 * descriptors in a real map is firmware-supplied and may exceed
 * sizeof of this struct; never stride by sizeof. */
struct efi_memory_descriptor {
    uint32_t type;
    uint32_t _pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};
#define EFI_MEMORY_DESCRIPTOR_VERSION 1
#define EFI_PAGE_SIZE 4096ULL

/* AllocatePages allocation types. */
#define EFI_ALLOCATE_ANY_PAGES   0
#define EFI_ALLOCATE_MAX_ADDRESS 1
#define EFI_ALLOCATE_ADDRESS     2

/* Memory types the loader itself allocates with. */
#define EFI_LOADER_CODE 1
#define EFI_LOADER_DATA 2

/* ---- console output -------------------------------------------- */

struct efi_simple_text_output_protocol {
    void         *_reset;
    efi_status_t (EFIAPI *output_string)(void *self,
                                         const efi_char16_t *string);
    void *_test_string;
    void *_query_mode;
    void *_set_mode;
    void *_set_attribute;
    void *_clear_screen;
    void *_set_cursor_position;
    void *_enable_cursor;
    void *_mode;
};

/* ---- file access ----------------------------------------------- */

struct efi_file_protocol;

struct efi_simple_file_system_protocol {
    uint64_t revision;
    efi_status_t (EFIAPI *open_volume)(
        struct efi_simple_file_system_protocol *self,
        struct efi_file_protocol **root);
};

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_FILE_POSITION_END 0xFFFFFFFFFFFFFFFFULL

struct efi_file_protocol {
    uint64_t revision;
    efi_status_t (EFIAPI *open)(struct efi_file_protocol *self,
                                struct efi_file_protocol **new_handle,
                                const efi_char16_t *file_name,
                                uint64_t open_mode, uint64_t attributes);
    efi_status_t (EFIAPI *close)(struct efi_file_protocol *self);
    void *_delete;
    efi_status_t (EFIAPI *read)(struct efi_file_protocol *self,
                                uint64_t *buffer_size, void *buffer);
    void *_write;
    efi_status_t (EFIAPI *get_position)(struct efi_file_protocol *self,
                                         uint64_t *position);
    efi_status_t (EFIAPI *set_position)(struct efi_file_protocol *self,
                                         uint64_t position);
    void *_get_info;
    void *_set_info;
    void *_flush;
};

/* ---- loaded image ---------------------------------------------- */

struct efi_loaded_image_protocol {
    uint32_t revision;
    uint32_t _pad;
    efi_handle_t parent_handle;
    void        *system_table;
    efi_handle_t device_handle;
    void        *file_path;     /* EFI_DEVICE_PATH_PROTOCOL, unused */
    void        *reserved;
    uint32_t     load_options_size;  /* bytes, not characters */
    uint32_t     _pad2;
    void        *load_options;       /* CHAR16 string */
    void        *image_base;
    uint64_t     image_size;
    uint32_t     image_code_type;
    uint32_t     image_data_type;
    void        *_unload;
};

/* ---- boot services --------------------------------------------- */
/* Only fields up to exit_boot_services are modelled; the offsets
 * are pinned by the spec's table layout (8 bytes per pointer after
 * the 24-byte table header). Slots the loader does not call are
 * named with a leading underscore and never dereferenced. */

struct efi_boot_services {
    /* EFI_TABLE_HEADER */
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
    /* Task priority */
    void *_raise_tpl;
    void *_restore_tpl;
    /* Memory */
    efi_status_t (EFIAPI *allocate_pages)(uint32_t type,
                                          uint32_t memory_type,
                                          uint64_t pages,
                                          uint64_t *memory);
    efi_status_t (EFIAPI *free_pages)(uint64_t memory, uint64_t pages);
    efi_status_t (EFIAPI *get_memory_map)(uint64_t *memory_map_size,
                                          struct efi_memory_descriptor *memory_map,
                                          uint64_t *map_key,
                                          uint64_t *descriptor_size,
                                          uint32_t *descriptor_version);
    efi_status_t (EFIAPI *allocate_pool)(uint32_t pool_type,
                                         uint64_t size, void **buffer);
    efi_status_t (EFIAPI *free_pool)(void *buffer);
    /* Events (unused) */
    void *_create_event;
    void *_set_timer;
    void *_wait_for_event;
    void *_signal_event;
    void *_close_event;
    void *_check_event;
    /* Protocols */
    void *_install_protocol_interface;
    void *_reinstall_protocol_interface;
    void *_uninstall_protocol_interface;
    efi_status_t (EFIAPI *handle_protocol)(efi_handle_t handle,
                                           const struct efi_guid *protocol,
                                           void **interface);
    void *_reserved2;
    void *_register_protocol_notify;
    void *_locate_handle;
    void *_locate_device_path;
    void *_install_configuration_table;
    /* Images */
    void *_load_image;
    void *_start_image;
    void *_exit;
    void *_unload_image;
    efi_status_t (EFIAPI *exit_boot_services)(efi_handle_t image_handle,
                                              uint64_t map_key);
};

/* ---- system table ---------------------------------------------- */

struct efi_configuration_table {
    struct efi_guid guid;
    void *vendor_table;
};

struct efi_system_table {
    /* EFI_TABLE_HEADER */
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
    efi_char16_t *firmware_vendor;
    uint32_t firmware_revision;
    uint32_t _pad;
    efi_handle_t console_in_handle;
    void *con_in;
    efi_handle_t console_out_handle;
    struct efi_simple_text_output_protocol *con_out;
    efi_handle_t standard_error_handle;
    void *std_err;
    void *runtime_services;
    struct efi_boot_services *boot_services;
    uint64_t number_of_table_entries;
    struct efi_configuration_table *configuration_table;
};

#endif /* YOUSJ_BOOTLOADER_UEFI_H */
