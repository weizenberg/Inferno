/*
 * Apple auxiliary kernel collection boot handoff.
 *
 * Copyright (c) 2026 Inferno contributors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/boot.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "system/memory.h"

// Bound the allocation for this experimental, uncompressed fileset input.
#define AUXKC_MAX_SIZE (64 * MiB)
#define AUXKC_PAGE_SIZE 0x4000

typedef struct AuxKCSegment {
    uint64_t addr;
    uint64_t size;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t prot;
    bool linkedit;
} AuxKCSegment;

static int auxkc_segment_compare(const void *a, const void *b)
{
    const AuxKCSegment *sa = a;
    const AuxKCSegment *sb = b;

    return (sa->addr > sb->addr) - (sa->addr < sb->addr);
}

bool apple_boot_load_auxkc(const char *filename, AddressSpace *as,
                           AppleBootInfo *info, Error **errp)
{
    g_autofree char *file = NULL;
    g_autofree uint8_t *image = NULL;
    g_autoptr(GArray) segments =
        g_array_new(false, false, sizeof(AuxKCSegment));
    g_autoptr(GError) read_error = NULL;
    gsize length;
    uint64_t extent = 0, ro_start = UINT64_MAX, header_addr = UINT64_MAX;
    uint64_t base, end;
    uint32_t ncmds, cmdbytes, filesets = 0;
    size_t off, cmds_end;

    if (!g_file_get_contents(filename, &file, &length, &read_error)) {
        error_setg(errp, "Cannot read AuxKC: %s", read_error->message);
        return false;
    }
    if (length < sizeof(MachoHeader64) || length > AUXKC_MAX_SIZE ||
        ldl_le_p(file) != MACH_MAGIC_64 || ldl_le_p(file + 4) != 0x0100000c ||
        ldl_le_p(file + 12) != MH_FILESET) {
        error_setg(errp, "AuxKC must be an arm64 MH_FILESET of at most 64 MiB");
        return false;
    }
    ncmds = ldl_le_p(file + 16);
    cmdbytes = ldl_le_p(file + 20);
    if (cmdbytes > length - sizeof(MachoHeader64) || ncmds > cmdbytes / 8) {
        error_setg(errp, "AuxKC load commands exceed the file");
        return false;
    }
    off = sizeof(MachoHeader64);
    cmds_end = off + cmdbytes;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd, size;

        if (cmds_end - off < 8) {
            goto bad_commands;
        }
        cmd = ldl_le_p(file + off);
        size = ldl_le_p(file + off + 4);
        if (size < 8 || (size & 7) || size > cmds_end - off) {
            goto bad_commands;
        }
        if (cmd == LC_SEGMENT_64) {
            AuxKCSegment seg;
            uint32_t maxprot, nsects;

            if (size < sizeof(MachoSegmentCommand64)) {
                goto bad_commands;
            }
            seg.addr = ldq_le_p(file + off + 24);
            seg.size = ldq_le_p(file + off + 32);
            seg.fileoff = ldq_le_p(file + off + 40);
            seg.filesize = ldq_le_p(file + off + 48);
            maxprot = ldl_le_p(file + off + 56);
            seg.prot = ldl_le_p(file + off + 60);
            nsects = ldl_le_p(file + off + 64);
            seg.linkedit = !strncmp(file + off + 8, "__LINKEDIT", 16);
            if (nsects > (size - sizeof(MachoSegmentCommand64)) /
                             sizeof(MachoSection64) ||
                !seg.size || (seg.addr | seg.size) % AUXKC_PAGE_SIZE ||
                seg.addr > AUXKC_MAX_SIZE ||
                seg.size > AUXKC_MAX_SIZE - seg.addr || seg.fileoff > length ||
                seg.filesize > length - seg.fileoff ||
                seg.filesize > seg.size || maxprot != seg.prot ||
                (seg.prot != VM_PROT_READ &&
                 seg.prot != (VM_PROT_READ | VM_PROT_WRITE) &&
                 seg.prot != (VM_PROT_READ | VM_PROT_EXECUTE))) {
                error_setg(
                    errp,
                    "AuxKC segment has unsupported bounds or permissions");
                return false;
            }
            if (seg.fileoff == 0 && seg.filesize >= cmds_end) {
                if (header_addr != UINT64_MAX) {
                    goto bad_commands;
                }
                header_addr = seg.addr;
            }
            if (!(seg.prot & VM_PROT_WRITE) && !seg.linkedit) {
                ro_start = MIN(ro_start, seg.addr);
            }
            g_array_append_val(segments, seg);
        } else if (cmd == LC_FILESET_ENTRY) {
            uint64_t childoff;
            uint32_t idoff;

            if (size < sizeof(MachoFilesetEntryCommand)) {
                goto bad_commands;
            }
            childoff = ldq_le_p(file + off + 16);
            idoff = ldl_le_p(file + off + 24);
            if (childoff > length - sizeof(MachoHeader64) ||
                ldl_le_p(file + childoff) != MACH_MAGIC_64 ||
                idoff < sizeof(MachoFilesetEntryCommand) || idoff >= size ||
                !memchr(file + off + idoff, 0, size - idoff)) {
                goto bad_commands;
            }
            filesets++;
        }
        off += size;
    }
    if (off != cmds_end || !filesets || !segments->len ||
        header_addr == UINT64_MAX || ro_start == UINT64_MAX ||
        header_addr < ro_start) {
        goto bad_commands;
    }
    g_array_sort(segments, auxkc_segment_compare);
    for (unsigned i = 0; i < segments->len; i++) {
        AuxKCSegment *seg = &g_array_index(segments, AuxKCSegment, i);

        if ((!i && seg->addr != 0) || seg->addr < extent ||
            ((seg->prot & VM_PROT_WRITE) && seg->addr + seg->size > ro_start)) {
            error_setg(errp, "AuxKC requires disjoint segments, VM base zero, "
                             "and writable data below read-only data");
            return false;
        }
        for (unsigned j = 0; j < i; j++) {
            AuxKCSegment *prev = &g_array_index(segments, AuxKCSegment, j);

            if (seg->filesize && prev->filesize &&
                seg->fileoff < prev->fileoff + prev->filesize &&
                prev->fileoff < seg->fileoff + seg->filesize) {
                error_setg(errp, "AuxKC segment file ranges overlap");
                return false;
            }
        }
        extent = seg->addr + seg->size;
    }

    // Fileset entries use VM addresses at runtime; a valid file offset alone
    // is insufficient. Fixup data must also remain in file-backed LINKEDIT.
    off = sizeof(MachoHeader64);
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = ldl_le_p(file + off);
        uint32_t size = ldl_le_p(file + off + 4);
        uint64_t dataoff, datasize;
        bool found = false;

        if (cmd == LC_FILESET_ENTRY) {
            dataoff = ldq_le_p(file + off + 16);
            datasize =
                sizeof(MachoHeader64) + (uint64_t)ldl_le_p(file + dataoff + 20);
            if (ldl_le_p(file + dataoff + 16) >
                (datasize - sizeof(MachoHeader64)) / 8) {
                goto bad_commands;
            }
        } else if (cmd == LC_DYLD_CHAINED_FIXUPS) {
            if (size != 16) {
                goto bad_commands;
            }
            dataoff = ldl_le_p(file + off + 8);
            datasize = ldl_le_p(file + off + 12);
            if (datasize < 28) {
                goto bad_commands;
            }
        } else {
            off += size;
            continue;
        }
        for (unsigned j = 0; j < segments->len; j++) {
            AuxKCSegment *seg = &g_array_index(segments, AuxKCSegment, j);

            if (dataoff < seg->fileoff ||
                dataoff - seg->fileoff > seg->filesize ||
                datasize > seg->filesize - (dataoff - seg->fileoff)) {
                continue;
            }
            if (cmd == LC_FILESET_ENTRY) {
                found = ldq_le_p(file + off + 8) ==
                        seg->addr + dataoff - seg->fileoff;
            } else {
                found = seg->linkedit;
            }
            break;
        }
        if (!found) {
            error_setg(
                errp,
                "AuxKC child header or fixup data is outside its segment");
            return false;
        }
        off += size;
    }

    // XNU requires the AuxKC's RO end (including LINKEDIT) to abut the
    // previous lowest boot region. t8030 places its TrustCache there.
    end = info->trustcache_addr;
    if ((end & (AUXKC_PAGE_SIZE - 1)) || end < info->dram_base ||
        end - info->dram_base > info->dram_size ||
        extent > end - info->dram_base) {
        error_setg(errp, "No room for AuxKC immediately below TrustCache");
        return false;
    }
    base = end - extent;
    image = g_malloc0(extent);
    for (unsigned i = 0; i < segments->len; i++) {
        AuxKCSegment *seg = &g_array_index(segments, AuxKCSegment, i);

        memcpy(image + seg->addr, file + seg->fileoff, seg->filesize);
    }
    // Keep all linked addresses and chained pointers intact. XNU applies the
    // collection slide and signs pointers after discovering these DT ranges.
    if (address_space_write(as, base, MEMTXATTRS_UNSPECIFIED, image, extent) !=
        MEMTX_OK) {
        error_setg(errp, "Failed to write AuxKC into guest RAM");
        return false;
    }
    info->auxkc_addr = base;
    info->auxkc_size = extent;
    info->auxkc_header_addr = base + header_addr;
    info->auxkc_ro_addr = base + ro_start;
    return true;

bad_commands:
    error_setg(errp, "Malformed or unsupported AuxKC load commands");
    return false;
}
