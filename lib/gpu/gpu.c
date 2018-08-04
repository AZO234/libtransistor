#include<libtransistor/err.h>
#include<libtransistor/internal_util.h>
#include<libtransistor/types.h>
#include<libtransistor/ipc/nv.h>
#include<libtransistor/gpu/gpu.h>
#include<libtransistor/gpu/nv_ioc.h>

static int nvas_fd;
static int nvmap_fd;
static int nvhost_ctrl_fd;
static int gpu_initializations = 0;

result_t gpu_initialize() {
	if(gpu_initializations++ > 0) {
		return RESULT_OK;
	}
	
	result_t r;
	if((r = nv_init()) != RESULT_OK) {
		goto fail;
	}

	if((nvas_fd = nv_open("/dev/nvhost-as-gpu")) < 0) {
		r = nv_result;
		goto fail_nv;
	}

	if((nvmap_fd = nv_open("/dev/nvmap")) < 0) {
		r = nv_result;
		goto fail_nvas;
	}

	if((nvhost_ctrl_fd = nv_open("/dev/nvhost-ctrl")) < 0) {
		r = nv_result;
		goto fail_nvmap;
	}

	return RESULT_OK;

fail_nvmap:
	nv_close(nvmap_fd);
fail_nvas:
	nv_close(nvas_fd);
fail_nv:
	nv_finalize();
fail:
	gpu_initializations--;
	return r;
}

result_t gpu_buffer_initialize(gpu_buffer_t *gpu_b, void *addr, size_t size, uint32_t heapmask, uint32_t flags, uint32_t alignment, uint8_t kind) {
	INITIALIZATION_GUARD(gpu);
	
	if(((uint64_t) addr & ((uint64_t) alignment)-1) != 0) {
		// I wish it weren't true, but this legitimately crashes the graphics driver instead of throwing an error
		return LIBTRANSISTOR_ERR_GPU_BUFFER_UNALIGNED;
	}
  
	gpu_b->size = size;
	gpu_b->alignment = alignment;
	gpu_b->kind = kind;

	nvmap_ioc_create_args nvm_create;
	nvm_create.size = size;
	if(nv_ioctl(nvmap_fd, NVMAP_IOC_CREATE, &nvm_create, sizeof(nvm_create)) != 0) {
		return nv_result;
	}

	gpu_b->nvmap_handle = nvm_create.handle;
  
	nvmap_ioc_alloc_args nvm_alloc;
	nvm_alloc.handle = nvm_create.handle;
	nvm_alloc.heapmask = heapmask;
	nvm_alloc.flags = flags;
	nvm_alloc.align = alignment;
	nvm_alloc.kind = kind;
	nvm_alloc.addr = (uint64_t) addr;
	if(nv_ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &nvm_alloc, sizeof(nvm_alloc)) != 0) {
		return nv_result;
	}
  
	return RESULT_OK;
}

result_t gpu_buffer_destroy(gpu_buffer_t *gpu_b, uint64_t *refcount, uint32_t *flags) {
	INITIALIZATION_GUARD(gpu);
	
	nvmap_ioc_free_args nvm_free;
	nvm_free.handle = gpu_b->nvmap_handle;
	if(nv_ioctl(nvmap_fd, NVMAP_IOC_FREE, &nvm_free, sizeof(nvm_free)) != 0) {
		return nv_result;
	}

	if(refcount) {
		*refcount = nvm_free.refcount;
	}
	if(flags) {
		*flags = nvm_free.flags;
	}

	return RESULT_OK;
}

result_t gpu_buffer_get_id(gpu_buffer_t *gpu_b, uint32_t *id) {
	INITIALIZATION_GUARD(gpu);
	
	nvmap_ioc_get_id_args nvm_get_id;
	nvm_get_id.handle = gpu_b->nvmap_handle;

	if(nv_ioctl(nvmap_fd, NVMAP_IOC_GET_ID, &nvm_get_id, sizeof(nvm_get_id)) != 0) {
		return nv_result;
	}

	*id = nvm_get_id.id;
  
	return RESULT_OK;
}

result_t gpu_buffer_initialize_from_id(gpu_buffer_t *gpu_b, uint32_t id) {
	INITIALIZATION_GUARD(gpu);
	
	nvmap_ioc_from_id_args nvm_from_id;
	nvm_from_id.id = id;

	if(nv_ioctl(nvmap_fd, NVMAP_IOC_FROM_ID, &nvm_from_id, sizeof(nvm_from_id)) != 0) {
		return nv_result;
	}

	gpu_b->nvmap_handle = nvm_from_id.handle;

	nvmap_ioc_param_args nvm_param;
	nvm_param.handle = gpu_b->nvmap_handle;
  
	nvm_param.param = 1; // SIZE
	if(nv_ioctl(nvmap_fd, NVMAP_IOC_PARAM, &nvm_param, sizeof(nvm_param)) != 0) { return nv_result; }
	gpu_b->size = nvm_param.value;

	nvm_param.param = 2; // ALIGNMENT
	if(nv_ioctl(nvmap_fd, NVMAP_IOC_PARAM, &nvm_param, sizeof(nvm_param)) != 0) { return nv_result; }
	gpu_b->alignment = nvm_param.value;

	nvm_param.param = 5; // KIND
	if(nv_ioctl(nvmap_fd, NVMAP_IOC_PARAM, &nvm_param, sizeof(nvm_param)) != 0) { return nv_result; }
	gpu_b->kind = nvm_param.value;

	return RESULT_OK;
}

result_t gpu_wait_fence(gpu_fence_t *fence, uint32_t timeout) {
	INITIALIZATION_GUARD(gpu);

	nvhost_ioc_ctrl_syncpt_wait_args nvh_wait;
	nvh_wait.syncpt_id = fence->syncpt_id;
	nvh_wait.threshold = fence->syncpt_value;
	nvh_wait.timeout = timeout;

	if(nv_ioctl(nvhost_ctrl_fd, NVHOST_IOC_CTRL_SYNCPT_WAIT, &nvh_wait, sizeof(nvh_wait)) != 0) {
		return nv_result;
	}

	return RESULT_OK;
}

static void gpu_force_finalize() {
	nv_close(nvhost_ctrl_fd);
	nv_close(nvmap_fd);
	nv_close(nvas_fd);
	nv_finalize();
	gpu_initializations = 0;
}

void gpu_finalize() {
	if(--gpu_initializations == 0) {
		gpu_force_finalize();
	}
}

static __attribute__((destructor)) void gpu_destruct() {
	if(gpu_initializations > 0) {
		gpu_force_finalize();
	}
}
