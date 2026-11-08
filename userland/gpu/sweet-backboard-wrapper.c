/* Launch the signed 20H392 backboardd with the optional Sweet renderer.
 * The wrapper is installed only in the offline GPU test root. It leaves the
 * normal software path alone unless sweetcagpu=1 was passed at boot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define REAL_BACKBOARDD "/usr/libexec/real/backboardd"
#define RENDERER_LOADER "/sweet/lib/libSweetRendererLoader.dylib"

static int renderer_requested(void)
{
    char args[1024];
    size_t size = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || size >= sizeof(args))
        return 0;
    args[size] = '\0';
    char *save = NULL;
    for (char *arg = strtok_r(args, " ", &save); arg; arg = strtok_r(NULL, " ", &save)) {
        if (strcmp(arg, "sweetcagpu=1") == 0)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (renderer_requested() && access(RENDERER_LOADER, R_OK) == 0) {
        if (setenv("DYLD_INSERT_LIBRARIES", RENDERER_LOADER, 1) != 0)
            fputs("SWEET CA: cannot set renderer loader\n", stderr);
    }
    if (argc > 0)
        argv[0] = (char *)REAL_BACKBOARDD;
    execv(REAL_BACKBOARDD, argv);
    perror("SWEET CA: exec backboardd");
    return 127;
}
