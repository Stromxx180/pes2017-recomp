#include "guest_memory.h"
#include <initializer_list>
#include "log.h"
#include <ppc_config.h>

#ifdef _WIN32
#  error "the shared-bank memory model (one bank at 0x80000000/0xA0000000/0xC0000000) is POSIX-only so far"
#else
#  include <sys/mman.h>
#  include <unistd.h>
#  if !defined(__APPLE__)
#    include <sys/syscall.h>
#  endif
#  include <fcntl.h>
#  include <cstdlib>
#  include <string>
#  ifdef PES17_ANDROID
#    include "android/android.h"
#  endif
#endif

namespace
{
    uint8_t* g_base = nullptr;
    constexpr uint64_t kSpace = 0x100000000ull;   // full 32-bit guest space
}

uint8_t* guest::Base() { return g_base; }

bool guest::Commit(uint32_t address, uint32_t size)
{
    (void)address; (void)size;
    return true;
}

bool guest::CommitOnTouch(const void* address)
{
    (void)address;
    return false;
}

namespace
{
    bool ReserveSpace()
    {
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
        g_base = (uint8_t*)mmap(nullptr, kSpace, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (g_base == MAP_FAILED) g_base = nullptr;
        return g_base != nullptr;
    }

    int OpenBankDescriptor()
    {
#if defined(__APPLE__)
        const char* tmp = std::getenv("TMPDIR");
        std::string path = std::string(tmp ? tmp : "/tmp") + "/pes17_physical.bank";
        const int file = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (file >= 0)
        {
            unlink(path.c_str());
            return file;
        }
        return -1;
#else
        const int fd = int(syscall(SYS_memfd_create, "pes17-physical", 0));
        if (fd >= 0) return fd;
#ifdef PES17_ANDROID
        const std::string path = android::GetPaths().cache + "/physical.bank";
        const int file = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (file >= 0)
        {
            unlink(path.c_str());
            LOGW("memfd_create refused; the physical bank is an unlinked cache file");
            return file;
        }
#endif
        return -1;
#endif
    }

    bool MapPhysicalAperture()
    {
        const size_t size = size_t(guest::kPhysicalEnd - guest::kPhysicalBase);
        const int fd = OpenBankDescriptor();
        if (fd < 0) { LOGE("no descriptor for the physical bank"); return false; }
        bool ok = ftruncate(fd, off_t(size)) == 0;
        for (uint32_t window : { guest::kCachedBase, guest::kPhysicalBase, guest::kApertureBase })
        {
            if (!ok) break;
            ok = mmap(g_base + window, size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_FIXED | MAP_NORESERVE, fd, 0) != MAP_FAILED;
        }
        close(fd);
        return ok;
    }
}

uint8_t* guest::Initialise()
{
    LOGI("host pages are %ld bytes", sysconf(_SC_PAGESIZE));
    if (!ReserveSpace())
    {
        LOGE("could not reserve %llu bytes of address space", (unsigned long long)kSpace);
        return nullptr;
    }
    if (!MapPhysicalAperture())
    {
        LOGE("could not alias the physical aperture at %08X", kApertureBase);
        return nullptr;
    }

    const uint32_t funcTableBase = kFuncTableBase;
    const uint32_t funcTableSize = uint32_t(PPC_CODE_SIZE * 2);

    if (!Commit(uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_SIZE)) ||
        !Commit(funcTableBase, funcTableSize) ||
        !Commit(kHeapBase, kHeapEnd - kHeapBase))
    {
        LOGE("failed to commit guest regions");
        return nullptr;
    }

    LOGI("guest space at host %p", (void*)g_base);
    return g_base;
}
