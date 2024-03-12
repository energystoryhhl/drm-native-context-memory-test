#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <xf86drm.h>
#include <time.h>

// #include <xf86drm.h>

#include "virtgpu_drm.h"
#include "amdgpu_virtio_proto.h"
#include <libdrm/drm.h>
#include "amdgpu_drm.h"
// #include <linux/drm.h>

static char drm_device[] = "/dev/dri/renderD128";

#define AMDGPU_GEM_DOMAIN_SH_MEM 0x40

#define virtio_ioctl(fd, name, args) ({                 \
    int ret = drmIoctl((fd), DRM_IOCTL_##name, (args)); \
    ret;                                                \
})

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

typedef struct _drm_shmem_somke
{
    char *drm_device;
    int fd;

    unsigned long seqno; //TODO: shoule be atomic
    unsigned long blob_id; //TODO: shoule be atomic

    int test_times;
    size_t read_delay;
    int mem_type;
    size_t mem_size;
} drm_shmem_somke_t;

typedef struct _drm_bo
{
    void *addr;
    size_t size;
    __uint64_t blob_id;
    int fd;
    struct
    {
        uint32_t handle; /* virtio-gpu kms_handle */
        uint32_t res_id; /* virtio-gpu hw_res_handle */
        uint64_t offset;
        uint64_t alloc_size;
        int map_count;
    } real;
} drm_bo_t;

enum virgl_renderer_capset
{
    VIRGL_RENDERER_CAPSET_VIRGL = 1,
    VIRGL_RENDERER_CAPSET_VIRGL2 = 2,
    /* 3 is reserved for gfxstream */
    VIRGL_RENDERER_CAPSET_VENUS = 4,
    /* 5 is reserved for cross-domain */
    VIRGL_RENDERER_CAPSET_DRM = 6,
    VIRGL_RENDERER_CAPSET_VIRCL = 7,
};

static int set_context(int fd)
{
    struct drm_virtgpu_context_set_param params[] = {
        {VIRTGPU_CONTEXT_PARAM_CAPSET_ID, VIRGL_RENDERER_CAPSET_DRM},
        {VIRTGPU_CONTEXT_PARAM_NUM_RINGS, 64},
        {VIRTGPU_CONTEXT_PARAM_HOST_FENCE_WAIT, 1}};
    struct drm_virtgpu_context_init args = {
        .num_params = ARRAY_SIZE(params),
        .ctx_set_params = (uintptr_t)(params),
    };

    return virtio_ioctl(fd, VIRTGPU_CONTEXT_INIT, &args);
}

static int drm_shmem_somke_init(drm_shmem_somke_t *d)
{
    if (!d)
        return -1;

    char *drm_device_env = getenv("DRM_DEVICE_SMOKE");
    if (drm_device_env)
    {
        d->drm_device = drm_device_env;
        printf("Use DRM_DEVICE_SMOKE set drm device as: %s\n", d->drm_device);
    }
    else
    {
        d->drm_device = drm_device;
        printf("Use default drm device: %s\n", drm_device);
    }

    d->fd = open(d->drm_device, O_RDWR);
    if (d->fd == -1)
    {
        fprintf(stderr, "Can not open KFD device.\n");
        return -1;
    }

    if (set_context(d->fd))
    {
        fprintf(stderr, "Could not set context type: %s\n", strerror(errno));
        fprintf(stderr, "Things to check:\n");
        fprintf(stderr, "  - /sys/kernel/debug/dri/0/virtio-gpu-features\n");
        fprintf(stderr, "  - guest dmesg should have '+host_fence_wait'\n");

        return -1;
    }

    return 0;
}

static drm_bo_t *drm_shmem_somke_alloc_shmem(drm_shmem_somke_t *test, size_t size, int mem_type)
{
    int ret;
    uint32_t res_id;
    void *shmem_addr;

    if (mem_type != AMDGPU_GEM_DOMAIN_SH_MEM && mem_type !=AMDGPU_GEM_DOMAIN_GTT)
    {
        fprintf(stderr, "unsupport mem type: %d\n", mem_type);
        return NULL;
    }

    struct drm_virtgpu_resource_create_blob args = {
        .blob_mem = VIRTGPU_BLOB_MEM_HOST3D,
        .size = size,
    };
    args.blob_id = mem_type == AMDGPU_GEM_DOMAIN_SH_MEM ? 0 : ++test->blob_id; // shmem blob_id must be 0
    args.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;

    struct amdgpu_ccmd_gem_new_req req = {
        .hdr = AMDGPU_CCMD(GEM_NEW, sizeof(req)),
    };
    req.r.alloc_size = size;
    req.r.phys_alignment = 0;
    req.r.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
    req.r.flags = AMDGPU_GEM_PIN_ON_MMAP;
    req.vm_map_size = size;
    req.blob_id = args.blob_id;

    if (mem_type != AMDGPU_GEM_DOMAIN_SH_MEM)
    {
        args.cmd = (uintptr_t)&req;
        args.cmd_size = sizeof(req);
        req.hdr.seqno = ++test->seqno;
    }

    drm_bo_t *bo = calloc(1, sizeof(drm_bo_t));
    bo->size = size;
    ret = virtio_ioctl(test->fd, VIRTGPU_RESOURCE_CREATE_BLOB, &args);

    // No fence sync in this test

    if (ret)
    {
        fprintf(stderr, "VIRTGPU_RESOURCE_CREATE_BLOB failed (%s)\n", strerror(errno));
        fprintf(stderr, "\targs.blob_mem:   %u\n", args.blob_mem);
        fprintf(stderr, "\targs.blob_flags: %u\n", args.blob_flags);
        fprintf(stderr, "\targs.bo_handle:  %u\n", args.bo_handle);
        fprintf(stderr, "\targs.res_handle: %u\n", args.res_handle);
        fprintf(stderr, "\targs.size:     %llu\n", args.size);
        fprintf(stderr, "\targs.cmd_size:   %u\n", args.cmd_size);
        fprintf(stderr, "\targs.blob_id:  %llu\n", args.blob_id);
        return NULL;
    }

    struct drm_virtgpu_resource_info args2 = {
        .bo_handle = args.bo_handle,
    };

    ret = virtio_ioctl(test->fd, VIRTGPU_RESOURCE_INFO, &args2);
    res_id = args2.res_handle;

    bo->real.alloc_size = size;
    bo->real.handle = args.bo_handle;
    bo->real.res_id = res_id;

    struct drm_virtgpu_map req2 = {
        .handle = bo->real.handle,
    };
    ret = virtio_ioctl(test->fd, VIRTGPU_MAP, &req2);
    if (ret)
    {
        fprintf(stderr, "Shmem get offset failed\n");
        return NULL;
    }
    bo->real.offset = req2.offset;

    shmem_addr = mmap(0, bo->real.alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      test->fd, bo->real.offset);

    if (shmem_addr == MAP_FAILED)
    {
        fprintf(stderr, "Shmem map failed\n");
        return NULL;
    }

    bo->addr = shmem_addr;
    bo->fd = test->fd;

    return bo;
}

static void show_bo_param(drm_bo_t *bo)
{
    fprintf(stderr, "shmem fd: %d\n", bo->fd);
    fprintf(stderr, "\taddr:   %p\n", bo->addr);
    fprintf(stderr, "\tsize: 0x%lx\n", bo->size);
    fprintf(stderr, "\tbo handle: %d\n", bo->real.handle);
    fprintf(stderr, "\tres id:  %d\n", bo->real.res_id);
}

void write_random_numbers(void *buf, size_t size)
{
    if (!buf)
        return;

    size_t i = 0;
    static int rand_add = 1;
    srand(time(NULL) + rand_add++);

    int *buf2 = (int *)buf;

    while (i < (size / sizeof(int)))
    {
        buf2[i] = rand();
        i++;
    }
}

void dump_memory(void *buf, size_t size)
{
    if (!buf)
        return;

    int i = 0;
    int step = sizeof(short);

    while (i < size)
    {
        if (i % 8 == 0)
        {
            if (i != 0)
                printf("\n");
            printf("%p: ", buf + i);
        }
        printf("%02X ", ((unsigned char *)buf)[i]);

        i++;
    }
    printf("\n");
}

int find_memory_dismatch(void *m1, void *m2)
{
    bool match = true;
    int i = 0;

    while (match)
    {
        if (memcmp(m1 + i, m2 + i, sizeof(int)))
        {
            return i;
        }
        i += sizeof(int);
    }

    return 0;
}

static int drm_shmem_somke_random_write_read_test(drm_bo_t *bo, int times, size_t read_delay, size_t size)
{
    void *rand_buffer = calloc(1, size);
    int i = 0;
    bool run = 1;

    while (run)
    {
        write_random_numbers(rand_buffer, size);

        // write to shmem
        memcpy(bo->addr, rand_buffer, size);

        sleep(read_delay);

        // compare
        if (memcmp(rand_buffer, bo->addr, size))
        {
            fprintf(stderr, "SHMEM ERROR OCCURED, TIME: %d", i);
            int loc = find_memory_dismatch(rand_buffer, bo->addr);
            show_bo_param(bo);
            printf("rand buffer: \n");
            dump_memory(rand_buffer + loc, 0x10);
            printf("shmem buffer: \n");
            dump_memory(bo->addr + loc, 0x10);

            getchar();
            return -1;
        }
        else
        {
            printf("shmem test times: %d, pass.\n", i + 1);
            // dump_memory(bo->addr, 0x100);
        }

        i++;
        if (i >= times && times != -1)
            run = 0;

        // if (i % 1000 == 0)
        //     printf("runnning test, times: %d\n", i);
    }

    printf("shmem test total times: %d, pass.\n", i);
    show_bo_param(bo);
    free(rand_buffer);
}

extern char *optarg;
static int get_opt(int argc, char *argv[], int *times, size_t *read_delay, int *mem_type, size_t *size)
{
    char *optstring = "t:d:m:hs:";
    int opt;

    while ((opt = getopt(argc, argv, optstring)) != -1)
    {
        switch (opt)
        {
        case 't':
            *times = atoi(optarg);
            printf("test times: %d\n", *times);
            break;

        case 'd':
            *read_delay = atoi(optarg);
            printf("read delay: %ld\n", *read_delay);
            break;

        case 'm':
            *mem_type = atoi(optarg);
            printf("mem type: %d\n", *mem_type);
            break;

        case 's':
            *size = atoi(optarg);
            printf("mem size: %ld\n", *size);
            break;

        case 'h':
            printf("-t  the run times for test\n-d  read delay after write memory\n-m  memory type, GTT: 2 shmem: 64\n-s  memory size, will be aligned up by 0x1000\n");
            exit(0);
            break;

        default:
            break;
        }
    }
}

static inline uint64_t
align64(uint64_t value, unsigned alignment)
{
   return (value + alignment - 1) & ~((uint64_t)alignment - 1);
}

int main(int argc, char *argv[])
{
    int rc = 0;
    drm_bo_t *bo;

    printf("Test program for blob shmem smoke\n");

    drm_shmem_somke_t *drm_shmem_test = calloc(1, sizeof(drm_shmem_somke_t));
    if (!drm_shmem_test)
    {
        fprintf(stderr, "malloc drm shmem test failed\n");
        return -1;
    }

    drm_shmem_test->mem_type = AMDGPU_GEM_DOMAIN_SH_MEM; // default shmem
    drm_shmem_test->mem_size = 0x4000;

    get_opt(argc, argv, &drm_shmem_test->test_times,
            &drm_shmem_test->read_delay,
            &drm_shmem_test->mem_type,
            &drm_shmem_test->mem_size);
    
    drm_shmem_test->mem_size = align64(drm_shmem_test->mem_size, 0x1000);

    printf("mem size aligned: 0x%lx\n", drm_shmem_test->mem_size);

    rc = drm_shmem_somke_init(drm_shmem_test);
    if (rc)
    {
        fprintf(stderr, "init drm shmem failed\n");
        return -1;
    }

    bo = drm_shmem_somke_alloc_shmem(drm_shmem_test, drm_shmem_test->mem_size, drm_shmem_test->mem_type);
    if (!bo)
    {
        fprintf(stderr, "alloc drm shmem failed\n");
        return -1;
    }

    // show_bo_param(bo);

    drm_shmem_somke_random_write_read_test(bo, drm_shmem_test->test_times, drm_shmem_test->read_delay, bo->size);

    free(drm_shmem_test);
    free(bo);
    // TODO: free shmem memory, but when program exit, kernel will free the shmem automatically
    return 0;
}