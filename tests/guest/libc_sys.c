// sysctl/sysctlbyname, getpwuid, getrlimit/getrusage, uname, mach calls.
#include <mach/mach.h>
#include <pwd.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "libc_check.h"

_Static_assert(sizeof(struct passwd) == 40 && sizeof(struct rusage) == 72 && sizeof(struct kinfo_proc) == 492, "layouts");

int main(void) {
    int v = 0;
    size_t l = sizeof v;
    CHECK(sysctlbyname("hw.cputype", &v, &l, NULL, 0) == 0 && v == 7 && l == 4, "hw.cputype %d", v);
    CHECK(sysctlbyname("hw.cpu64bit_capable", &v, &l, NULL, 0) == 0 && v == 1, "cpu64bit_capable");
    CHECK(sysctlbyname("sysctl.proc_native", &v, &l, NULL, 0) == 0 && v == 1, "proc_native");
    int ncpu = 0;
    l = sizeof ncpu;
    CHECK(sysctlbyname("hw.ncpu", &ncpu, &l, NULL, 0) == 0 && ncpu >= 1, "hw.ncpu");
    int mib[2] = {CTL_HW, HW_NCPU};
    v = 0;
    l = sizeof v;
    CHECK(sysctl(mib, 2, &v, &l, NULL, 0) == 0 && v == ncpu, "sysctl HW_NCPU");
    CHECK(sysctlbyname("hw.logicalcpu_max", &v, &l, NULL, 0) == 0 && v >= 1, "logicalcpu_max");
    uint64_t mem = 0;
    l = sizeof mem;
    CHECK(sysctlbyname("hw.memsize", &mem, &l, NULL, 0) == 0 && l == 8 && mem > (1ULL << 30), "hw.memsize");
    mib[1] = HW_PHYSMEM;
    uint32_t pm = 0;
    l = sizeof pm;
    CHECK(sysctl(mib, 2, &pm, &l, NULL, 0) == 0 && l == 4 && pm > 0 && pm <= 0x80000000u, "hw.physmem %u", pm);
    uint64_t freq = 0;
    l = sizeof freq;
    CHECK(sysctlbyname("hw.cpufrequency", &freq, &l, NULL, 0) == 0 && freq > 100000000ULL, "hw.cpufrequency");
    char str[256];
    l = 0;
    CHECK(sysctlbyname("hw.machine", NULL, &l, NULL, 0) == 0 && l == 5, "hw.machine size %zu", l);
    l = sizeof str;
    CHECK(sysctlbyname("hw.machine", str, &l, NULL, 0) == 0 && strcmp(str, "i386") == 0, "hw.machine");
    l = sizeof str;
    CHECK(sysctlbyname("machdep.cpu.brand_string", str, &l, NULL, 0) == 0 && strlen(str) > 3, "brand_string");
    l = sizeof str;
    CHECK(sysctlbyname("kern.osversion", str, &l, NULL, 0) == 0 && strlen(str) > 3, "kern.osversion");
    l = sizeof str;
    CHECK(sysctlbyname("kern.osrelease", str, &l, NULL, 0) == 0 && str[0] >= '1', "kern.osrelease");
    l = sizeof str;
    CHECK(sysctlbyname("hw.model", str, &l, NULL, 0) == 0 && strlen(str) > 2, "hw.model");
    struct timeval bt;
    l = 0;
    int bmib[2] = {CTL_KERN, KERN_BOOTTIME};
    CHECK(sysctl(bmib, 2, NULL, &l, NULL, 0) == 0 && l == 8, "boottime size %zu", l);
    l = sizeof bt;
    CHECK(sysctl(bmib, 2, &bt, &l, NULL, 0) == 0 && l == 8 && bt.tv_sec > 1000000000 && bt.tv_usec < 1000000, "boottime");
    // the classic debugger check
    struct kinfo_proc kp;
    int pmib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    l = sizeof kp;
    memset(&kp, 0, sizeof kp);
    CHECK(sysctl(pmib, 4, &kp, &l, NULL, 0) == 0 && l == sizeof kp && kp.kp_proc.p_pid == getpid() &&
              kp.kp_eproc.e_ppid == getppid() && !(kp.kp_proc.p_flag & P_TRACED),
          "kern.proc.pid len %zu pid %d", l, kp.kp_proc.p_pid);
    CHECK(sysctlbyname("no.such.thing", &v, &l, NULL, 0) == -1, "unknown sysctl");

    struct passwd *pw = getpwuid(getuid());
    CHECK(pw && pw->pw_uid == getuid() && pw->pw_name && pw->pw_name[0] && pw->pw_dir && pw->pw_dir[0] == '/', "getpwuid");
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 0, "getrlimit");
    struct rusage ru;
    CHECK(getrusage(RUSAGE_SELF, &ru) == 0 && ru.ru_maxrss > 0 && ru.ru_utime.tv_usec < 1000000, "getrusage");
    struct utsname u;
    CHECK(uname(&u) == 0 && strcmp(u.sysname, "Darwin") == 0 && strcmp(u.machine, "i386") == 0, "uname");
    CHECK(sysconf(_SC_PAGESIZE) == 4096 && sysconf(_SC_NPROCESSORS_ONLN) >= 1, "sysconf");

    CHECK(mach_task_self() != 0, "mach_task_self_");
    CHECK(mach_port_deallocate(mach_task_self(), 0x1234) == KERN_SUCCESS, "mach_port_deallocate");
    static char src[64] = "vm_read_overwrite source", dst[64];
    vm_size_t out = 0;
    CHECK(vm_read_overwrite(mach_task_self(), (vm_address_t)src, 25, (vm_address_t)dst, &out) == KERN_SUCCESS &&
              out == 25 && strcmp(dst, src) == 0,
          "vm_read_overwrite");
    CHECK(vm_read_overwrite(mach_task_self(), 0x10, 16, (vm_address_t)dst, &out) != KERN_SUCCESS, "vm_read_overwrite bad");
    CHECK(vm_deallocate(mach_task_self(), (vm_address_t)dst, 0) == KERN_SUCCESS, "vm_deallocate");
    DONE();
}
