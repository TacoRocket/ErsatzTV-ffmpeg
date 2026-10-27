/* Read-only capability query; no surfaces are submitted or rendered. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_vpp.h>

static void check(VAStatus result, const char *operation)
{
    if (result != VA_STATUS_SUCCESS) {
        fprintf(stderr, "%s: %s\n", operation, vaErrorStr(result));
        exit(1);
    }
}

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) { perror("open render node"); return 1; }
    VADisplay display = vaGetDisplayDRM(fd);
    if (!display) { fprintf(stderr, "No VA display\n"); close(fd); return 1; }
    int major, minor;
    VAConfigID config;
    VAContextID context;
    check(vaInitialize(display, &major, &minor), "initialize");
    printf("VA API %d.%d; %s\n", major, minor, vaQueryVendorString(display));
    check(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc,
                         NULL, 0, &config), "create config");
    check(vaCreateContext(display, config, 1920, 1080, VA_PROGRESSIVE,
                          NULL, 0, &context), "create context");
    VAProcFilterType filters[VAProcFilterCount];
    unsigned int count = VAProcFilterCount;
    check(vaQueryVideoProcFilters(display, context, filters, &count), "query filters");
    int lut_supported = 0;
    for (unsigned int i = 0; i < count; i++) {
        printf("filter=%d%s\n", filters[i], filters[i] == VAProcFilter3DLUT ? " (3DLUT)" : "");
        lut_supported |= filters[i] == VAProcFilter3DLUT;
    }
    if (lut_supported) {
        VAProcFilterCap3DLUT caps[32] = {0};
        unsigned int n = 32;
        check(vaQueryVideoProcFilterCaps(display, context, VAProcFilter3DLUT,
                                         caps, &n), "query 3DLUT caps");
        for (unsigned int i = 0; i < n; i++)
            printf("3DLUT size=%u stride=%u,%u,%u depth=%u channels=%u mapping=%u\n",
                caps[i].lut_size, caps[i].lut_stride[0], caps[i].lut_stride[1],
                caps[i].lut_stride[2], caps[i].bit_depth, caps[i].num_channel,
                caps[i].channel_mapping);
    } else {
        puts("3DLUT not advertised by this driver/device");
    }
    VAProcFilterCapColorBalance colors[VAProcColorBalanceCount] = {0};
    unsigned int n = VAProcColorBalanceCount;
    check(vaQueryVideoProcFilterCaps(display, context, VAProcFilterColorBalance,
                                     colors, &n), "query color balance caps");
    for (unsigned int i = 0; i < n; i++)
        printf("color balance=%d min=%g max=%g default=%g step=%g\n", colors[i].type,
               colors[i].range.min_value, colors[i].range.max_value,
               colors[i].range.default_value, colors[i].range.step);
    check(vaDestroyContext(display, context), "destroy context");
    check(vaDestroyConfig(display, config), "destroy config");
    check(vaTerminate(display), "terminate");
    close(fd);
    return 0;
}
