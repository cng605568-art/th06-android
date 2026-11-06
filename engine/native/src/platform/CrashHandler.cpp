#include "platform/CrashHandler.hpp"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

#if defined(__unix__) || defined(__ANDROID__)
#include <dlfcn.h>
#include <fcntl.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
#define TH06_CRASH_HANDLER 1
#endif

#include "core/Log.hpp"

namespace touhou {

#if defined(TH06_CRASH_HANDLER)
namespace {

constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
constexpr int kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
struct sigaction g_previous[kSignalCount];
char g_path[1024];
constexpr std::size_t kAltStackSize = 64 * 1024;
alignas(16) char g_altStack[kAltStackSize];

// Only async-signal-safe output below: write(2) on a stack buffer.
void put(int fd, const char* s) {
    if (s) (void)!::write(fd, s, std::strlen(s));
}
void putHex(int fd, std::uintptr_t v) {
    char buf[2 + 2 * sizeof(v) + 1];
    char* p = buf + sizeof(buf) - 1;
    *p = '\0';
    do {
        *--p = "0123456789abcdef"[v & 15u];
        v >>= 4;
    } while (v != 0);
    *--p = 'x';
    *--p = '0';
    put(fd, p);
}
void putDec(int fd, long v) {
    char buf[24];
    char* p = buf + sizeof(buf) - 1;
    *p = '\0';
    const bool negative = v < 0;
    unsigned long u = negative ? 0ul - static_cast<unsigned long>(v) : static_cast<unsigned long>(v);
    do {
        *--p = static_cast<char>('0' + u % 10u);
        u /= 10u;
    } while (u != 0);
    if (negative) *--p = '-';
    put(fd, p);
}

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (bad memory access)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGBUS: return "SIGBUS (bus error)";
        case SIGFPE: return "SIGFPE (arithmetic error)";
        case SIGILL: return "SIGILL (illegal instruction)";
        default: return "?";
    }
}

std::uintptr_t contextPc(void* context) {
    if (!context) return 0;
    const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__aarch64__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
#elif defined(__arm__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.arm_pc);
#elif defined(__x86_64__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__i386__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_EIP]);
#else
    (void)uc;
    return 0;
#endif
}

struct Trace {
    std::uintptr_t pcs[48];
    int count = 0;
};

_Unwind_Reason_Code collect(_Unwind_Context* context, void* arg) {
    auto* trace = static_cast<Trace*>(arg);
    const std::uintptr_t pc = _Unwind_GetIP(context);
    if (pc != 0) {
        if (trace->count == 48) return _URC_END_OF_STACK;
        trace->pcs[trace->count++] = pc;
    }
    return _URC_NO_REASON;
}

void putFrame(int fd, int index, std::uintptr_t pc) {
    put(fd, "  #");
    if (index < 10) put(fd, "0");
    putDec(fd, index);
    put(fd, " pc ");
    putHex(fd, pc);
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(pc), &info) != 0 && info.dli_fname) {
        const char* base = std::strrchr(info.dli_fname, '/');
        put(fd, "  ");
        put(fd, base ? base + 1 : info.dli_fname);
        put(fd, "+");
        putHex(fd, pc - reinterpret_cast<std::uintptr_t>(info.dli_fbase));
        if (info.dli_sname) {
            put(fd, "  (");
            put(fd, info.dli_sname);
            put(fd, "+");
            putHex(fd, pc - reinterpret_cast<std::uintptr_t>(info.dli_saddr));
            put(fd, ")");
        }
    }
    put(fd, "\n");
}

void onFatalSignal(int sig, siginfo_t* info, void* context) {
    const int fd = ::open(g_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        put(fd, "th06 native crash\nsignal ");
        putDec(fd, sig);
        put(fd, " ");
        put(fd, signalName(sig));
        if (info) {
            put(fd, ", code ");
            putDec(fd, info->si_code);
            put(fd, ", fault address ");
            putHex(fd, reinterpret_cast<std::uintptr_t>(info->si_addr));
        }
        put(fd, "\n");
        if (const std::uintptr_t pc = contextPc(context)) {
            put(fd, "crashed at:\n");
            putFrame(fd, 0, pc);
        }
        Trace trace;
        _Unwind_Backtrace(collect, &trace);
        put(fd, "backtrace (from the signal handler):\n");
        for (int i = 0; i < trace.count; ++i) putFrame(fd, i, trace.pcs[i]);
        ::close(fd);
    }
    // Hand over: restore the previous action. A fault re-runs its instruction and an abort
    // raises again, both now reaching the system's handler, which ends the process as before.
    for (int i = 0; i < kSignalCount; ++i) {
        if (kSignals[i] == sig) sigaction(sig, &g_previous[i], nullptr);
    }
    if (info && info->si_code <= 0) raise(sig);  // sent by kill/raise: deliver it again
}

void onTerminate() {
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            log::error("Uncaught exception ({}): {}", typeid(ex).name(), ex.what());
        } catch (...) {
            log::error("Uncaught exception of an unknown type");
        }
    } else {
        log::error("std::terminate called");
    }
    std::abort();
}

}  // namespace

void installCrashHandler(const std::string& path) {
    if (path.empty() || path.size() >= sizeof(g_path)) return;
    std::memcpy(g_path, path.c_str(), path.size() + 1);
    // An alternate stack, so a stack overflow on this (the game's) thread is still reported.
    stack_t ss{};
    ss.ss_sp = g_altStack;
    ss.ss_size = kAltStackSize;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = onFatalSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < kSignalCount; ++i) sigaction(kSignals[i], &sa, &g_previous[i]);
    std::set_terminate(onTerminate);
}

#else

void installCrashHandler(const std::string&) {}

#endif

}  // namespace touhou
