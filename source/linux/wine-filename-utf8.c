#define _GNU_SOURCE
#include <dlfcn.h>
#include <locale.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Atomic pid_t reported_pid;

/* dwproton-11.0-16 ntdll initializes Unix filenames from
 * setlocale(LC_CTYPE, "") followed by nl_langinfo(CODESET). The Steam C
 * locale override must not take UTF-8 away from Wine's filename lookup.
 * Do not alter LC_ALL, explicit locale names, queries, or other libraries. */
char *setlocale(int category, const char *locale)
{
    char *(*real_setlocale)(int, const char *) = dlsym(RTLD_NEXT, "setlocale");
    if (category == LC_CTYPE && locale && !*locale) {
        const char *all = getenv("LC_ALL");
        Dl_info caller;
        if (all && (!strcmp(all, "C") || !strcmp(all, "POSIX")) &&
            dladdr(__builtin_return_address(0), &caller) && caller.dli_fname) {
            const char *name = strrchr(caller.dli_fname, '/');
            if (!strcmp(name ? name + 1 : caller.dli_fname, "ntdll.so")) {
                const char *original = all[0] == 'C' ? "C" : "POSIX";
                char *selected = real_setlocale(category, "C.UTF-8");
                if (selected) {
                    pid_t pid = getpid();
                    if (atomic_exchange_explicit(&reported_pid, pid, memory_order_relaxed) != pid)
                        dprintf(STDERR_FILENO, "WWMI-WINE-UTF8 pid=%ld original=%s LC_CTYPE=%s\n",
                                (long)pid, original, selected);
                }
                return selected;
            }
        }
    }
    return real_setlocale(category, locale);
}
