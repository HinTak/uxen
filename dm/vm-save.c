/*
 * Copyright 2012-2015, Bromium, Inc.
 * Author: Christian Limpach <Christian.Limpach@gmail.com>
 * SPDX-License-Identifier: ISC
 */

/* 
 *  vm-save.c
 *  uxen dm
 *
 *  COPYRIGHT
 *
 */


#include "config.h"

#include <err.h>
#include <inttypes.h>
#include <stdint.h>

#include <uuid/uuid.h>

#include "async-op.h"
#include "bitops.h"
#include "control.h"
#include "dm.h"
#include "dmpdev.h"
#include "dmreq.h"
#include "filebuf.h"
#include "introspection_info.h"
#include "monitor.h"
#include "qemu_savevm.h"
#include "vm.h"
#include "vm-save.h"
#include "uxen.h"
#include "hw/uxen_platform.h"
#include "mapcache.h"

#include <lz4.h>
#include <lz4hc.h>

#include <xenctrl.h>
#include <xc_private.h>

#include <xen/hvm/e820.h>

#define SAVE_FORMAT_VERSION 4

#define DECOMPRESS_THREADED
#define DECOMPRESS_THREADS 2

#ifdef DEBUG
#define VERBOSE 1
#endif
// #define VERBOSE_SAVE 1
// #define VERBOSE_LOAD 1

#undef DPRINTF
#ifdef VERBOSE
#define DPRINTF(fmt, ...) debug_printf(fmt "\n", ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { ; } while(0)
#endif
#ifdef VERBOSE_SAVE
#define SAVE_DPRINTF(fmt, ...) debug_printf(fmt "\n", ## __VA_ARGS__)
#else
#define SAVE_DPRINTF(fmt, ...) do { ; } while(0)
#endif
#ifdef VERBOSE_LOAD
#define LOAD_DPRINTF(fmt, ...) debug_printf(fmt "\n", ## __VA_ARGS__)
#else
#define LOAD_DPRINTF(fmt, ...) do { ; } while(0)
#endif
#undef APRINTF
#define APRINTF(fmt, ...) debug_printf(fmt "\n", ## __VA_ARGS__)
#undef EPRINTF
#define EPRINTF(fmt, ...) error_printf("%s: " fmt "\n", __FUNCTION__, \
                                       ## __VA_ARGS__)

// #include <xg_save_restore.h>
#define XC_SAVE_ID_VCPU_INFO          -2 /* Additional VCPU info */
#define XC_SAVE_ID_HVM_IDENT_PT       -3 /* (HVM-only) */
#define XC_SAVE_ID_HVM_VM86_TSS       -4 /* (HVM-only) */
#define XC_SAVE_ID_TSC_INFO           -7
#define XC_SAVE_ID_HVM_CONSOLE_PFN    -8 /* (HVM-only) */
#define XC_SAVE_ID_HVM_ACPI_IOPORTS_LOCATION -10
#define XC_SAVE_ID_HVM_MAGIC_PFNS     -11
#define XC_SAVE_ID_HVM_CONTEXT        -12
#define XC_SAVE_ID_HVM_DM             -13
#define XC_SAVE_ID_VM_UUID            -14
#define XC_SAVE_ID_VM_TEMPLATE_UUID   -15
#define XC_SAVE_ID_VERSION            -16
#define XC_SAVE_ID_HVM_INTROSPEC      -17
#define XC_SAVE_ID_MAPCACHE_PARAMS    -18
#define XC_SAVE_ID_VM_TEMPLATE_FILE   -19
#define XC_SAVE_ID_PAGE_OFFSETS       -20
#define XC_SAVE_ID_ZERO_BITMAP        -21

struct vm_save_info vm_save_info = { };

static int
uxenvm_savevm_initiate(char **err_msg)
{
    int ret;

    ret = xc_domain_shutdown(xc_handle, vm_id, SHUTDOWN_suspend);
    if (ret)
	asprintf(err_msg, "xc_domain_shutdown(SHUTDOWN_suspend) failed: %d",
		 ret);

    return ret;
}

struct xc_save_generic {
    int32_t marker;
    uint32_t size;
};

struct xc_save_version_info {
    int32_t marker;
    uint32_t version;
};

struct xc_save_tsc_info {
    int32_t marker;
    uint32_t tsc_mode;
    uint64_t nsec;
    uint32_t khz;
    uint32_t incarn;
};

struct xc_save_vcpu_info {
    int32_t marker;
    int max_vcpu_id;
    uint64_t vcpumap;
};

struct xc_save_hvm_generic_chunk {
    int32_t marker;
    uint32_t pad;
    uint64_t data;
};

struct xc_save_hvm_magic_pfns {
    int32_t marker;
    uint64_t magic_pfns[5];
};

struct xc_save_hvm_context {
    int32_t marker;
    uint32_t size;
    uint8_t context[];
};

struct xc_save_hvm_dm {
    int32_t marker;
    uint32_t size;
    uint8_t state[];
};

struct xc_save_vm_uuid {
    int32_t marker;
    uint8_t uuid[16];
};

struct xc_save_vm_template_uuid {
    int32_t marker;
    uint8_t uuid[16];
};

struct xc_save_hvm_introspec {
    int32_t marker;
    struct guest_introspect_info_header info;
};

struct xc_save_mapcache_params {
    int32_t marker;
    uint32_t end_low_pfn;
    uint32_t start_high_pfn;
    uint32_t end_high_pfn;
};

struct xc_save_vm_template_file {
    int32_t marker;
    uint16_t size;
    char file[];
};

struct xc_save_vm_page_offsets {
    struct xc_save_generic;

    uint32_t pfn_off_nr;
    uint64_t pfn_off[];
};

struct xc_save_zero_bitmap {
    struct xc_save_generic;

    uint32_t zero_bitmap_size;
    uint8_t data[];
};

struct PACKED xc_save_index {
    uint64_t offset;
    int32_t marker;             /* marker field last such that the
                                 * regular end marker also doubles as
                                 * an index end marker */
};

#define MAX_BATCH_SIZE 1023

typedef uint16_t cs16_t;

#define PP_BUFFER_PAGES                                                 \
    (int)((MAX_BATCH_SIZE * (sizeof(cs16_t) + PAGE_SIZE) + PAGE_SIZE - 1) \
          >> PAGE_SHIFT)

#define PCI_HOLE_START_PFN (PCI_HOLE_START >> UXEN_PAGE_SHIFT)
#define PCI_HOLE_END_PFN (PCI_HOLE_END >> UXEN_PAGE_SHIFT)
#define skip_pci_hole(pfn) ((pfn) < PCI_HOLE_END_PFN ?                  \
                            (pfn) :                                     \
                            (pfn) - (PCI_HOLE_END_PFN - PCI_HOLE_START_PFN))
#define poi_valid_pfn(poi, pfn) ((pfn) < (poi)->max_gpfn &&      \
                                 ((pfn) < PCI_HOLE_START_PFN ||  \
                                  (pfn) >= PCI_HOLE_END_PFN))
#define poi_pfn_index(poi, pfn) skip_pci_hole(pfn)

struct page_offset_info {
    uint32_t max_gpfn;
    uint64_t *pfn_off;
    struct filebuf *fb;
};
#define PAGE_OFFSET_INDEX_PFN_OFF_COMPRESSED (1ULL << 63)
#define PAGE_OFFSET_INDEX_PFN_OFF_MASK (~(PAGE_OFFSET_INDEX_PFN_OFF_COMPRESSED))

static struct page_offset_info dm_lazy_load_info = { };

#define uxenvm_read_struct_size(s) (sizeof(*(s)) - sizeof(marker))
#define uxenvm_read_struct(f, s)                                        \
    filebuf_read(f, (uint8_t *)(s) + sizeof(marker),                    \
                 uxenvm_read_struct_size(s))

static int
uxenvm_savevm_get_dm_state(uint8_t **dm_state_buf, int *dm_state_size,
                           char **err_msg)
{
    QEMUFile *mf;
    int ret = 0;

    mf = qemu_memopen(NULL, 0, "wb");
    if (mf == NULL) {
        asprintf(err_msg, "qemu_memopen() failed");
        ret = EPERM;
        goto out;
    }

    ret = qemu_savevm_state(NULL, mf);
    if (ret < 0) {
        asprintf(err_msg, "qemu_savevm_state() failed");
        ret = EPERM;
        goto out;
    }

    *dm_state_buf = qemu_meminfo(mf, dm_state_size);
    if (*dm_state_buf == NULL) {
        asprintf(err_msg, "qemu_meminfo() failed");
        ret = EPERM;
        goto out;
    }
  out:
    if (mf)
        qemu_fclose(mf);
    return ret;
}

static int
uxenvm_savevm_write_info(struct filebuf *f, uint8_t *dm_state_buf,
                         int dm_state_size, char **err_msg)
{
    int32_t hvm_buf_size;
    uint8_t *hvm_buf = NULL;
    xc_dominfo_t dom_info[1];
    xc_vcpuinfo_t vcpu_info;
    struct xc_save_version_info s_version_info;
    struct xc_save_tsc_info s_tsc_info;
    struct xc_save_vcpu_info s_vcpu_info;
    struct xc_save_hvm_generic_chunk s_hvm_ident_pt;
    struct xc_save_hvm_generic_chunk s_hvm_vm86_tss;
    struct xc_save_hvm_generic_chunk s_hvm_console_pfn;
    struct xc_save_hvm_generic_chunk s_hvm_acpi_ioports_location;
    struct xc_save_hvm_magic_pfns s_hvm_magic_pfns;
    struct xc_save_hvm_context s_hvm_context;
    struct xc_save_hvm_dm s_hvm_dm;
    struct xc_save_vm_uuid s_vm_uuid;
    struct xc_save_vm_template_uuid s_vm_template_uuid;
    struct xc_save_mapcache_params s_mapcache_params;
    struct xc_save_vm_template_file s_vm_template_file;
    int j;
    int ret;

    s_version_info.marker = XC_SAVE_ID_VERSION;
    s_version_info.version = SAVE_FORMAT_VERSION;
    filebuf_write(f, &s_version_info, sizeof(s_version_info));

    s_tsc_info.marker = XC_SAVE_ID_TSC_INFO;
    ret = xc_domain_get_tsc_info(xc_handle, vm_id, &s_tsc_info.tsc_mode,
				 &s_tsc_info.nsec, &s_tsc_info.khz,
				 &s_tsc_info.incarn);
    if (ret < 0) {
	asprintf(err_msg, "xc_domain_get_tsc_info() failed");
	ret = -EPERM;
	goto out;
    }
    APRINTF("tsc info: mode %d nsec %"PRIu64" khz %d incarn %d",
	    s_tsc_info.tsc_mode, s_tsc_info.nsec, s_tsc_info.khz,
	    s_tsc_info.incarn);
    filebuf_write(f, &s_tsc_info, sizeof(s_tsc_info));

    ret = xc_domain_getinfo(xc_handle, vm_id, 1, dom_info);
    if (ret != 1 || dom_info[0].domid != vm_id) {
	asprintf(err_msg, "xc_domain_getinfo(%d) failed", vm_id);
	ret = -EPERM;
	goto out;
    }
    s_vcpu_info.marker = XC_SAVE_ID_VCPU_INFO;
    s_vcpu_info.max_vcpu_id = dom_info[0].max_vcpu_id;
    s_vcpu_info.vcpumap = 0ULL;
    for (j = 0; j <= s_vcpu_info.max_vcpu_id; j++) {
	ret = xc_vcpu_getinfo(xc_handle, vm_id, j, &vcpu_info);
	if (ret == 0 && vcpu_info.online)
	    s_vcpu_info.vcpumap |= 1ULL << j;
    }
    APRINTF("vcpus %d online %"PRIx64, s_vcpu_info.max_vcpu_id,
	    s_vcpu_info.vcpumap);
    filebuf_write(f, &s_vcpu_info, sizeof(s_vcpu_info));

    s_hvm_ident_pt.marker = XC_SAVE_ID_HVM_IDENT_PT;
    s_hvm_ident_pt.data = 0;
    xc_get_hvm_param(xc_handle, vm_id, HVM_PARAM_IDENT_PT,
		     &s_hvm_ident_pt.data);
    APRINTF("ident_pt %"PRIx64, s_hvm_ident_pt.data);
    if (s_hvm_ident_pt.data)
	filebuf_write(f, &s_hvm_ident_pt, sizeof(s_hvm_ident_pt));

    s_hvm_vm86_tss.marker = XC_SAVE_ID_HVM_VM86_TSS;
    s_hvm_vm86_tss.data = 0;
    xc_get_hvm_param(xc_handle, vm_id, HVM_PARAM_VM86_TSS,
		     &s_hvm_vm86_tss.data);
    APRINTF("vm86_tss %"PRIx64, s_hvm_vm86_tss.data);
    if (s_hvm_vm86_tss.data)
	filebuf_write(f, &s_hvm_vm86_tss, sizeof(s_hvm_vm86_tss));

    s_hvm_console_pfn.marker = XC_SAVE_ID_HVM_CONSOLE_PFN;
    s_hvm_console_pfn.data = 0;
    xc_get_hvm_param(xc_handle, vm_id, HVM_PARAM_CONSOLE_PFN,
		     &s_hvm_console_pfn.data);
    APRINTF("console_pfn %"PRIx64, s_hvm_console_pfn.data);
    if (s_hvm_console_pfn.data)
	filebuf_write(f, &s_hvm_console_pfn, sizeof(s_hvm_console_pfn));

    s_hvm_acpi_ioports_location.marker = XC_SAVE_ID_HVM_ACPI_IOPORTS_LOCATION;
    s_hvm_acpi_ioports_location.data = 0;
    xc_get_hvm_param(xc_handle, vm_id, HVM_PARAM_ACPI_IOPORTS_LOCATION,
		     &s_hvm_acpi_ioports_location.data);
    APRINTF("acpi_ioports_location %"PRIx64, s_hvm_acpi_ioports_location.data);
    if (s_hvm_acpi_ioports_location.data)
	filebuf_write(f, &s_hvm_acpi_ioports_location,
                      sizeof(s_hvm_acpi_ioports_location));

    s_hvm_magic_pfns.marker = XC_SAVE_ID_HVM_MAGIC_PFNS;
    memset(s_hvm_magic_pfns.magic_pfns, 0, sizeof(s_hvm_magic_pfns.magic_pfns));
    xc_get_hvm_param(xc_handle, vm_id, HVM_PARAM_IO_PFN_FIRST,
		     &s_hvm_magic_pfns.magic_pfns[0]);
    xc_get_hvm_param(xc_handle, vm_id, HVM_PARAM_IO_PFN_LAST,
		     &s_hvm_magic_pfns.magic_pfns[1]);
    xc_get_hvm_param(xc_handle, vm_id, HVM_PARAM_SHARED_INFO_PFN,
		     &s_hvm_magic_pfns.magic_pfns[2]);
    xc_get_hvm_param(xc_handle, vm_id, HVM_PARAM_DMREQ_PFN,
                     &s_hvm_magic_pfns.magic_pfns[3]);
    xc_get_hvm_param(xc_handle, vm_id, HVM_PARAM_DMREQ_VCPU_PFN,
                     &s_hvm_magic_pfns.magic_pfns[4]);
    APRINTF("ioreq pfn %"PRIx64"-%"PRIx64
            " shared info pfn %"PRIx64" dmreq pfn %"PRIx64"/%"PRIx64,
            s_hvm_magic_pfns.magic_pfns[0], s_hvm_magic_pfns.magic_pfns[1],
            s_hvm_magic_pfns.magic_pfns[2], s_hvm_magic_pfns.magic_pfns[3],
            s_hvm_magic_pfns.magic_pfns[4]);
    filebuf_write(f, &s_hvm_magic_pfns, sizeof(s_hvm_magic_pfns));

    hvm_buf_size = xc_domain_hvm_getcontext(xc_handle, vm_id, 0, 0);
    if (hvm_buf_size == -1) {
	asprintf(err_msg, "xc_domain_hvm_getcontext(0, 0) failed");
	ret = -EPERM;
	goto out;
    }
    APRINTF("hvm_buf_size is %d", hvm_buf_size);

    hvm_buf = malloc(hvm_buf_size);
    if (hvm_buf == NULL) {
	asprintf(err_msg, "hvm_buf = malloc(%d) failed", hvm_buf_size);
	ret = -ENOMEM;
	goto out;
    }

    s_hvm_context.marker = XC_SAVE_ID_HVM_CONTEXT;
    s_hvm_context.size = xc_domain_hvm_getcontext(xc_handle, vm_id,
						  hvm_buf, hvm_buf_size);
    if (s_hvm_context.size == -1) {
	asprintf(err_msg, "xc_domain_hvm_getcontext(%d) failed", hvm_buf_size);
	ret = -EPERM;
	goto out;
    }
    APRINTF("hvm rec size %d", s_hvm_context.size);
    filebuf_write(f, &s_hvm_context, sizeof(s_hvm_context));
    filebuf_write(f, hvm_buf, s_hvm_context.size);

#if defined(_WIN32)
    /* "set_introspect_info" should be set for template only (last boot)*/
    if (strstr(lava_options, "set_introspect_info")) {
        struct guest_introspect_info_t *guest_introspect_info;
        guest_introspect_info = get_guest_introspect_info();
        if (guest_introspect_info) {
            struct xc_save_hvm_introspec s_hvm_introspec;
            int introspect_rect_size;
            s_hvm_introspec.marker = XC_SAVE_ID_HVM_INTROSPEC;
            s_hvm_introspec.info = guest_introspect_info->hdr;
            introspect_rect_size = s_hvm_introspec.info.n_immutable_ranges *
                sizeof(struct immutable_range);
            DPRINTF("introspect rec size %d", introspect_rect_size);
            filebuf_write(f, &s_hvm_introspec, sizeof(s_hvm_introspec));
            filebuf_write(f, guest_introspect_info->ranges,
                          introspect_rect_size);
        }
    }
#endif  /* _WIN32 */

    s_hvm_dm.marker = XC_SAVE_ID_HVM_DM;
    s_hvm_dm.size = dm_state_size;
    APRINTF("dm rec size %d", s_hvm_dm.size);
    filebuf_write(f, &s_hvm_dm, sizeof(s_hvm_dm));
    vm_save_info.dm_offset = filebuf_tell(f);
    filebuf_write(f, dm_state_buf, s_hvm_dm.size);

    s_vm_uuid.marker = XC_SAVE_ID_VM_UUID;
    memcpy(s_vm_uuid.uuid, vm_uuid, sizeof(s_vm_uuid.uuid));
    filebuf_write(f, &s_vm_uuid, sizeof(s_vm_uuid));

    if (vm_has_template_uuid) {
	s_vm_template_uuid.marker = XC_SAVE_ID_VM_TEMPLATE_UUID;
	memcpy(s_vm_template_uuid.uuid, vm_template_uuid,
	       sizeof(s_vm_template_uuid.uuid));
	filebuf_write(f, &s_vm_template_uuid, sizeof(s_vm_template_uuid));
    }

    s_mapcache_params.marker = XC_SAVE_ID_MAPCACHE_PARAMS;
    mapcache_get_params(&s_mapcache_params.end_low_pfn,
                        &s_mapcache_params.start_high_pfn,
                        &s_mapcache_params.end_high_pfn);
    filebuf_write(f, &s_mapcache_params, sizeof(s_mapcache_params));

    if (vm_template_file) {
        s_vm_template_file.marker = XC_SAVE_ID_VM_TEMPLATE_FILE;
        s_vm_template_file.size = strlen(vm_template_file);
        filebuf_write(f, &s_vm_template_file, sizeof(s_vm_template_file));
        filebuf_write(f, vm_template_file, s_vm_template_file.size);
    }

  out:
    return ret;
}

int
vm_save_read_dm_offset(void *dst, off_t offset, size_t size)
{
    int ret;
    off_t o;

    o = filebuf_tell(vm_save_info.f);
    offset += vm_save_info.dm_offset;
    filebuf_seek(vm_save_info.f, offset, FILEBUF_SEEK_SET);
    ret = filebuf_read(vm_save_info.f, dst, size);
    filebuf_seek(vm_save_info.f, o, FILEBUF_SEEK_SET);
    return ret;
}

static inline int
uxenvm_compress_lz4(const void *src, void *dst, int sz)
{
    return vm_save_info.high_compress ?
        LZ4_compressHC(src, dst, sz) :
        LZ4_compress(src, dst, sz);
}

static int
uxenvm_savevm_write_pages(struct filebuf *f, char **err_msg)
{
    uint8_t *hvm_buf = NULL;
    int p2m_size, pfn, batch, _batch, run, b_run, m_run, v_run, rezero, clone;
    int _zero;
    unsigned long batch_done;
    int total_pages = 0, total_zero = 0, total_rezero = 0, total_clone = 0;
    int total_compressed_pages = 0, total_compress_in_vain = 0;
    size_t total_compress_save = 0;
    int j;
    int *pfn_batch = NULL;
    uint8_t *zero_bitmap = NULL, *zero_bitmap_compressed = NULL;
    uint32_t zero_bitmap_size;
    struct xc_save_zero_bitmap s_zero_bitmap;
    char *compress_mem = NULL;
    char *compress_buf = NULL;
    uint32_t compress_size = 0;
    DECLARE_HYPERCALL_BUFFER(uint8_t, mem_buffer);
#define MEM_BUFFER_SIZE (MAX_BATCH_SIZE * PAGE_SIZE)
    xen_memory_capture_gpfn_info_t *gpfn_info_list = NULL;
    uint64_t mem_pos = 0, pos;
    struct page_offset_info poi;
    int rezero_nr = 0;
    xen_pfn_t *rezero_pfns = NULL;
    struct xc_save_vm_page_offsets s_vm_page_offsets;
    struct xc_save_index page_offsets_index = { 0, XC_SAVE_ID_PAGE_OFFSETS };
    int free_mem;
    int ret;

    free_mem = vm_save_info.free_mem;

    p2m_size = xc_domain_maximum_gpfn(xc_handle, vm_id);
    if (p2m_size < 0) {
	asprintf(err_msg, "xc_domain_maximum_gpfn() failed");
	ret = -EPERM;
	goto out;
    }
    p2m_size++;
    APRINTF("p2m_size: 0x%x", p2m_size);

    zero_bitmap_size = (p2m_size + 7) / 8;
    zero_bitmap = calloc(zero_bitmap_size, 1);
    if (zero_bitmap == NULL) {
        asprintf(err_msg, "zero_bitmap = calloc(%d) failed", zero_bitmap_size);
        ret = -ENOMEM;
        goto out;
    }

    gpfn_info_list = malloc(MAX_BATCH_SIZE * sizeof(*gpfn_info_list));
    if (gpfn_info_list == NULL) {
        asprintf(err_msg, "gpfn_info_list = malloc(%"PRIdSIZE") failed",
                 MAX_BATCH_SIZE * sizeof(*gpfn_info_list));
        ret = -ENOMEM;
        goto out;
    }

    mem_buffer = xc_hypercall_buffer_alloc_pages(
        xc_handle, mem_buffer, MEM_BUFFER_SIZE >> PAGE_SHIFT);
    if (!mem_buffer) {
        asprintf(err_msg, "mem_buffer = xc_hypercall_buffer_alloc_pages(%ld)"
                 " failed", MEM_BUFFER_SIZE >> PAGE_SHIFT);
        ret = -ENOMEM;
        goto out;
    }

    pfn_batch = malloc(MAX_BATCH_SIZE * sizeof(*pfn_batch));
    if (pfn_batch == NULL) {
        asprintf(err_msg, "pfn_batch = malloc(%"PRIdSIZE") failed",
                 MAX_BATCH_SIZE * sizeof(*pfn_batch));
	ret = -ENOMEM;
	goto out;
    }

    if (!free_mem) {
        rezero_pfns = malloc(MAX_BATCH_SIZE * sizeof(*rezero_pfns));
        if (rezero_pfns == NULL) {
            asprintf(err_msg, "rezero_pfns = malloc(%"PRIdSIZE") failed",
                     MAX_BATCH_SIZE * sizeof(*rezero_pfns));
            ret = -ENOMEM;
            goto out;
        }
    }

    if (vm_save_info.compress_mode == VM_SAVE_COMPRESS_LZ4) {
        if (!vm_save_info.single_page) {
            /* The LZ4_compressBound macro is unsafe, so we have to wrap the
             * argument. */
            compress_buf =
                (char *)malloc(LZ4_compressBound(
                                   (MAX_BATCH_SIZE << PAGE_SHIFT)));
            if (!compress_buf) {
                asprintf(err_msg, "malloc(compress_buf) failed");
                ret = -ENOMEM;
                goto out;
            }
            compress_mem = (char *)malloc(MAX_BATCH_SIZE << PAGE_SHIFT);
            if (!compress_mem) {
                asprintf(err_msg, "malloc(compress_mem) failed");
                ret = -ENOMEM;
                goto out;
            }
        } else {
            compress_buf = (char *)malloc(
                sizeof(compress_size) +
                MAX_BATCH_SIZE * (sizeof(cs16_t) + PAGE_SIZE));
            if (!compress_buf) {
                asprintf(err_msg, "malloc(compress_buf) failed");
                ret = -ENOMEM;
                goto out;
            }
        }
    }

    poi.max_gpfn = vm_mem_mb << (20 - UXEN_PAGE_SHIFT);
    poi.pfn_off = calloc(1, poi.max_gpfn * sizeof(poi.pfn_off[0]));
    /* adjust max_gpfn to account for pci hole after allocating pfn_off */
    if (poi.max_gpfn > PCI_HOLE_START_PFN)
        poi.max_gpfn += PCI_HOLE_END_PFN - PCI_HOLE_START_PFN;

    /* store start of batch file offset, to allow restoring page data
     * without parsing the entire save file */
    vm_save_info.page_batch_offset = filebuf_tell(f);

    pfn = 0;
    while (pfn < p2m_size && !vm_save_info.save_abort && !vm_quit_interrupt) {
        batch = 0;
        while ((pfn + batch) < p2m_size && batch < MAX_BATCH_SIZE) {
            gpfn_info_list[batch].gpfn = pfn + batch;
            gpfn_info_list[batch].flags = XENMEM_MCGI_FLAGS_VM |
                (free_mem ? XENMEM_MCGI_FLAGS_REMOVE_PFN : 0);
            batch++;
        }
        ret = xc_domain_memory_capture(
            xc_handle, vm_id, batch, gpfn_info_list, &batch_done,
            HYPERCALL_BUFFER(mem_buffer), MEM_BUFFER_SIZE);
        if (ret || batch_done != batch) {
            EPRINTF("xc_domain_memory_capture fail/incomple: ret %d"
                    " errno %d done %ld/%d", ret, errno, batch_done, batch);
        }
        rezero = 0;
        clone = 0;
        _batch = 0;
        _zero = 0;
        for (j = 0; j < batch_done; j++) {
            gpfn_info_list[j].type &= XENMEM_MCGI_TYPE_MASK;
            if (gpfn_info_list[j].type == XENMEM_MCGI_TYPE_NORMAL) {
                uint64_t *p = (uint64_t *)&mem_buffer[gpfn_info_list[j].offset];
                int i = 0;
                while (i < (PAGE_SIZE >> 3) && !p[i])
                    i++;
                if (i == (PAGE_SIZE >> 3)) {
                    gpfn_info_list[j].type = XENMEM_MCGI_TYPE_ZERO;
                    rezero++;
                    total_rezero++;
                    /* Always re-share zero pages. */
                    if (!free_mem)
                        rezero_pfns[rezero_nr++] = pfn + j;
                }
            }
            if (gpfn_info_list[j].type == XENMEM_MCGI_TYPE_NORMAL) {
                pfn_batch[_batch] = pfn + j;
                _batch++;
            }
            if (gpfn_info_list[j].type == XENMEM_MCGI_TYPE_ZERO) {
                __set_bit(pfn + j, zero_bitmap);
                _zero++;
                total_zero++;
            }
            if (gpfn_info_list[j].type == XENMEM_MCGI_TYPE_POD) {
                clone++;
                total_clone++;
            }
        }
        if (rezero_nr) {
            xc_domain_populate_physmap(xc_handle, vm_id, rezero_nr, 0,
                                       XENMEMF_populate_on_demand, rezero_pfns);
            rezero_nr = 0;
        }
        if (_batch) {
            if (vm_save_compress_mode_batched(vm_save_info.compress_mode)) {
                SAVE_DPRINTF("page batch %08x:%08x = %03x pages,"
                             " rezero %03x, clone %03x, zero %03x",
                             pfn, pfn + batch, _batch, rezero, clone, _zero);
                if (vm_save_info.compress_mode == VM_SAVE_COMPRESS_LZ4)
                    _batch += vm_save_info.single_page ?
                        2 * MAX_BATCH_SIZE : MAX_BATCH_SIZE;
                filebuf_write(f, &_batch, sizeof(_batch));
                if (vm_save_info.compress_mode == VM_SAVE_COMPRESS_LZ4)
                    _batch -= vm_save_info.single_page ?
                        2 * MAX_BATCH_SIZE : MAX_BATCH_SIZE;
                filebuf_write(f, pfn_batch, _batch * sizeof(pfn_batch[0]));
                if (vm_save_info.compress_mode == VM_SAVE_COMPRESS_LZ4 &&
                    vm_save_info.single_page) {
                    compress_size = 0;
                    mem_pos = filebuf_tell(f) + sizeof(compress_size);
                }
            }
            j = 0;
            m_run = 0;
            v_run = 0;
            while (j != batch) {
                while (j != batch &&
                       gpfn_info_list[j].type != XENMEM_MCGI_TYPE_NORMAL)
                    j++;
                run = j;
                while (j != batch &&
                       gpfn_info_list[j].type == XENMEM_MCGI_TYPE_NORMAL)
                    j++;
                if (run != j) {
                    b_run = j - run;
                    SAVE_DPRINTF(
                        "     write %08x:%08x = %03x pages",
                        pfn + run, pfn + j, b_run);
                    if (vm_save_info.compress_mode == VM_SAVE_COMPRESS_NONE) {
                        int i;
                        pos = filebuf_tell(f);
                        for (i = 0; i < b_run; i++) {
                            if (!poi_valid_pfn(&poi, pfn + run + i))
                                continue;
                            poi.pfn_off[poi_pfn_index(&poi, pfn + run + i)] =
                                pos + (i << PAGE_SHIFT);
                        }
                        filebuf_write(
                            f, &mem_buffer[gpfn_info_list[run].offset],
                            b_run << PAGE_SHIFT);
                    } else if (vm_save_info.compress_mode ==
                               VM_SAVE_COMPRESS_LZ4) {
                        if (vm_save_info.single_page) {
                            int i, cs1;
                            for (i = 0; i < b_run; i++) {
                                cs1 = uxenvm_compress_lz4(
                                    (const char *)&mem_buffer[
                                        gpfn_info_list[run + i].offset],
                                    &compress_buf[compress_size +
                                                  sizeof(cs16_t)],
                                    PAGE_SIZE);
                                if (cs1 >= PAGE_SIZE) {
                                    memcpy(&compress_buf[compress_size +
                                                         sizeof(cs16_t)],
                                           &mem_buffer[
                                               gpfn_info_list[run + i].offset],
                                           PAGE_SIZE);
                                    cs1 = PAGE_SIZE;
                                    v_run++;
                                } else
                                    m_run++;
                                /* if the page is not compressed, then
                                 * record the offset of the page data,
                                 * otherwise record the offset of the
                                 * size field and set the
                                 * PAGE_OFFSET_INDEX_PFN_OFF_COMPRESSED
                                 * indicator */
                                if (poi_valid_pfn(&poi, pfn + run + i))
                                    poi.pfn_off[
                                        poi_pfn_index(&poi,
                                                      pfn + run + i)] =
                                        (mem_pos + compress_size) +
                                        (cs1 == PAGE_SIZE ? sizeof(cs16_t) :
                                         PAGE_OFFSET_INDEX_PFN_OFF_COMPRESSED);
                                *(cs16_t *)&compress_buf[compress_size] = cs1;
                                compress_size += sizeof(cs16_t) + cs1;
                            }
                        } else {
                            memcpy(&compress_mem[m_run << PAGE_SHIFT],
                                   &mem_buffer[gpfn_info_list[run].offset],
                                   b_run << PAGE_SHIFT);
                            m_run += b_run;
                        }
                    }
                    run += b_run;
                    _batch -= b_run;
                    total_pages += b_run;
                }
            }

            if (_batch)
                debug_printf("%d stray pages\n", _batch);
            if (vm_save_info.compress_mode == VM_SAVE_COMPRESS_LZ4) {
                if (!vm_save_info.single_page) {
                    compress_size = uxenvm_compress_lz4(
                        compress_mem, compress_buf, m_run << PAGE_SHIFT);
                    if (compress_size >= m_run << PAGE_SHIFT) {
                        SAVE_DPRINTF("compressed size larger for pages "
                                     "%08x:%08x by %d", pfn, pfn + m_run,
                                     compress_size - (m_run << PAGE_SHIFT));
                        compress_size = -1;
                    }
                    filebuf_write(f, &compress_size, sizeof(compress_size));
                    if (compress_size != -1) {
                        filebuf_write(f, compress_buf, compress_size);
                        total_compressed_pages += m_run;
                        total_compress_save +=
                            (m_run << PAGE_SHIFT) - compress_size;
                    } else {
                        filebuf_write(f, compress_mem,
                                      m_run << PAGE_SHIFT);
                        total_compress_in_vain += m_run;
                    }
                } else {
                    filebuf_write(f, &compress_size, sizeof(compress_size));
                    filebuf_write(f, compress_buf, compress_size);
                    total_compressed_pages += m_run;
                    total_compress_save +=
                        ((m_run + v_run) << PAGE_SHIFT) - compress_size;
                    total_compress_in_vain += v_run;
                }
            }
	}
	pfn += batch;
    }

    if (!vm_save_info.save_abort && !vm_quit_interrupt) {
        s_zero_bitmap.marker = XC_SAVE_ID_ZERO_BITMAP;
        s_zero_bitmap.zero_bitmap_size = zero_bitmap_size;
        zero_bitmap_compressed = malloc(LZ4_compressBound((zero_bitmap_size)));
        if (!zero_bitmap_compressed)
            s_zero_bitmap.size = zero_bitmap_size;
        else {
            s_zero_bitmap.size = uxenvm_compress_lz4(
                (const char *)zero_bitmap, (char *)zero_bitmap_compressed,
                zero_bitmap_size);
            if (s_zero_bitmap.size >= zero_bitmap_size) {
                free(zero_bitmap_compressed);
                zero_bitmap_compressed = NULL;
                s_zero_bitmap.size = zero_bitmap_size;
            }
        }
        s_zero_bitmap.size += sizeof(s_zero_bitmap);
        APRINTF("zero bitmap: size %d bitmap_size %d",
                s_zero_bitmap.size, s_zero_bitmap.zero_bitmap_size);
        filebuf_write(f, &s_zero_bitmap, sizeof(s_zero_bitmap));
        filebuf_write(f, zero_bitmap_compressed ? : zero_bitmap,
                      s_zero_bitmap.size - sizeof(s_zero_bitmap));

        s_vm_page_offsets.marker = XC_SAVE_ID_PAGE_OFFSETS;
        s_vm_page_offsets.pfn_off_nr = poi_pfn_index(&poi, poi.max_gpfn);
        page_offsets_index.offset = filebuf_tell(f);
        APRINTF("page offset index: pos %"PRId64" size %"PRIdSIZE" nr off %d",
                page_offsets_index.offset, s_vm_page_offsets.pfn_off_nr *
                sizeof(s_vm_page_offsets.pfn_off[0]),
                s_vm_page_offsets.pfn_off_nr);
        BUILD_BUG_ON(sizeof(poi.pfn_off[0]) !=
                     sizeof(s_vm_page_offsets.pfn_off[0]));
        s_vm_page_offsets.size = sizeof(s_vm_page_offsets) +
            s_vm_page_offsets.pfn_off_nr * sizeof(s_vm_page_offsets.pfn_off[0]);
        filebuf_write(f, &s_vm_page_offsets, sizeof(s_vm_page_offsets));
        filebuf_write(f, poi.pfn_off, s_vm_page_offsets.pfn_off_nr *
                      sizeof(s_vm_page_offsets.pfn_off[0]));
    }

    if (!vm_save_info.save_abort && !vm_quit_interrupt) {
        /* 0: end marker */
        batch = 0;
        filebuf_write(f, &batch, sizeof(batch));

        /* indexes */
        filebuf_write(f, &page_offsets_index, sizeof(page_offsets_index));

        APRINTF("memory: pages %d zero %d rezero %d clone %d", total_pages,
                total_zero - total_rezero, total_rezero, total_clone);
        if (vm_save_info.compress_mode == VM_SAVE_COMPRESS_LZ4 && total_pages) {
            int pct;
            pct = 10000 * (total_compress_save >> PAGE_SHIFT) / total_pages;
            APRINTF("        compressed %d in-vain %d -- saved %"PRIdSIZE
                    " bytes (%d.%02d%%)",
                    total_compressed_pages, total_compress_in_vain,
                    total_compress_save, pct / 100, pct % 100);
        }
    } else
        APRINTF("%s: save aborted%s", __FUNCTION__,
                vm_quit_interrupt ? " (quit interrupt)" : "");

    ret = 0;
  out:
    if (mem_buffer)
        xc_hypercall_buffer_free_pages(xc_handle, mem_buffer,
                                       MEM_BUFFER_SIZE >> PAGE_SHIFT);
    free(zero_bitmap);
    free(zero_bitmap_compressed);
    free(rezero_pfns);
    free(pfn_batch);
    free(gpfn_info_list);
    free(compress_mem);
    free(compress_buf);
    free(hvm_buf);
    return ret;
}

#define uxenvm_load_read(f, buf, size, ret, err_msg, _out) do {         \
        (ret) = filebuf_read((f), (buf), (size));                       \
        if ((ret) != (size)) {                                          \
            asprintf((err_msg), "uxenvm_load_read(%s) failed", #buf);   \
            if ((ret) >= 0)                                             \
                (ret) = -EIO;                                           \
            else                                                        \
                (ret) = -errno;                                         \
            goto _out;                                                  \
        }                                                               \
    } while(0)

static int
uxenvm_load_zero_bitmap(uint8_t *zero_bitmap, uint32_t zero_bitmap_size,
                        xen_pfn_t *pfn_type, char **err_msg)
{
    int i, j;
    int ret = 0;

    for (i = j = 0; i < 8 * zero_bitmap_size; ++i) {
        if (test_bit(i, zero_bitmap))
            pfn_type[j++] = i;
        if (j == MAX_BATCH_SIZE || i == 8 * zero_bitmap_size - 1) {
            ret = xc_domain_populate_physmap_exact(
                xc_handle, vm_id, j, 0, XENMEMF_populate_on_demand,
                pfn_type);
            if (ret) {
                asprintf(err_msg,
                         "xc_domain_populate_physmap_exact failed");
                goto out;
            }
            j = 0;
        }
    }
  out:
    return ret;
}

static int
decompress_batch(int batch, xen_pfn_t *pfn_type, uint8_t *mem,
                 char *compress_buf, uint32_t compress_size,
                 int single_page, char **err_msg)
{
    int ret;

    if (single_page) {
        int i;
        uint32_t decompress_pos = 0;
        for (i = 0; i < batch; i++) {
            cs16_t cs1;
            cs1 = *(cs16_t *)&compress_buf[decompress_pos];
            if (cs1 > PAGE_SIZE) {
                asprintf(err_msg, "invalid size %d for page %"PRIx64
                         "\n", cs1, pfn_type ? pfn_type[i] : 0);
                ret = -1;
                goto out;
            }
            decompress_pos += sizeof(cs16_t);
            if (cs1 < PAGE_SIZE) {
                ret = LZ4_decompress_fast(&compress_buf[decompress_pos],
                                          (char *)&mem[i << PAGE_SHIFT],
                                          PAGE_SIZE);
                if (ret != cs1) {
                    asprintf(err_msg, "decompression of page %"PRIx64
                             " failed at byte %d of %d\n",
                             pfn_type ? pfn_type[i] : 0,
                             -ret, cs1);
                    ret = -1;
                    goto out;
                }
            } else
                memcpy(&mem[i << PAGE_SHIFT],
                       &compress_buf[decompress_pos], PAGE_SIZE);
            decompress_pos += cs1;
        }
    } else {
        ret = LZ4_decompress_fast(compress_buf, (char *)mem,
                                  batch << PAGE_SHIFT);
        if (ret != compress_size) {
            asprintf(err_msg, "decompression of page %"PRIx64
                     ":%"PRIx64" failed at byte %d of %d\n",
                     pfn_type ? pfn_type[0] : 0,
                     pfn_type ? pfn_type[batch - 1] + 1 : 0,
                     -ret, compress_size);
            ret = -1;
            goto out;
        }
    }

    ret = 0;
  out:
    return ret;
}

#ifndef DECOMPRESS_THREADED
struct decompress_ctx {
    xc_hypercall_buffer_t pp_buffer;
};
#else  /* DECOMPRESS_THREADED */
struct decompress_ctx;

struct decompress_buf_ctx {
    int batch;
    void *compress_buf;
    int compress_size;
    int single_page;
    int populate_compressed;
    xc_hypercall_buffer_t pp_buffer;
    xen_pfn_t *pfn_type;
    struct decompress_ctx *dc;
    LIST_ENTRY(decompress_buf_ctx) elem;
};

struct decompress_ctx {
    struct async_op_ctx *async_op_ctx;
    LIST_HEAD(, decompress_buf_ctx) list;
    ioh_event process_event;
    int ret;
    xc_interface *xc_handle;
    int vm_id;
    char **err_msg;
};

static void
decompress_cb(void *opaque)
{
    struct decompress_buf_ctx *dbc = (struct decompress_buf_ctx *)opaque;
    int ret;

    if (!dbc->populate_compressed) {
        ret = decompress_batch(
            dbc->batch, dbc->pfn_type,
            HYPERCALL_BUFFER_ARGUMENT_BUFFER(&dbc->pp_buffer),
            dbc->compress_buf, dbc->compress_size, dbc->single_page,
            dbc->dc->err_msg);
        if (ret)
            goto out;
    } else
        memcpy(HYPERCALL_BUFFER_ARGUMENT_BUFFER(&dbc->pp_buffer),
               dbc->compress_buf, dbc->compress_size);

    ret = xc_domain_populate_physmap_from_buffer(
        dbc->dc->xc_handle, dbc->dc->vm_id, dbc->batch, 0,
        dbc->populate_compressed ? XENMEMF_populate_from_buffer_compressed :
        XENMEMF_populate_from_buffer, &dbc->pfn_type[0], &dbc->pp_buffer);
    if (ret)
        asprintf(dbc->dc->err_msg,
                 "xc_domain_populate_physmap_from_buffer failed");

  out:
    if (ret)
        dbc->dc->ret = ret;
}

static void
decompress_complete(void *opaque)
{
    struct decompress_buf_ctx *dbc = (struct decompress_buf_ctx *)opaque;

    free(dbc->compress_buf);
    dbc->compress_buf = NULL;
    LIST_INSERT_HEAD(&dbc->dc->list, dbc, elem);
}

static int
decompress_wait_all(struct decompress_ctx *dc, char **err_msg)
{
    struct decompress_buf_ctx *dbc;
    int i;
    int ret = 0;

    APRINTF("waiting for decompress threads");
    assert(dc->async_op_ctx);
    assert(dc->xc_handle);
    for (i = 0; i < DECOMPRESS_THREADS; i++) {
        ioh_event_reset(&dc->process_event);
        async_op_process(dc->async_op_ctx);
        dbc = LIST_FIRST(&dc->list);
        if (!dbc) {
            ioh_event_wait(&dc->process_event);
            async_op_process(dc->async_op_ctx);
            dbc = LIST_FIRST(&dc->list);
        }
        if (!dbc) {
            if (err_msg)
                asprintf(err_msg, "failed to wait for dbc");
            ret = -1;
            continue;
        }
        LIST_REMOVE(dbc, elem);
        xc__hypercall_buffer_free_pages(dc->xc_handle, &dbc->pp_buffer,
                                        PP_BUFFER_PAGES);
        free(dbc);
    }

    ioh_event_close(&dc->process_event);

    async_op_free(dc->async_op_ctx);
    dc->async_op_ctx = NULL;

    return ret;
}
#endif  /* DECOMPRESS_THREADED */

static int
uxenvm_load_readbatch(struct filebuf *f, int batch, xen_pfn_t *pfn_type,
                      int *pfn_info, int *pfn_err, int decompress,
                      struct decompress_ctx *dc, int single_page,
                      int do_lazy_load, int populate_compressed, char **err_msg)
{
    uint8_t *mem = NULL;
    int j;
    int ret;
    char *compress_buf = NULL;
    int compress_size = 0;

    LOAD_DPRINTF("page batch %03x pages", batch);

    if (!single_page)
        populate_compressed = 0;

    uxenvm_load_read(f, &pfn_info[0], batch * sizeof(pfn_info[0]),
                     ret, err_msg, out);

    /* XXX legacy -- new save files have a clean pfn_info array */
    for (j = 0; j < batch; j++) {
	pfn_type[j] = pfn_info[j] & ~XEN_DOMCTL_PFINFO_LTAB_MASK;
        if (!do_lazy_load)
            continue;
        if (pfn_type[j] >= PCI_HOLE_START_PFN && pfn_type[j] < PCI_HOLE_END_PFN)
            do_lazy_load = 0;
    }

    if (decompress) {
        uxenvm_load_read(f, &compress_size, sizeof(compress_size),
                         ret, err_msg, out);
        if (compress_size == -1)
            decompress = 0;
    }

    if (!decompress || do_lazy_load) {
        LOAD_DPRINTF("  populate %08"PRIx64":%08"PRIx64" = %03x pages",
                     pfn_type[0], pfn_type[batch - 1] + 1, batch);
        ret = xc_domain_populate_physmap_exact(
            xc_handle, vm_id, batch, 0, XENMEMF_populate_on_demand |
            (do_lazy_load ? XENMEMF_populate_on_demand_dmreq : 0),
            &pfn_type[0]);
        if (ret) {
            asprintf(err_msg, "xc_domain_populate_physmap_exact failed");
            goto out;
        }

        if (do_lazy_load) {
            uint32_t skip;
            if (decompress)
                skip = compress_size;
            else
                skip = batch << PAGE_SHIFT;
            ret = filebuf_seek(f, skip, FILEBUF_SEEK_CUR) != -1 ? 0 : -EIO;
            if (ret < 0)
                asprintf(err_msg, "page %"PRIx64":%"PRIx64" skip failed",
                         pfn_type[0], pfn_type[batch - 1] + 1);
            goto out;
        }

        mem = xc_map_foreign_bulk(xc_handle, vm_id, PROT_WRITE,
                                  &pfn_type[0], &pfn_err[0], batch);
        if (mem == NULL) {
            asprintf(err_msg, "xc_map_foreign_bulk failed");
            ret = -1;
            goto out;
        }
        for (j = 0; j < batch; j++) {
            if (pfn_err[j]) {
                asprintf(err_msg, "map fail: %d/%d gpfn %08"PRIx64" err %d",
                         j, batch, pfn_type[j], pfn_err[j]);
                ret = -1;
                goto out;
            }
        }

        LOAD_DPRINTF("      read %08"PRIx64":%08"PRIx64" = %03x pages",
                     pfn_type[0], pfn_type[batch - 1] + 1, batch);
        uxenvm_load_read(f, mem, batch << PAGE_SHIFT, ret, err_msg, out);
    } else {
#ifdef DECOMPRESS_THREADED
        struct decompress_buf_ctx *dbc;
#endif  /* DECOMPRESS_THREADED */

        compress_buf = malloc(compress_size);
        if (!compress_buf) {
            asprintf(err_msg, "malloc(compress_size) failed");
            goto out;
        }

        LOAD_DPRINTF("      read %08"PRIx64":%08"PRIx64" = %03x pages",
                     pfn_type[0], pfn_type[batch - 1] + 1, batch);
        uxenvm_load_read(f, compress_buf, compress_size, ret, err_msg, out);
#ifdef DECOMPRESS_THREADED
        ioh_event_reset(&dc->process_event);
        async_op_process(dc->async_op_ctx);
        if (dc->ret) {
            ret = -1;
            goto out;
        }
        dbc = LIST_FIRST(&dc->list);
        if (!dbc) {
            ioh_event_wait(&dc->process_event);
            async_op_process(dc->async_op_ctx);
            dbc = LIST_FIRST(&dc->list);
        }
        if (!dbc) {
            asprintf(err_msg, "no decompress_buf_ctx");
            ret = -1;
            goto out;
        }
        LIST_REMOVE(dbc, elem);
        memcpy(&dbc->pfn_type[0], &pfn_type[0], batch * sizeof(*pfn_type));
        dbc->compress_buf = compress_buf;
        compress_buf = NULL;
        dbc->compress_size = compress_size;
        dbc->batch = batch;
        dbc->single_page = single_page;
        dbc->populate_compressed = populate_compressed;
        ret = async_op_add(dc->async_op_ctx, dbc, &dc->process_event,
                           decompress_cb, decompress_complete);
        if (ret) {
            asprintf(err_msg, "async_op_add failed");
            goto out;
        }
#else  /* DECOMPRESS_THREADED */
        if (!populate_compressed) {
            ret = decompress_batch(
                batch, pfn_type,
                HYPERCALL_BUFFER_ARGUMENT_BUFFER(&dc->pp_buffer),
                compress_buf, compress_size, single_page, err_msg);
            if (ret)
                goto out;
        } else
            memcpy(HYPERCALL_BUFFER_ARGUMENT_BUFFER(&dc->pp_buffer),
                   compress_buf, compress_size);

        LOAD_DPRINTF("  populate %08"PRIx64":%08"PRIx64" = %03x pages",
                     pfn_type[0], pfn_type[batch - 1] + 1, batch);
        ret = xc_domain_populate_physmap_from_buffer(
            xc_handle, domid, batch, 0, populate_compressed ?
            XENMEMF_populate_from_buffer_compressed :
            XENMEMF_populate_from_buffer, &pfn_type[0], &dc->pp_buffer);
        if (ret) {
            asprintf(err_msg, "xc_domain_populate_physmap_from_buffer "
                     "compressed failed");
            goto out;
        }
#endif  /* DECOMPRESS_THREADED */
    }

    ret = 0;
  out:
    if (mem)
        xc_munmap(xc_handle, vm_id, mem, batch * PAGE_SIZE);
    free(compress_buf);
    return ret;
}

static uint32_t uxenvm_load_progress = 0;

static int
uxenvm_load_alloc(xen_pfn_t **pfn_type, int **pfn_err, int **pfn_info,
                  char **err_msg)
{
    int ret = 0;

    *pfn_type = malloc(MAX_BATCH_SIZE * sizeof(**pfn_type));
    if (*pfn_type == NULL) {
	asprintf(err_msg, "pfn_type = malloc(%"PRIdSIZE") failed",
		 MAX_BATCH_SIZE * sizeof(**pfn_type));
	ret = -ENOMEM;
	goto out;
    }

    *pfn_info = malloc(MAX_BATCH_SIZE * sizeof(**pfn_info));
    if (*pfn_info == NULL) {
	asprintf(err_msg, "pfn_info = malloc(%"PRIdSIZE") failed",
		 MAX_BATCH_SIZE * sizeof(**pfn_info));
	ret = -ENOMEM;
	goto out;
    }

    *pfn_err = malloc(MAX_BATCH_SIZE * sizeof(**pfn_err));
    if (*pfn_err == NULL) {
	asprintf(err_msg, "pfn_err = malloc(%"PRIdSIZE") failed",
		 MAX_BATCH_SIZE * sizeof(**pfn_err));
	ret = -ENOMEM;
	goto out;
    }

    uxenvm_load_progress = 0;

  out:
    return ret;
}

static int
uxenvm_load_batch(struct filebuf *f, int32_t marker, xen_pfn_t *pfn_type,
                  int *pfn_err, int *pfn_info, struct decompress_ctx *dc,
                  int do_lazy_load, int populate_compressed, char **err_msg)
{
    DECLARE_HYPERCALL_BUFFER(uint8_t, pp_buffer);
    int decompress;
    int single_page;
    int ret;

    decompress = 0;
    single_page = 0;
    if ((unsigned int)marker > 3 * MAX_BATCH_SIZE) {
        asprintf(err_msg, "invalid batch size: %x",
                 (unsigned int)marker);
        ret = -EINVAL;
        goto out;
    } else if (marker > 2 * MAX_BATCH_SIZE) {
        marker -= 2 * MAX_BATCH_SIZE;
        decompress = 1;
        single_page = 1;
    } else if (marker > MAX_BATCH_SIZE) {
        marker -= MAX_BATCH_SIZE;
        decompress = 1;
    }
    if (decompress) {
#ifdef DECOMPRESS_THREADED
        if (!dc->async_op_ctx) {
            struct decompress_buf_ctx *dbc;
            int i;
            dc->ret = 0;
            dc->async_op_ctx = async_op_init();
            LIST_INIT(&dc->list);
            for (i = 0; i < DECOMPRESS_THREADS; i++) {
                dbc = calloc(1, sizeof(struct decompress_buf_ctx));
                if (!dbc) {
                    asprintf(err_msg, "calloc dbc failed");
                    ret = -ENOMEM;
                    goto out;
                }
                pp_buffer = xc_hypercall_buffer_alloc_pages(
                    xc_handle, pp_buffer, PP_BUFFER_PAGES);
                if (!pp_buffer) {
                    asprintf(err_msg, "xc_hypercall_buffer_alloc_pages"
                             "(%d pages) failed", PP_BUFFER_PAGES);
                    ret = -ENOMEM;
                    goto out;
                }
                dbc->pp_buffer = *HYPERCALL_BUFFER(pp_buffer);
                dbc->pfn_type = malloc(
                    MAX_BATCH_SIZE * sizeof(*pfn_type));
                if (dbc->pfn_type == NULL) {
                    asprintf(err_msg, "dbc->pfn_type = malloc(%"
                             PRIdSIZE") failed",
                             MAX_BATCH_SIZE * sizeof(*pfn_type));
                    ret = -ENOMEM;
                    goto out;
                }
                dbc->dc = dc;
                LIST_INSERT_HEAD(&dc->list, dbc, elem);
            }
            ioh_event_init(&dc->process_event);
            dc->xc_handle = xc_handle;
            dc->vm_id = vm_id;
            dc->err_msg = err_msg;
            pp_buffer = NULL;
        }
#else
        if (!dc->pp_buffer) {
            pp_buffer = xc_hypercall_buffer_alloc_pages(
                xc_handle, pp_buffer, PP_BUFFER_PAGES);
            if (!pp_buffer) {
                asprintf(err_msg, "xc_hypercall_buffer_alloc_pages"
                         "(%d pages) failed", PP_BUFFER_PAGES);
                ret = -ENOMEM;
                goto out;
            }
            dc->pp_buffer = *HYPERCALL_BUFFER(pp_buffer);
        }
#endif  /* DECOMPRESS_THREADED */
    }
    uxenvm_load_progress += marker;
    /* output progress load message every ~10% */
    if ((uxenvm_load_progress * 10 / (vm_mem_mb << 8UL)) !=
        ((uxenvm_load_progress - marker) * 10 / (vm_mem_mb << 8UL)))
        APRINTF("memory load %d pages", uxenvm_load_progress);
    ret = uxenvm_load_readbatch(f, marker, pfn_type, pfn_info, pfn_err,
                                decompress, dc, single_page, do_lazy_load,
                                populate_compressed, err_msg);
  out:
    return ret;
}

static int
apply_immutable_memory(struct immutable_range *r, int nranges)
{
    int i;

    for (i = 0; i < nranges; i++)
        if (xc_hvm_set_mem_type(xc_handle, vm_id, HVMMEM_ram_immutable,
                                r[i].base, r[i].size))
            EPRINTF("xc_hvm_set_mem_type(HVMMEM_ram_immutable) failed: "
                    "pfn 0x%"PRIx64" size 0x%"PRIx64, r[i].base, r[i].size);
    APRINTF("%s: done", __FUNCTION__);

    return 0;
}

#define uxenvm_load_read_struct(f, s, _marker, ret, err_msg, _out) do {	\
        (ret) = uxenvm_read_struct((f), &(s));                          \
        if ((ret) != uxenvm_read_struct_size(&(s))) {                   \
            asprintf((err_msg), "uxenvm_read_struct(%s) failed", #s);   \
	    goto _out;							\
	}								\
	(s).marker = (_marker);						\
    } while(0)

static uint8_t *dm_state_load_buf = NULL;
static int dm_state_load_size = 0;

#define uxenvm_check_restore_clone(mode) do {                           \
        if ((mode) == VM_RESTORE_CLONE) {                               \
            ret = xc_domain_clone_physmap(xc_handle, vm_id,             \
                                          vm_template_uuid);            \
            if (ret < 0) {                                              \
                asprintf(err_msg, "xc_domain_clone_physmap failed");    \
                goto out;                                               \
            }                                                           \
            if (!vm_has_template_uuid) {                                \
                vm_has_template_uuid = 1;                               \
                ret = 0;                                                \
                goto skip_mem;                                          \
            }                                                           \
            (mode) = VM_RESTORE_NORMAL;                                 \
        }                                                               \
    } while (0)

static int
uxenvm_loadvm_execute(struct filebuf *f, int restore_mode, char **err_msg)
{
    struct xc_save_version_info s_version_info = { };
    struct xc_save_tsc_info s_tsc_info = { };
    struct xc_save_vcpu_info s_vcpu_info = { };
    struct xc_save_hvm_generic_chunk s_hvm_ident_pt = { };
    struct xc_save_hvm_generic_chunk s_hvm_vm86_tss = { };
    struct xc_save_hvm_generic_chunk s_hvm_console_pfn = { };
    struct xc_save_hvm_generic_chunk s_hvm_acpi_ioports_location = { };
    struct xc_save_hvm_magic_pfns s_hvm_magic_pfns = { };
    struct xc_save_hvm_context s_hvm_context = { };
    struct xc_save_hvm_dm s_hvm_dm = { };
    struct xc_save_vm_uuid s_vm_uuid = { };
    struct xc_save_vm_template_uuid s_vm_template_uuid = { };
    struct xc_save_hvm_introspec s_hvm_introspec = { };
    struct xc_save_mapcache_params s_mapcache_params = { };
    struct xc_save_vm_template_file s_vm_template_file = { };
    struct xc_save_vm_page_offsets s_vm_page_offsets = { };
    struct xc_save_zero_bitmap s_zero_bitmap = { };
    struct immutable_range *immutable_ranges = NULL;
    uint8_t *hvm_buf = NULL;
    uint8_t *zero_bitmap = NULL, *zero_bitmap_compressed = NULL;
    xen_pfn_t *pfn_type = NULL;
    int *pfn_err = NULL, *pfn_info = NULL;
    struct decompress_ctx dc = { 0 };
    int populate_compressed = (restore_mode == VM_RESTORE_TEMPLATE);
    int do_lazy_load = 0;
    int load_lazy_load_info = do_lazy_load;
    struct page_offset_info *lli = &dm_lazy_load_info;
    int32_t marker;
    int ret;
    int size;

    /* XXX init debug option */
    if (strstr(uxen_opt_debug, ",uncomptmpl,"))
        populate_compressed = 0;

    ret = uxenvm_load_alloc(&pfn_type, &pfn_err, &pfn_info, err_msg);
    if (ret < 0)
        goto out;

    uxenvm_load_read(f, &marker, sizeof(marker), ret, err_msg, out);
    if (marker == XC_SAVE_ID_VERSION)
        uxenvm_load_read_struct(f, s_version_info, marker, ret, err_msg, out);
    if (s_version_info.version != SAVE_FORMAT_VERSION) {
        asprintf(err_msg, "version info mismatch: %d != %d",
                 s_version_info.version, SAVE_FORMAT_VERSION);
        ret = -EINVAL;
        goto out;
    }
    while (!vm_quit_interrupt) {
        uxenvm_load_read(f, &marker, sizeof(marker), ret, err_msg, out);
	if (marker == 0)	/* end marker */
	    break;
	switch (marker) {
	case XC_SAVE_ID_TSC_INFO:
	    uxenvm_load_read_struct(f, s_tsc_info, marker, ret, err_msg, out);
	    APRINTF("tsc info: mode %d nsec %"PRIu64" khz %d incarn %d",
		    s_tsc_info.tsc_mode, s_tsc_info.nsec, s_tsc_info.khz,
		    s_tsc_info.incarn);
	    break;
	case XC_SAVE_ID_VCPU_INFO:
	    uxenvm_load_read_struct(f, s_vcpu_info, marker, ret, err_msg, out);
	    APRINTF("vcpus %d online %"PRIx64, s_vcpu_info.max_vcpu_id,
		    s_vcpu_info.vcpumap);
	    break;
	case XC_SAVE_ID_HVM_IDENT_PT:
	    uxenvm_load_read_struct(f, s_hvm_ident_pt, marker, ret, err_msg,
				    out);
	    APRINTF("ident_pt %"PRIx64, s_hvm_ident_pt.data);
	    break;
	case XC_SAVE_ID_HVM_VM86_TSS:
	    uxenvm_load_read_struct(f, s_hvm_vm86_tss, marker, ret, err_msg,
				    out);
	    APRINTF("vm86_tss %"PRIx64, s_hvm_vm86_tss.data);
	    break;
	case XC_SAVE_ID_HVM_CONSOLE_PFN:
	    uxenvm_load_read_struct(f, s_hvm_console_pfn, marker, ret, err_msg,
				    out);
	    APRINTF("console_pfn %"PRIx64, s_hvm_console_pfn.data);
	    break;
	case XC_SAVE_ID_HVM_ACPI_IOPORTS_LOCATION:
	    uxenvm_load_read_struct(f, s_hvm_acpi_ioports_location, marker,
				    ret, err_msg, out);
	    APRINTF("acpi_ioports_location %"PRIx64,
		    s_hvm_acpi_ioports_location.data);
	    break;
	case XC_SAVE_ID_HVM_MAGIC_PFNS:
	    uxenvm_load_read_struct(f, s_hvm_magic_pfns, marker, ret, err_msg,
				    out);
            APRINTF("ioreq pfn %"PRIx64"-%"PRIx64" shared info pfn %"PRIx64
                    " dmreq pfn %"PRIx64"/%"PRIx64,
                    s_hvm_magic_pfns.magic_pfns[0],
                    s_hvm_magic_pfns.magic_pfns[1],
                    s_hvm_magic_pfns.magic_pfns[2],
                    s_hvm_magic_pfns.magic_pfns[3],
                    s_hvm_magic_pfns.magic_pfns[4]);
	    break;
	case XC_SAVE_ID_HVM_CONTEXT:
	    uxenvm_load_read_struct(f, s_hvm_context, marker, ret, err_msg,
				    out);
	    APRINTF("hvm rec size %d", s_hvm_context.size);
	    hvm_buf = malloc(s_hvm_context.size);
	    if (hvm_buf == NULL) {
		asprintf(err_msg, "hvm_buf = malloc(%d) failed",
			 s_hvm_context.size);
		ret = -ENOMEM;
		goto out;
	    }
            uxenvm_load_read(f, hvm_buf, s_hvm_context.size, ret, err_msg, out);
	    break;
	case XC_SAVE_ID_HVM_DM:
	    uxenvm_load_read_struct(f, s_hvm_dm, marker, ret, err_msg, out);
	    APRINTF("dm rec size %d", s_hvm_dm.size);
	    dm_state_load_buf = malloc(s_hvm_dm.size);
	    if (dm_state_load_buf == NULL) {
		asprintf(err_msg, "dm_state_load_buf = malloc(%d) failed",
			 s_hvm_dm.size);
		ret = -ENOMEM;
		goto out;
	    }
            uxenvm_load_read(f, dm_state_load_buf, s_hvm_dm.size,
                             ret, err_msg, out);
	    dm_state_load_size = s_hvm_dm.size;
	    break;
	case XC_SAVE_ID_VM_UUID:
	    uxenvm_load_read_struct(f, s_vm_uuid, marker, ret, err_msg, out);
            if (restore_mode == VM_RESTORE_TEMPLATE)
                memcpy(vm_uuid, s_vm_uuid.uuid, sizeof(vm_uuid));
            if (!vm_has_template_uuid)
                memcpy(vm_template_uuid, s_vm_uuid.uuid,
                       sizeof(vm_template_uuid));
	    break;
	case XC_SAVE_ID_VM_TEMPLATE_UUID:
	    uxenvm_load_read_struct(f, s_vm_template_uuid, marker, ret,
				    err_msg, out);
	    memcpy(vm_template_uuid, s_vm_template_uuid.uuid,
                   sizeof(vm_template_uuid));
	    vm_has_template_uuid = 1;
	    break;
        case XC_SAVE_ID_HVM_INTROSPEC:
            uxenvm_load_read_struct(f, s_hvm_introspec, marker, ret, err_msg,
                                    out);
            dmpdev_PsLoadedModulesList =
                s_hvm_introspec.info.PsLoadedModulesList;
            dmpdev_PsActiveProcessHead =
                s_hvm_introspec.info.PsActiveProcessHead;
            size = s_hvm_introspec.info.n_immutable_ranges *
                sizeof(struct immutable_range);
            immutable_ranges = malloc(size);
            if (!immutable_ranges) {
                asprintf(err_msg,
                         "introspec_state_load_buf = malloc(%d) failed", size);
                ret = -ENOMEM;
                goto out;
            }
            uxenvm_load_read(f, immutable_ranges, size, ret, err_msg, out);
            APRINTF("immutable_ranges size 0x%x", size);
            break;
        case XC_SAVE_ID_MAPCACHE_PARAMS:
            uxenvm_load_read_struct(f, s_mapcache_params, marker, ret, err_msg,
                                    out);
            break;
        case XC_SAVE_ID_VM_TEMPLATE_FILE:
            uxenvm_load_read_struct(f, s_vm_template_file, marker, ret,
                                    err_msg, out);
            vm_template_file = calloc(1, s_vm_template_file.size + 1);
            if (vm_template_file == NULL) {
                asprintf(err_msg, "vm_template_file = calloc(%d) failed",
                         s_vm_template_file.size + 1);
                ret = -ENOMEM;
                goto out;
            }
            uxenvm_load_read(f, vm_template_file, s_vm_template_file.size,
                             ret, err_msg, out);
            vm_template_file[s_vm_template_file.size] = 0;
            APRINTF("vm template file: %s", vm_template_file);
            do_lazy_load = 0;
            break;
        case XC_SAVE_ID_PAGE_OFFSETS:
            uxenvm_load_read_struct(f, s_vm_page_offsets, marker, ret,
                                    err_msg, out);
            ret = filebuf_seek(f, s_vm_page_offsets.pfn_off_nr *
                               sizeof(s_vm_page_offsets.pfn_off[0]),
                               FILEBUF_SEEK_CUR) != -1 ? 0 : -EIO;
            if (ret < 0) {
                asprintf(err_msg, "filebuf_seek(vm_page_offsets) failed");
                goto out;
            }
            APRINTF("page offset index: %d pages, skipped %"PRIdSIZE
                    " bytes at %"PRId64,
                    s_vm_page_offsets.pfn_off_nr,
                    s_vm_page_offsets.pfn_off_nr *
                    sizeof(s_vm_page_offsets.pfn_off[0]),
                    filebuf_tell(f) - s_vm_page_offsets.size);
            break;
        case XC_SAVE_ID_ZERO_BITMAP:
            uxenvm_load_read_struct(f, s_zero_bitmap, marker, ret, err_msg,
                                    out);
            zero_bitmap_compressed =
                malloc(s_zero_bitmap.size - sizeof(s_zero_bitmap));
            if (zero_bitmap_compressed == NULL) {
                asprintf(err_msg, "zero_bitmap_compressed = "
                         "malloc(%"PRIdSIZE") failed",
                         s_zero_bitmap.size - sizeof(s_zero_bitmap));
                ret = -ENOMEM;
                goto out;
            }
            zero_bitmap = malloc(s_zero_bitmap.zero_bitmap_size);
            if (zero_bitmap == NULL) {
                asprintf(err_msg, "zero_bitmap = malloc(%d) failed",
                         s_zero_bitmap.zero_bitmap_size);
                ret = -ENOMEM;
                goto out;
            }
            uxenvm_load_read(f, zero_bitmap_compressed, s_zero_bitmap.size -
                             sizeof(s_zero_bitmap), ret, err_msg, out);
            ret = LZ4_decompress_fast((const char *)zero_bitmap_compressed,
                                      (char *)zero_bitmap,
                                      s_zero_bitmap.zero_bitmap_size);
            if (ret != s_zero_bitmap.size - sizeof(s_zero_bitmap)) {
                asprintf(err_msg, "LZ4_decompress_fast(zero_bitmap) failed:"
                         " %d != %"PRIdSIZE, ret,
                         s_zero_bitmap.size - sizeof(s_zero_bitmap));
                ret = -EINVAL;
                goto out;
            }
            uxenvm_check_restore_clone(restore_mode);
            ret = uxenvm_load_zero_bitmap(
                zero_bitmap, s_zero_bitmap.zero_bitmap_size, pfn_type, err_msg);
            if (ret)
                goto out;
            break;
	default:
            uxenvm_check_restore_clone(restore_mode);
            ret = uxenvm_load_batch(f, marker, pfn_type, pfn_err, pfn_info,
                                    &dc, do_lazy_load, populate_compressed,
                                    err_msg);
	    if (ret)
		goto out;
	    break;
	}
    }
#ifdef DECOMPRESS_THREADED
    if (dc.async_op_ctx) {
        ret = decompress_wait_all(&dc, err_msg);
        if (ret)
            goto out;
    }
#endif  /* DECOMPRESS_THREADED */

  skip_mem:
    if (vm_quit_interrupt)
        goto out;
    if (restore_mode == VM_RESTORE_TEMPLATE) {		/* template load */
        ret = xc_domain_sethandle(xc_handle, vm_id, vm_uuid);
        if (ret < 0) {
            asprintf(err_msg,
                     "xc_domain_sethandle(template uuid) failed");
            goto out;
        }
        /* we need to do apply_immutable_memory() only for the template.
        The HVMMEM_ram_immutable attribute is stored in the loaded template
        p2m structures, not in the guest's.
        It is checked only when unsharing a page.
        So, if we save/restore a ucVM, then this information is still
        preserved in loaded template p2m structures, not in ucvm's savefile.
        */
        if (immutable_ranges)
            apply_immutable_memory(immutable_ranges,
                                   s_hvm_introspec.info.n_immutable_ranges);
	goto out;
    }

    if (s_mapcache_params.marker == XC_SAVE_ID_MAPCACHE_PARAMS)
        mapcache_init_restore(s_mapcache_params.end_low_pfn,
                              s_mapcache_params.start_high_pfn,
                              s_mapcache_params.end_high_pfn);
    else
        mapcache_init(vm_mem_mb);

    if (load_lazy_load_info) {
        uint64_t page_offsets_pos = 0;

        if (vm_template_file) {
            lli->fb = filebuf_open(vm_template_file, "rb");
            if (!lli->fb) {
                ret = -errno;
                asprintf(err_msg,
                         "uxenvm_open(vm_template_file = %s) failed",
                         vm_template_file);
                goto out;
            }
        } else
            lli->fb = filebuf_openref(f);

        filebuf_buffer_max(lli->fb, PAGE_SIZE);
        filebuf_seek(lli->fb, 0, FILEBUF_SEEK_END);
        while (1) {
            struct xc_save_index index;

            filebuf_seek(lli->fb, -(off_t)sizeof(index), FILEBUF_SEEK_CUR);
            uxenvm_load_read(lli->fb, &index, sizeof(index), ret, err_msg, out);
            if (!index.marker)
                break;
            switch (index.marker) {
            case XC_SAVE_ID_PAGE_OFFSETS:
                page_offsets_pos = index.offset;
                break;
            }
            filebuf_seek(lli->fb, -(off_t)sizeof(index), FILEBUF_SEEK_CUR);
        }

        if (page_offsets_pos) {
            filebuf_seek(lli->fb, page_offsets_pos, FILEBUF_SEEK_SET);
            uxenvm_load_read(lli->fb, &s_vm_page_offsets.marker,
                             sizeof(s_vm_page_offsets.marker), ret, err_msg,
                             out);
            if (s_vm_page_offsets.marker != XC_SAVE_ID_PAGE_OFFSETS) {
                asprintf(err_msg, "page_offsets index corrupt, no page offsets "
                         "index at offset %"PRId64, page_offsets_pos);
                ret = -EINVAL;
                goto out;
            }
            uxenvm_load_read_struct(lli->fb, s_vm_page_offsets,
                                    XC_SAVE_ID_PAGE_OFFSETS, ret, err_msg, out);
            lli->max_gpfn = s_vm_page_offsets.pfn_off_nr;
            if (lli->max_gpfn > PCI_HOLE_START_PFN)
                lli->max_gpfn += PCI_HOLE_END_PFN - PCI_HOLE_START_PFN;
            page_offsets_pos += sizeof(s_vm_page_offsets);
            APRINTF("lazy load index: pos %"PRId64" size %"PRIdSIZE
                    " nr off %d", page_offsets_pos,
                    s_vm_page_offsets.pfn_off_nr * sizeof(lli->pfn_off[0]),
                    s_vm_page_offsets.pfn_off_nr);
            lli->pfn_off = (uint64_t *)filebuf_mmap(
                lli->fb, page_offsets_pos,
                s_vm_page_offsets.pfn_off_nr * sizeof(lli->pfn_off[0]));
        }
    }

    if (s_tsc_info.marker == XC_SAVE_ID_TSC_INFO)
	xc_domain_set_tsc_info(xc_handle, vm_id, s_tsc_info.tsc_mode,
			       s_tsc_info.nsec, s_tsc_info.khz,
			       s_tsc_info.incarn);
    if (s_vcpu_info.marker == XC_SAVE_ID_VCPU_INFO)
	;
    if (s_hvm_ident_pt.marker == XC_SAVE_ID_HVM_IDENT_PT)
	xc_set_hvm_param(xc_handle, vm_id, HVM_PARAM_IDENT_PT,
			 s_hvm_ident_pt.data);
    if (s_hvm_vm86_tss.marker == XC_SAVE_ID_HVM_VM86_TSS)
	xc_set_hvm_param(xc_handle, vm_id, HVM_PARAM_VM86_TSS,
			 s_hvm_vm86_tss.data);
    if (s_hvm_console_pfn.marker == XC_SAVE_ID_HVM_CONSOLE_PFN) {
	xc_clear_domain_page(xc_handle, vm_id, s_hvm_console_pfn.data);
	xc_set_hvm_param(xc_handle, vm_id, HVM_PARAM_CONSOLE_PFN,
			 s_hvm_console_pfn.data);
    }
    if (s_hvm_acpi_ioports_location.marker ==
	XC_SAVE_ID_HVM_ACPI_IOPORTS_LOCATION)
	xc_set_hvm_param(xc_handle, vm_id, HVM_PARAM_ACPI_IOPORTS_LOCATION,
			 s_hvm_acpi_ioports_location.data);
    if (s_hvm_magic_pfns.marker == XC_SAVE_ID_HVM_MAGIC_PFNS) {
	uint64_t pfn;
	xc_set_hvm_param(xc_handle, vm_id, HVM_PARAM_IO_PFN_FIRST,
			 s_hvm_magic_pfns.magic_pfns[0]);
	xc_set_hvm_param(xc_handle, vm_id, HVM_PARAM_IO_PFN_LAST,
			 s_hvm_magic_pfns.magic_pfns[1]);
	for (pfn = s_hvm_magic_pfns.magic_pfns[0];
	     pfn <= s_hvm_magic_pfns.magic_pfns[1]; pfn++)
	    xc_clear_domain_page(xc_handle, vm_id, pfn);
        if (!s_hvm_magic_pfns.magic_pfns[2]) /* XXX ignore 0 for now */
            s_hvm_magic_pfns.magic_pfns[2] = -1;
        if (s_hvm_magic_pfns.magic_pfns[2] != -1) {
            ret = xc_domain_add_to_physmap(xc_handle, vm_id,
                                           XENMAPSPACE_shared_info, 0,
                                           s_hvm_magic_pfns.magic_pfns[2]);
            if (ret < 0) {
                asprintf(err_msg, "add_to_physmap(shared_info) failed");
                goto out;
            }
        }
        xc_clear_domain_page(xc_handle, vm_id, s_hvm_magic_pfns.magic_pfns[3]);
        ret = xc_set_hvm_param(xc_handle, vm_id, HVM_PARAM_DMREQ_VCPU_PFN,
                               s_hvm_magic_pfns.magic_pfns[4]);
        if (ret < 0) {
            asprintf(err_msg, "set_hvm_param(HVM_PARAM_DMREQ_VCPU_PFN) = %"
                     PRIx64" failed", s_hvm_magic_pfns.magic_pfns[4]);
            goto out;
        }
        ret = xc_set_hvm_param(xc_handle, vm_id, HVM_PARAM_DMREQ_PFN,
                               s_hvm_magic_pfns.magic_pfns[3]);
        if (ret < 0) {
            asprintf(err_msg, "set_hvm_param(HVM_PARAM_DMREQ_PFN) = %"PRIx64
                     " failed", s_hvm_magic_pfns.magic_pfns[3]);
            goto out;
        }
        dmreq_init();
    }
    if (s_hvm_context.marker == XC_SAVE_ID_HVM_CONTEXT)
	xc_domain_hvm_setcontext(xc_handle, vm_id, hvm_buf, s_hvm_context.size);

    /* XXX pae? */

    ret = 0;
  out:
#ifndef DECOMPRESS_THREADED
    if (HYPERCALL_BUFFER_ARGUMENT_BUFFER(dc.pp_buffer))
        xc__hypercall_buffer_free_pages(xc_handle, dc.pp_buffer,
                                        PP_BUFFER_PAGES);
#else  /* DECOMPRESS_THREADED */
    if (dc.async_op_ctx)
        (void)decompress_wait_all(&dc, NULL);
#endif  /* DECOMPRESS_THREADED */
    free(pfn_err);
    free(pfn_info);
    free(pfn_type);
    free(hvm_buf);
    free(zero_bitmap);
    free(zero_bitmap_compressed);
    return ret;
}

static int
uxenvm_loadvm_execute_finish(char **err_msg)
{
    QEMUFile *mf = NULL;
    int ret;

    if (dm_state_load_size) {
	mf = qemu_memopen(dm_state_load_buf, dm_state_load_size, "rb");
	if (mf == NULL) {
	    asprintf(err_msg, "qemu_memopen(dm_state_load_buf, %d) failed",
		     dm_state_load_size);
	    ret = -ENOMEM;
	    goto out;
	}
	ret = qemu_loadvm_state(mf);
	if (ret < 0) {
	    asprintf(err_msg, "qemu_loadvm_state() failed");
	    goto out;
	}
    }

    vm_time_update();

    ret = 0;

  out:
    if (mf)
	qemu_fclose(mf);
    if (dm_state_load_buf) {
	free(dm_state_load_buf);
	dm_state_load_buf = NULL;
	dm_state_load_size = 0;
    }
    return ret;
}

int
vm_lazy_load_page(uint32_t gpfn, uint8_t *va, int compressed)
{
    int ret;
    uint64_t offset;
    static int lazy_compressed = 0;

    if (gpfn >= PCI_HOLE_START_PFN && gpfn < PCI_HOLE_END_PFN)
        errx(1, "%s: gpfn %x in pci hole", __FUNCTION__, gpfn);
    if (gpfn >= dm_lazy_load_info.max_gpfn)
        errx(1, "%s: gpfn %x too large, max_gpfn %x", __FUNCTION__,
             gpfn, dm_lazy_load_info.max_gpfn);

    offset = dm_lazy_load_info.pfn_off[skip_pci_hole(gpfn)];

    /* dprintf("%s: gpfn %x at file offset %"PRIu64" to %p\n", __FUNCTION__, */
    /*         gpfn, offset & PAGE_OFFSET_INDEX_PFN_OFF_MASK, va); */

    filebuf_seek(dm_lazy_load_info.fb, offset & PAGE_OFFSET_INDEX_PFN_OFF_MASK,
                 FILEBUF_SEEK_SET);

    if (offset & PAGE_OFFSET_INDEX_PFN_OFF_COMPRESSED) {
        cs16_t cs1;
        ret = filebuf_read(dm_lazy_load_info.fb, &cs1, sizeof(cs1));
        if (ret != sizeof(cs1)) {
            ret = -errno;
            warn("%s: filebuf_read(lazy load page) gpfn %x offset %"PRIu64
                 " read page size failed", __FUNCTION__, gpfn,
                 offset & PAGE_OFFSET_INDEX_PFN_OFF_MASK);
            goto out;
        }
        if (cs1 > PAGE_SIZE) {
            warnx("%s: filebuf_read(lazy load page) gpfn %x offset %"PRIu64
                  " invalid size: %d", __FUNCTION__, gpfn,
                  offset & PAGE_OFFSET_INDEX_PFN_OFF_MASK, cs1);
            ret = -EINVAL;
            goto out;
        }
        if (cs1 == PAGE_SIZE) {
            /* this codepath should not be taken, unless the save file
             * doesn't have the optimisation to not set
             * LAZY_LOAD_PFN_OFF_COMPRESSED for compressed in vain pages */
            ret = filebuf_read(dm_lazy_load_info.fb, va, PAGE_SIZE);
            if (ret != PAGE_SIZE) {
                ret = -errno;
                warn("%s: filebuf_read(lazy load page) gpfn %x offset %"PRIu64
                     " read %ld failed", __FUNCTION__, gpfn,
                     offset & PAGE_OFFSET_INDEX_PFN_OFF_MASK, PAGE_SIZE);
            }
            goto out;
        }
        if (lazy_compressed && compressed && cs1 <= (PAGE_SIZE - 256)) {
            /* load compressed data, decompress in uxen */
            ret = filebuf_read(dm_lazy_load_info.fb, va, cs1);
            if (ret != cs1) {
                ret = -errno;
                warn("%s: filebuf_read(lazy load page) gpfn %x offset %"PRIu64
                     " read %d failed", __FUNCTION__, gpfn,
                     offset & PAGE_OFFSET_INDEX_PFN_OFF_MASK, cs1);
                goto out;
            }
        } else {
            char page[PAGE_SIZE];
            ret = filebuf_read(dm_lazy_load_info.fb, &page[0], cs1);
            if (ret != cs1) {
                ret = -errno;
                warn("%s: filebuf_read(lazy load page) gpfn %x offset %"PRIu64
                     " read %d failed", __FUNCTION__, gpfn,
                     offset & PAGE_OFFSET_INDEX_PFN_OFF_MASK, cs1);
                goto out;
            }
            ret = LZ4_decompress_fast(&page[0], (char *)va, PAGE_SIZE);
            if (ret != cs1) {
                ret = -EINVAL;
                warnx("%s: decompress gpfn %x offset %"PRIu64" failed",
                      __FUNCTION__, gpfn,
                      offset & PAGE_OFFSET_INDEX_PFN_OFF_MASK);
                goto out;
            }
            ret = PAGE_SIZE;
        }
    } else {
        ret = filebuf_read(dm_lazy_load_info.fb, va, PAGE_SIZE);
        if (ret != PAGE_SIZE) {
            ret = -errno;
            warn("%s: filebuf_read(lazy load page) gpfn %x offset %"PRIu64
                 " failed", __FUNCTION__, gpfn,
                 offset & PAGE_OFFSET_INDEX_PFN_OFF_MASK);
        }
    }

  out:
    return ret;
}

/* 
 * globals used:
 * xc_handle: xc interface handle
 * vm_id: domain id
 * debug_printf: output error message
 * awaiting_suspend: flag indicating that suspend has been requested
 */
void
vm_save(void)
{
    char *err_msg = NULL;
    int ret;

    /* XXX init debug option */
    if (strstr(uxen_opt_debug, ",compbatch,"))
        vm_save_info.single_page = 0;

    vm_save_info.awaiting_suspend = 1;

    ret = uxenvm_savevm_initiate(&err_msg);
    if (ret) {
	if (err_msg)
            EPRINTF("%s: ret %d", err_msg, ret);
	return;
    }
}

#ifdef MONITOR
void
mc_savevm(Monitor *mon, const dict args)
{
    const char *filename;
    const char *c;

    filename = dict_get_string(args, "filename");
    vm_save_info.filename = filename ? strdup(filename) : NULL;

    vm_save_info.compress_mode = VM_SAVE_COMPRESS_NONE;
    c = dict_get_string(args, "compress");
    if (c) {
        if (!strcmp(c, "lz4"))
            vm_save_info.compress_mode = VM_SAVE_COMPRESS_LZ4;
    }

    vm_save_info.single_page = dict_get_boolean_default(args, "single-page", 1);
    vm_save_info.free_mem = dict_get_boolean_default(args, "free-mem", 1);
    vm_save_info.high_compress = dict_get_boolean_default(args,
                                                          "high-compress", 0);

    vm_save_info.save_abort = 0;
    vm_set_run_mode(SUSPEND_VM);
}

void
mc_resumevm(Monitor *mon, const dict args)
{

    vm_save_info.resume_delete =
        dict_get_boolean_default(args, "delete-savefile", 1);

    vm_set_run_mode(RUNNING_VM);
}
#endif  /* MONITOR */

int
vm_process_suspend(xc_dominfo_t *info)
{
 
    if (!info->shutdown || info->shutdown_reason != SHUTDOWN_suspend)
        return 0;

    APRINTF("vm is suspended");

    vm_save_info.save_requested = 1;
    vm_save_info.awaiting_suspend = 0;
    control_send_status("vm-runstate", "suspended", NULL);

    return 1;
}

char *vm_save_file_name(const uuid_t uuid)
{
    char *fn;
    char uuid_str[37];
    uuid_unparse_lower(uuid, uuid_str);
    asprintf(&fn, "%s%s.save", save_file_prefix, uuid_str);
    return fn;
}

#define ERRMSG(fmt, ...) do {			 \
	EPRINTF(fmt, ## __VA_ARGS__);		 \
	asprintf(&err_msg, fmt, ## __VA_ARGS__); \
    } while (0)

void
vm_save_execute(void)
{
    struct filebuf *f = NULL;
    char *err_msg = NULL;
    uint8_t *dm_state_buf = NULL;
    int dm_state_size;
    int ret;

    if (!vm_save_info.filename)
        vm_save_info.filename = vm_save_file_name(vm_uuid);

    APRINTF("device model saving state: %s", vm_save_info.filename);

    f = filebuf_open(vm_save_info.filename, "wb");
    if (f == NULL) {
	ret = errno;
	ERRMSG("filebuf_open(%s) failed", vm_save_info.filename);
	goto out;
    }
    filebuf_delete_on_close(f, 1);
    vm_save_info.f = f;

    ret = uxenvm_savevm_get_dm_state(&dm_state_buf, &dm_state_size, &err_msg);
    if (ret) {
	if (!err_msg)
	    asprintf(&err_msg, "uxenvm_savevm_get_dm_state() failed");
        EPRINTF("%s: ret %d", err_msg, ret);
	ret = -ret;
	goto out;
    }

    ret = uxenvm_savevm_write_info(f, dm_state_buf, dm_state_size, &err_msg);
    if (ret) {
	if (!err_msg)
	    asprintf(&err_msg, "uxenvm_savevm_write_info() failed");
        EPRINTF("%s: ret %d", err_msg, ret);
	ret = -ret;
	goto out;
    }

    ret = uxenvm_savevm_write_pages(f, &err_msg);
    if (ret) {
        if (!err_msg)
            asprintf(&err_msg, "uxenvm_savevm_write_pages() failed");
        EPRINTF("%s: ret %d", err_msg, ret);
        ret = -ret;
        goto out;
    }

  out:

    if (ret == 0) {
        APRINTF("total file size: %"PRIu64" bytes", (uint64_t)filebuf_tell(f));
        filebuf_flush(f);
    } else {
        filebuf_close(f);
        f = vm_save_info.f = NULL;
    }

    if (vm_save_info.command_cd)
	control_command_save_finish(ret, err_msg);
    if (dm_state_buf)
        free(dm_state_buf);
    if (err_msg)
	free(err_msg);
    free(vm_save_info.filename);
    vm_save_info.filename = NULL;
}

void
vm_save_finalize(void)
{

    if (vm_save_info.f) {
        if (!vm_quit_interrupt)
            filebuf_delete_on_close(vm_save_info.f, 0);
        filebuf_close(vm_save_info.f);
        vm_save_info.f = NULL;
    }
}

static int
vm_restore_memory(void)
{
    struct filebuf *f;
    xen_pfn_t *pfn_type = NULL;
    int *pfn_err = NULL, *pfn_info = NULL;
    struct decompress_ctx dc = { };
    int populate_compressed = 0;
    int do_lazy_load = 0;
    int32_t marker;
    struct xc_save_generic s_generic;
    char *err_msg = NULL;
#ifdef VERBOSE
    int count = 0;
#endif  /* VERBOSE */
    int ret = 0;

    if (!vm_save_info.f)
        errx(1, "%s: no file", __FUNCTION__);
    f = vm_save_info.f;

    if (!vm_save_info.page_batch_offset)
        errx(1, "%s: no page batch offset", __FUNCTION__);

    filebuf_set_readable(f);

    ret = filebuf_seek(f, vm_save_info.page_batch_offset,
                       FILEBUF_SEEK_SET) != -1 ? 0 : -1;
    if (ret < 0) {
        asprintf(&err_msg, "filebuf_seek(vm_page_offsets) failed");
        goto out;
    }

    ret = uxenvm_load_alloc(&pfn_type, &pfn_err, &pfn_info, &err_msg);
    if (ret < 0)
        goto out;

    while (1) {
        uxenvm_load_read(f, &marker, sizeof(marker), ret, &err_msg, out);
	if (marker == 0)	/* end marker */
	    break;
        switch (marker) {
        case XC_SAVE_ID_PAGE_OFFSETS:
        case XC_SAVE_ID_ZERO_BITMAP:
            uxenvm_load_read_struct(f, s_generic, marker, ret, &err_msg,
                                    out);
            ret = filebuf_seek(f, s_generic.size - sizeof(s_generic),
                               FILEBUF_SEEK_CUR) != -1 ? 0 : -EIO;
            if (ret < 0) {
                asprintf(&err_msg, "filebuf_seek(%d, SEEK_CUR) failed",
                         s_generic.size);
                goto out;
            }
            break;
        default:
            ret = uxenvm_load_batch(f, marker, pfn_type, pfn_err, pfn_info,
                                    &dc, do_lazy_load, populate_compressed,
                                    &err_msg);
            if (ret)
                goto out;
#ifdef VERBOSE
            while (marker > MAX_BATCH_SIZE)
                marker -= MAX_BATCH_SIZE;
            count += marker;
#endif  /* VERBOSE */
            break;
        }
    }
#ifdef VERBOSE
    DPRINTF("%s: %d pages", __FUNCTION__, count);
#endif  /* VERBOSE */

  out:
#ifndef DECOMPRESS_THREADED
    if (HYPERCALL_BUFFER_ARGUMENT_BUFFER(dc.pp_buffer))
        xc__hypercall_buffer_free_pages(xc_handle, dc.pp_buffer,
                                        PP_BUFFER_PAGES);
#else  /* DECOMPRESS_THREADED */
    if (dc.async_op_ctx)
        (void)decompress_wait_all(&dc, NULL);
#endif  /* DECOMPRESS_THREADED */
    free(pfn_err);
    free(pfn_info);
    free(pfn_type);
    if (ret < 0 && err_msg)
        EPRINTF("%s: ret %d", err_msg, ret);
    free(err_msg);
    return ret;
}

int
vm_load(const char *name, int restore_mode)
{
    struct filebuf *f;
    char *err_msg = NULL;
    int ret = 0;

    APRINTF("device model loading state: %s", name);

    f = filebuf_open(name, restore_mode == VM_RESTORE_TEMPLATE ? "rbn" : "rb");
    if (f == NULL) {
	ret = -errno;
        asprintf(&err_msg, "filebuf_open(%s) failed", name);
        EPRINTF("%s: ret %d", err_msg, ret);
	goto out;
    }

    ret = uxenvm_loadvm_execute(f, restore_mode, &err_msg);
    if (ret) {
	if (err_msg)
            EPRINTF("%s: ret %d", err_msg, ret);
	goto out;
    }

    /* 1st generation clone, record name as template filename */
    if (restore_mode == VM_RESTORE_CLONE && !vm_template_file)
        vm_template_file = strdup(name);

  out:
    if (f)
	filebuf_close(f);

    if (ret) {
        _set_errno(-ret);
        return -1;
    }
    return 0;
}

int
vm_load_finish(void)
{
    char *err_msg = NULL;
    int ret;

    ret = uxenvm_loadvm_execute_finish(&err_msg);
    if (ret) {
	if (err_msg)
            EPRINTF("%s: ret %d", err_msg, ret);
    }

    return ret;
}

int
vm_resume(void)
{
    int ret;
    char *err_msg = NULL;

    if (vm_save_info.f) {
        if (vm_save_info.free_mem)
            vm_restore_memory();

        qemu_savevm_resume();

        if (!vm_save_info.resume_delete)
            filebuf_delete_on_close(vm_save_info.f, 0);
        filebuf_close(vm_save_info.f);
        vm_save_info.f = NULL;
    }

    ret = xc_domain_resume(xc_handle, vm_id);
    if (ret) {
        if (!err_msg)
            asprintf(&err_msg, "xc_domain_resume failed");
        EPRINTF("%s: ret %d", err_msg, -ret);
        ret = -ret;
        goto out;
    }

  out:
    if (vm_save_info.resume_cd)
        control_command_resume_finish(ret, err_msg);
    return ret;
}
