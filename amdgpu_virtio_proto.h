#ifndef AMDGPU_VIRTIO_PROTO_H
#define AMDGPU_VIRTIO_PROTO_H

#include <stdint.h>
#include <libdrm/amdgpu.h>
#include "amdgpu_drm.h"

#define DEFINE_CAST(parent, child)                                             \
   static inline struct child *to_##child(const struct parent *x)              \
   {                                                                           \
      return (struct child *)x;                                                \
   }

enum amdgpu_ccmd {
   AMDGPU_CCMD_NOP = 1,         /* No payload, can be used to sync with host */
   AMDGPU_CCMD_QUERY_INFO,
   AMDGPU_CCMD_GEM_NEW,
   AMDGPU_CCMD_ASSIGN_VA,
   AMDGPU_CCMD_CS_SUBMIT,
   AMDGPU_CCMD_SET_METADATA,
   AMDGPU_CCMD_BO_QUERY_INFO,
   AMDGPU_CCMD_CREATE_CTX,
};

struct amdgpu_ccmd_req {
   uint32_t cmd;
   uint32_t len;
   uint32_t seqno;

   /* Offset into shmem ctrl buffer to write response.  The host ensures
    * that it doesn't write outside the bounds of the ctrl buffer, but
    * otherwise it is up to the guest to manage allocation of where responses
    * should be written in the ctrl buf.
    */
   uint32_t rsp_off;
};

struct amdgpu_ccmd_rsp {
   int32_t ret;
   uint32_t len;
};


/**
 * Defines the layout of shmem buffer used for host->guest communication.
 */
struct amdvgpu_shmem {
   /**
    * The sequence # of last cmd processed by the host
    */
   uint32_t seqno;

   /**
    * Offset to the start of rsp memory region in the shmem buffer.  This
    * is set by the host when the shmem buffer is allocated, to allow for
    * extending the shmem buffer with new fields.  The size of the rsp
    * memory region is the size of the shmem buffer (controlled by the
    * guest) minus rsp_mem_offset.
    *
    * The guest should use the msm_shmem_has_field() macro to determine
    * if the host supports a given field, ie. to handle compatibility of
    * newer guest vs older host.
    *
    * Making the guest userspace responsible for backwards compatibility
    * simplifies the host VMM.
    */
   uint32_t rsp_mem_offset;

#define amdgpu_shmem_has_field(shmem, field) ({                         \
      struct amdgpu_shmem *_shmem = (shmem);                            \
      (_shmem->rsp_mem_offset > offsetof(struct amdgpu_shmem, field));  \
   })

   /**
    * Counter that is incremented on asynchronous errors, like SUBMIT
    * or GEM_NEW failures.  The guest should treat errors as context-
    * lost.
    */
   uint32_t async_error;

   union {
      struct amdgpu_heap_info heap_info[3];
      struct {
         struct amdgpu_heap_info gtt;
         struct amdgpu_heap_info vram;
         struct amdgpu_heap_info vis_vram;
      };
   };
};


#define AMDGPU_CCMD(_cmd, _len) (struct amdgpu_ccmd_req){ \
       .cmd = AMDGPU_CCMD_##_cmd,                         \
       .len = (_len),                                     \
   }

struct amdgpu_ccmd_nop_req {
   struct amdgpu_ccmd_req hdr;
};

/*
 * AMDGPU_CCMD_QUERY_INFO
 * TODO: rename generic_ioctl
 *
 */
struct amdgpu_ccmd_query_info_req {
   struct amdgpu_ccmd_req hdr;
   union {
      struct drm_amdgpu_info info;
      int reserve_vmid; /* 1: reserve, 0: unreserve */
      struct {
         uint32_t ctx_id;
         uint32_t op;
         uint32_t flags;
      } pstate;
   };

   uint32_t type; /* possible values:
              *   0: DRM_AMDGPU_INFO
              *   1: amdgpu_sw_info_address32_hi
              *   2: amdgpu_query_gpu_info
              *   3: amdgpu_query_buffer_size_alignment
              *   4: marketing_name
              *   5: reserve_vmid
              *   6: set_pstate
              */
};
DEFINE_CAST(amdgpu_ccmd_req, amdgpu_ccmd_query_info_req)

struct amdgpu_ccmd_query_info_rsp {
   struct amdgpu_ccmd_rsp hdr;
   uint8_t payload[];
};

struct amdgpu_ccmd_gem_new_req {
   struct amdgpu_ccmd_req hdr;

   uint64_t blob_id;
   uint64_t va;
   uint32_t pad;
   uint32_t vm_flags;
   uint64_t vm_map_size; /* may be smaller than alloc_size */

   /* This is amdgpu_bo_alloc_request but padded correctly. */
   struct {
      uint64_t alloc_size;
      uint64_t phys_alignment;
      uint32_t preferred_heap;
      uint32_t __pad;
      uint64_t flags;
   } r;
};
DEFINE_CAST(amdgpu_ccmd_req, amdgpu_ccmd_gem_new_req)


/*
 * AMDGPU_CCMD_ASSIGN_VA
 *
 */
struct amdgpu_ccmd_assign_va_req {
   struct amdgpu_ccmd_req hdr;
   uint64_t va;
   uint64_t vm_map_size;
   uint64_t offset;
   uint32_t res_id;
   uint32_t op;
   bool is_sparse_bo;
};
DEFINE_CAST(amdgpu_ccmd_req, amdgpu_ccmd_assign_va_req)

/*
 * AMDGPU_CCMD_CS_SUBMIT
 */
struct amdgpu_ccmd_cs_submit_req {
   struct amdgpu_ccmd_req hdr;

   uint32_t ctx_id;
   uint32_t num_chunks;
   uint32_t bo_number;
   uint32_t ring_idx;

   /* Starts with a descriptor array:
    *     (chunk_id, offset_in_payload), ...
    */
   uint8_t payload[];
};
DEFINE_CAST(amdgpu_ccmd_req, amdgpu_ccmd_cs_submit_req)

/*
 * AMDGPU_CCMD_SET_METADATA
 */
struct amdgpu_ccmd_set_metadata_req {
   struct amdgpu_ccmd_req hdr;
   uint64_t flags;
   uint64_t tiling_info;
   uint32_t res_id;
   uint32_t size_metadata;
   uint32_t umd_metadata[];
};
DEFINE_CAST(amdgpu_ccmd_req, amdgpu_ccmd_set_metadata_req)


/*
 * AMDGPU_CCMD_BO_QUERY_INFO
 */
struct amdgpu_ccmd_bo_query_info_req {
   struct amdgpu_ccmd_req hdr;
   uint32_t res_id;
};
DEFINE_CAST(amdgpu_ccmd_req, amdgpu_ccmd_bo_query_info_req)

struct amdgpu_ccmd_bo_query_info_rsp {
   struct amdgpu_ccmd_rsp hdr;
   uint32_t __pad;
   /* This is almost struct amdgpu_bo_info, but padded to get
    * the same struct on 32 bit and 64 bit builds.
    */
   struct {
      uint64_t                   alloc_size;           /*     0     8 */
      uint64_t                   phys_alignment;       /*     8     8 */
      uint32_t                   preferred_heap;       /*    16     4 */
      uint32_t __pad;
      uint64_t                   alloc_flags;          /*    20     8 */
      struct amdgpu_bo_metadata  metadata;
   } info;
};

/*
 * AMDGPU_CCMD_CREATE_CTX
 */
struct amdgpu_ccmd_create_ctx_req {
   struct amdgpu_ccmd_req hdr;
   union {
      int32_t priority; /* create */
      uint32_t id;      /* destroy */
   };
   bool create;
};
DEFINE_CAST(amdgpu_ccmd_req, amdgpu_ccmd_create_ctx_req)

struct amdgpu_ccmd_create_ctx_rsp {
   struct amdgpu_ccmd_rsp hdr;
   uint32_t ctx_id;
};

#endif
