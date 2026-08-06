// Copyright 2016 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// Tests hypervisors if capabilities are available.
// For now, this is Mac-only.

#include <gtest/gtest.h>

#ifdef __APPLE__

#include <sys/sysctl.h>

#if defined(__arm64__) || defined(__aarch64__)
#include <Hypervisor/Hypervisor.h>
#else
#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#endif

static bool isRunningInNestedVm() {
    int vmm = 0;
    size_t len = sizeof(vmm);
    if (sysctlbyname("kern.hv_vmm_present", &vmm, &len, nullptr, 0) == 0) {
        return vmm != 0;
    }
    return false;
}

TEST(HypervisorTest, HypervisorFrameworkVmCreate) {
#if defined(__arm64__) || defined(__aarch64__)
    int res = hv_vm_create(0);
#else
    int res = hv_vm_create(HV_VM_DEFAULT);
#endif
    if (isRunningInNestedVm() && res != HV_SUCCESS) {
        GTEST_SKIP() << "Running in nested macOS VM where HVF is not exposed";
        return;
    }
    EXPECT_EQ(HV_SUCCESS, res);
    hv_vm_destroy();
}

TEST(HypervisorTest, HVF_MapRequiresPageAlignmentAndUnmap) {
#if defined(__arm64__) || defined(__aarch64__)
    int res = hv_vm_create(0);
#else
    int res = hv_vm_create(HV_VM_DEFAULT);
#endif
    if (isRunningInNestedVm() && res != HV_SUCCESS) {
        GTEST_SKIP() << "Running in nested macOS VM where HVF is not exposed";
        return;
    }
    EXPECT_EQ(HV_SUCCESS, res);

    const size_t hostPageSize = getpagesize();

    void* hva = nullptr;
    posix_memalign(&hva, hostPageSize, hostPageSize);
    ASSERT_NE(nullptr, hva);

    uint64_t gpa = 0x10000000;

    // 1. Unmapping a GPA that was never mapped does not crash; it cleanly returns HV_SUCCESS
    int unmapNeverMappedRes = hv_vm_unmap(gpa, hostPageSize);
    EXPECT_EQ(HV_SUCCESS, unmapNeverMappedRes);

    // 2. Mapping an unaligned size (512 bytes) fails natively in HVF without crashing
    int unalignedRes = hv_vm_map(hva, gpa, 512, HV_MEMORY_READ | HV_MEMORY_WRITE);
    EXPECT_NE(HV_SUCCESS, unalignedRes);

    // 3. Mapping a page-aligned size succeeds
    int mapRes = hv_vm_map(hva, gpa, hostPageSize, HV_MEMORY_READ | HV_MEMORY_WRITE);
    EXPECT_EQ(HV_SUCCESS, mapRes);

    // 4. Mapping over an active GPA without unmapping fails in HVF
    int mapAgainRes = hv_vm_map(hva, gpa, hostPageSize, HV_MEMORY_READ | HV_MEMORY_WRITE);
    EXPECT_NE(HV_SUCCESS, mapAgainRes);

    // 5. Unmapping first allows remapping to succeed
    int unmapRes = hv_vm_unmap(gpa, hostPageSize);
    EXPECT_EQ(HV_SUCCESS, unmapRes);

    mapRes = hv_vm_map(hva, gpa, hostPageSize, HV_MEMORY_READ | HV_MEMORY_WRITE);
    EXPECT_EQ(HV_SUCCESS, mapRes);

    // 6. Clean up: unmap the slot
    unmapRes = hv_vm_unmap(gpa, hostPageSize);
    EXPECT_EQ(HV_SUCCESS, unmapRes);

    // 7. Calling unmap a second time (double-unmap) does not crash; it is idempotent and returns HV_SUCCESS
    int unmapTwiceRes = hv_vm_unmap(gpa, hostPageSize);
    EXPECT_EQ(HV_SUCCESS, unmapTwiceRes);

    free(hva);
    hv_vm_destroy();
}

#endif
