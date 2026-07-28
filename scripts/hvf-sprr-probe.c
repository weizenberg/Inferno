/*
 * Hypervisor.framework SPRR compatibility probe.
 *
 * Copyright (c) 2026 Weizenberg.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Hypervisor/Hypervisor.h>
#include <inttypes.h>
#include <libkern/OSCacheControl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Hypervisor.framework's hv_sys_reg_t values use the architectural system
 * register encoding directly. SPRR_EL0BR0_EL1 is S3_6_C15_C1_5.
 */
#define SPRR_EL0BR0_EL1 ((hv_sys_reg_t)0xf78d)
#define SPRR_CONFIG_EL0 ((hv_sys_reg_t)0xf789)

static int check(const char *operation, hv_return_t result)
{
    if (result == HV_SUCCESS) {
        return 0;
    }

    fprintf(stderr, "%s failed: 0x%x\n", operation, result);
    return 1;
}

static int expect(const char *description, bool condition)
{
    if (condition) {
        return 0;
    }

    fprintf(stderr, "unexpected result: %s\n", description);
    return 1;
}

int main(void)
{
    hv_vcpu_exit_t *exit;
    hv_vcpu_t vcpu;
    uint32_t *code;
    size_t page_size = (size_t)getpagesize();
    uint64_t value;
    uint64_t host_sprr;
    hv_return_t result;
    int status = 0;

    __asm__ volatile("mrs %0, S3_6_C15_C1_5" : "=r"(host_sprr));
    printf("host SPRR_EL0BR0_EL1: 0x%016" PRIx64 "\n", host_sprr);

    code = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                -1, 0);
    if (code == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(code, 0, page_size);

    /*
     * EL0 code:
     *   msr S3_6_C15_C1_5, x0
     *   svc #0
     *
     * Either exception enters the lower-EL synchronous vector. HVC there
     * exits to the VMM, after which ESR_EL1 tells us whether the MSR ran
     * successfully (SVC, EC 0x15) or raised UNDEFINED (EC 0x00).
     */
    code[0x1000 / sizeof(*code)] = 0xd51ef1a0;
    code[0x1004 / sizeof(*code)] = 0xd4000001;
    code[0x2400 / sizeof(*code)] = 0xd4000002;

    result = hv_vm_create(NULL);
    if (check("hv_vm_create", result)) {
        return 1;
    }
    result = hv_vm_map(code, 0, page_size, HV_MEMORY_READ | HV_MEMORY_WRITE);
    if (check("hv_vm_map", result)) {
        hv_vm_destroy();
        return 1;
    }

    result = hv_vcpu_create(&vcpu, &exit, NULL);
    if (check("hv_vcpu_create", result)) {
        hv_vm_destroy();
        return 1;
    }

    result = hv_vcpu_get_sys_reg(vcpu, SPRR_EL0BR0_EL1, &value);
    if (result == HV_SUCCESS) {
        printf("SPRR_EL0BR0_EL1 read: 0x%016" PRIx64 "\n", value);
    } else {
        fprintf(stderr, "SPRR_EL0BR0_EL1 read unsupported: 0x%x\n", result);
        status = 2;
    }
    result = hv_vcpu_get_sys_reg(vcpu, SPRR_CONFIG_EL0, &value);
    if (result == HV_SUCCESS) {
        printf("SPRR_CONFIG_EL0 read: 0x%016" PRIx64 "\n", value);
        result = hv_vcpu_set_sys_reg(vcpu, SPRR_CONFIG_EL0, value | 1);
        status |= check("enable SPRR_CONFIG_EL0", result);
    } else {
        fprintf(stderr, "SPRR_CONFIG_EL0 read unsupported: 0x%x\n", result);
    }

    status |= check("set VBAR_EL1",
                    hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, 0x2000));
    status |= check("set X0", hv_vcpu_set_reg(vcpu, HV_REG_X0, 0));
    status |= check("set PC", hv_vcpu_set_reg(vcpu, HV_REG_PC, 0x1000));
    status |= check("set CPSR", hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0));
    if (!status) {
        result = hv_vcpu_run(vcpu);
        status |= check("hv_vcpu_run", result);
        printf("non-executable exit reason: %u, syndrome: 0x%016" PRIx64
               ", IPA: 0x%016" PRIx64 "\n",
               exit->reason, exit->exception.syndrome,
               exit->exception.physical_address);
        status |= expect("stage-2 execute fault",
                         exit->reason == HV_EXIT_REASON_EXCEPTION &&
                             (exit->exception.syndrome >> 26) == 0x20 &&
                             exit->exception.physical_address == 0x1000);

        status |= check(
            "make code executable",
            hv_vm_protect(0, page_size,
                          HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
        result = hv_vcpu_run(vcpu);
        status |= check("unpatched hv_vcpu_run", result);
        printf("unpatched exit reason: %u, syndrome: 0x%016" PRIx64 "\n",
               exit->reason, exit->exception.syndrome);
        status |= check("get ESR_EL1",
                        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &value));
        printf("ESR_EL1 after native EL0 SPRR write: 0x%016" PRIx64
               " (EC 0x%02" PRIx64 ")\n",
               value, value >> 26);
        status |= expect("native SPRR write raises UNDEFINED",
                         exit->reason == HV_EXIT_REASON_EXCEPTION &&
                             (exit->exception.syndrome >> 26) == 0x16 &&
                             (value >> 26) == 0x00);

        code[0x1000 / sizeof(*code)] = 0xd503201f;
        sys_icache_invalidate(&code[0x1000 / sizeof(*code)], sizeof(*code));
        status |= check("reset PC", hv_vcpu_set_reg(vcpu, HV_REG_PC, 0x1000));
        status |= check("reset CPSR", hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0));
        result = hv_vcpu_run(vcpu);
        status |= check("post-patch hv_vcpu_run", result);
        printf("post-patch exit reason: %u, syndrome: 0x%016" PRIx64 "\n",
               exit->reason, exit->exception.syndrome);
        status |= check("get post-patch ESR_EL1",
                        hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &value));
        printf("ESR_EL1 after EL0 SPRR write: 0x%016" PRIx64 " (EC 0x%02" PRIx64
               ")\n",
               value, value >> 26);
        status |= expect("NOP compatibility reaches SVC",
                         exit->reason == HV_EXIT_REASON_EXCEPTION &&
                             (exit->exception.syndrome >> 26) == 0x16 &&
                             (value >> 26) == 0x15);
    }

    result = hv_vcpu_destroy(vcpu);
    status |= check("hv_vcpu_destroy", result);
    result = hv_vm_unmap(0, page_size);
    status |= check("hv_vm_unmap", result);
    result = hv_vm_destroy();
    status |= check("hv_vm_destroy", result);
    munmap(code, page_size);

    return status;
}
