#else
#include <climits>
#include <dlfcn.h>
#if !defined(__ANDROID__)
#include <execinfo.h>
#endif
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <unistd.h>

void platform::Initialise() {}

uint32_t platform::ThreadId()
{
#if defined(__APPLE__)
    return uint32_t(pthread_mach_thread_np(pthread_self()));
#else
    return uint32_t(gettid());
#endif
}

bool platform::IsMainThread()
{
#if defined(__APPLE__)
    return pthread_main_np() != 0;
#else
    return gettid() == getpid();
#endif
}

void platform::SetThreadName(const char* name)
{
    char shortName[16];
    std::snprintf(shortName, sizeof shortName, "%s", name);
#if defined(__APPLE__)
    pthread_setname_np(shortName);
#else
    pthread_setname_np(pthread_self(), shortName);
#endif
}

void platform::LowerThreadPriority()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10);
#endif
}

void platform::IdleThreadPriority()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#else
    sched_param none{};
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &none) != 0) LowerThreadPriority();
#endif
}

void platform::SetThreadAffinity(ThreadAffinityRole role)
{
#if defined(__APPLE__)
    switch (role)
    {
    case ThreadAffinityRole::PrimeCore:
    case ThreadAffinityRole::PerformanceCore:
    case ThreadAffinityRole::RenderCore:
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        break;
    case ThreadAffinityRole::WorkerCore:
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        break;
    default:
        break;
    }
#else
    (void)role;
#endif
}

void platform::PrintBacktrace(const char* why)
{
#if !defined(__ANDROID__)
    void* frames[64];
    const int n = backtrace(frames, 64);
    std::fprintf(stderr, "\n[E] ---- %s (%d frames) ----\n", why, n);
    std::fflush(stderr);
    backtrace_symbols_fd(frames, n, 2);
    std::fprintf(stderr, "[E] --------------------------------\n");
    std::fflush(stderr);
#else
    (void)why;
#endif
}

namespace
{
    struct ThreadStart { void (*body)(void*); void* argument; const char* name; };

    void* RunThread(void* raw)
    {
        ThreadStart start = *static_cast<ThreadStart*>(raw);
        delete static_cast<ThreadStart*>(raw);
        if (start.name) platform::SetThreadName(start.name);
        start.body(start.argument);
        return nullptr;
    }
}

bool platform::StartThread(void (*body)(void*), void* argument, size_t stackBytes,
                           const char* name)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (stackBytes)
    {
        const size_t page = size_t(sysconf(_SC_PAGESIZE));
        size_t wanted = (stackBytes + page - 1) & ~(page - 1);
#ifdef PTHREAD_STACK_MIN
        if (wanted < size_t(PTHREAD_STACK_MIN)) wanted = size_t(PTHREAD_STACK_MIN);
#endif
        pthread_attr_setstacksize(&attributes, wanted);
    }
    auto* start = new ThreadStart{ body, argument, name };
    pthread_t thread{};
    const int made = pthread_create(&thread, &attributes, &RunThread, start);
    pthread_attr_destroy(&attributes);
    if (made != 0)
    {
        delete start;
        return false;
    }
    return true;
}

void* platform::OpenLibrary(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void* platform::LoadedLibrary(const char* name) { return dlopen(name, RTLD_NOW | RTLD_NOLOAD); }
void* platform::Symbol(void* library, const char* name) { return dlsym(library, name); }

#endif
