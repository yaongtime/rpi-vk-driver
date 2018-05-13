#define _GNU_SOURCE
#include "kernelInterface.h"

int controlFd = -1;
int renderFd = -1;

int openIoctl()
{
	controlFd = open(DRM_IOCTL_CTRL_DEV_FILE_NAME, O_RDWR | O_CLOEXEC);
	if (controlFd < 0) {
		printf("Can't open device file: %s\n", DRM_IOCTL_CTRL_DEV_FILE_NAME);
		return -1;
	}

	renderFd = open(DRM_IOCTL_RENDER_DEV_FILE_NAME, O_RDWR | O_CLOEXEC);
	if (renderFd < 0) {
		printf("Can't open device file: %s\n", DRM_IOCTL_RENDER_DEV_FILE_NAME);
		return -1;
	}

	return 0;
}

void closeIoctl()
{
	close(controlFd);
	close(renderFd);
}

static uint32_t align(uint32_t num, uint32_t alignment)
{
	uint32_t mod = num%alignment;
	if(!mod)
	{
		return num;
	}
	else
	{
		return num + alignment - mod;
	}
}

int vc4_get_chip_info(int fd)
{
	assert(fd);

	struct drm_vc4_get_param ident0 = {
		.param = DRM_VC4_PARAM_V3D_IDENT0,
	};
	struct drm_vc4_get_param ident1 = {
		.param = DRM_VC4_PARAM_V3D_IDENT1,
	};
	int ret;

	ret = drmIoctl(fd, DRM_IOCTL_VC4_GET_PARAM, &ident0);
	if (ret != 0) {
		if (errno == EINVAL) {
			/* Backwards compatibility with 2835 kernels which
						 * only do V3D 2.1.
						 */
			return 21;
		} else {
			printf("Couldn't get V3D IDENT0: %s\n",
				   strerror(errno));
			return 0;
		}
	}
	ret = drmIoctl(fd, DRM_IOCTL_VC4_GET_PARAM, &ident1);
	if (ret != 0) {
		printf("Couldn't get V3D IDENT1: %s\n",
			   strerror(errno));
		return 0;
	}

	uint32_t major = (ident0.value >> 24) & 0xff;
	uint32_t minor = (ident1.value >> 0) & 0xf;
	uint32_t v3d_ver = major * 10 + minor;

	if (v3d_ver != 21 && v3d_ver != 26) {
		printf("V3D %d.%d not supported.\n",
			   v3d_ver / 10,
			   v3d_ver % 10);
		return 0;
	}

	return v3d_ver;
}

int vc4_has_feature(int fd, uint32_t feature)
{
	assert(fd);

	struct drm_vc4_get_param p = {
		.param = feature,
	};
	int ret = drmIoctl(fd, DRM_IOCTL_VC4_GET_PARAM, &p);

	if (ret != 0)
		return 0;

	return p.value;
}

int vc4_test_tiling(int fd)
{
	assert(fd);

	/* Test if the kernel has GET_TILING; it will return -EINVAL if the
	 * ioctl does not exist, but -ENOENT if we pass an impossible handle.
	 * 0 cannot be a valid GEM object, so use that.
	 */
	struct drm_vc4_get_tiling get_tiling = {
		.handle = 0x0,
	};
	int ret = drmIoctl(fd, DRM_IOCTL_VC4_GET_TILING, &get_tiling);
	if (ret == -1 && errno == ENOENT)
	{
		return 1;
	}

	return 0;
}

uint64_t vc4_bo_get_tiling(int fd, uint32_t bo, uint64_t mod)
{
	assert(fd);
	assert(bo);

	struct drm_vc4_get_tiling get_tiling = {
		.handle = bo,
	};
	int ret = drmIoctl(fd, DRM_IOCTL_VC4_GET_TILING, &get_tiling);

	if (ret != 0) {
		return DRM_FORMAT_MOD_LINEAR;
	} else if (mod == DRM_FORMAT_MOD_INVALID) {
		return get_tiling.modifier;
	} else if (mod != get_tiling.modifier) {
		printf("Modifier 0x%llx vs. tiling (0x%llx) mismatch\n",
			   (long long)mod, get_tiling.modifier);
		return 0;
	}

	return 0;
}

int vc4_bo_set_tiling(int fd, uint32_t bo, uint64_t mod)
{
	assert(fd);
	assert(bo);
	assert(mod);

	struct drm_vc4_set_tiling set_tiling = {
		.handle = bo,
				.modifier = mod,
	};
	int ret = drmIoctl(fd, DRM_IOCTL_VC4_SET_TILING,
					   &set_tiling);
	if (ret != 0)
	{
		return 0;
	}

	return 1;
}

void* vc4_bo_map_unsynchronized(int fd, uint32_t bo, uint32_t size)
{
	assert(fd);
	assert(bo);
	assert(size);

	uint64_t offset;
	int ret;

	//if (bo->map)
	//		return bo->map;

	struct drm_vc4_mmap_bo map;
	memset(&map, 0, sizeof(map));
	map.handle = bo;
	ret = drmIoctl(fd, DRM_IOCTL_VC4_MMAP_BO, &map);
	offset = map.offset;
	if (ret != 0) {
		printf("map ioctl failure\n");
		return 0;
	}

	void* mapPtr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
						fd, offset);
	if (mapPtr == MAP_FAILED) {
		printf("mmap of bo %d (offset 0x%016llx, size %d) failed\n",
			   bo, (long long)offset, size);
		return 0;
	}
	//VG(VALGRIND_MALLOCLIKE_BLOCK(bo->map, bo->size, 0, false));

	return mapPtr;
}

static int vc4_bo_wait_ioctl(int fd, uint32_t handle, uint64_t timeout_ns)
{
	assert(fd);
	assert(handle);

	struct drm_vc4_wait_bo wait = {
		.handle = handle,
				.timeout_ns = timeout_ns,
	};
	int ret = drmIoctl(fd, DRM_IOCTL_VC4_WAIT_BO, &wait);
	if (ret == -1)
	{
		printf("bo wait fail: %s", strerror(errno));
		return 0;
	}
	else
	{
		return 1;
	}
}

int vc4_bo_wait(int fd, uint32_t bo, uint64_t timeout_ns)
{
	assert(fd);
	assert(bo);

	int ret = vc4_bo_wait_ioctl(fd, bo, timeout_ns);
	if (ret) {
		if (ret != -ETIME) {
			fprintf(stderr, "wait failed: %d\n", ret);
		}

		return 0;
	}

	return 1;
}

static int vc4_seqno_wait_ioctl(int fd, uint64_t seqno, uint64_t timeout_ns)
{
	assert(fd);
	assert(seqno);

	struct drm_vc4_wait_seqno wait = {
		.seqno = seqno,
				.timeout_ns = timeout_ns,
	};
	int ret = drmIoctl(fd, DRM_IOCTL_VC4_WAIT_SEQNO, &wait);
	if (ret == -1)
	{
		printf("bo wait fail: %s", strerror(errno));
		return 0;
	}
	else
	{
		return 1;
	}
}

int vc4_seqno_wait(int fd, uint64_t* lastFinishedSeqno, uint64_t seqno, uint64_t timeout_ns)
{
	assert(fd);
	assert(lastFinishedSeqno);
	assert(seqno);

	if (*lastFinishedSeqno >= seqno)
		return 1;

	int ret = vc4_seqno_wait_ioctl(fd, seqno, timeout_ns);
	if (ret) {
		if (ret != -ETIME) {
			printf("wait failed: %d\n", ret);
		}

		return 0;
	}

	*lastFinishedSeqno = seqno;
	return 1;
}

int vc4_bo_flink(int fd, uint32_t bo, uint32_t *name)
{
	assert(fd);
	assert(bo);
	assert(name);

	struct drm_gem_flink flink = {
		.handle = bo,
	};
	int ret = drmIoctl(fd, DRM_IOCTL_GEM_FLINK, &flink);
	if (ret) {
		printf("Failed to flink bo %d: %s\n",
			   bo, strerror(errno));
		//free(bo);
		return 0;
	}

	//bo->private = false;
	*name = flink.name;

	return 1;
}

uint32_t vc4_bo_alloc_shader(int fd, const void *data, uint32_t* size)
{
	assert(fd);
	assert(data);
	assert(size);

	int ret;

	uint32_t alignedSize = align(*size, ARM_PAGE_SIZE);

	struct drm_vc4_create_shader_bo create = {
		.size = alignedSize,
				.data = (uintptr_t)data,
	};

	ret = drmIoctl(fd, DRM_IOCTL_VC4_CREATE_SHADER_BO,
				   &create);

	if (ret != 0) {
		printf("create shader ioctl failure\n");
		return 0;
	}

	*size = alignedSize;

	return create.handle;
}

uint32_t vc4_bo_open_name(int fd, uint32_t name)
{
	assert(fd);
	assert(name);

	struct drm_gem_open o = {
		.name = name
	};
	int ret = drmIoctl(fd, DRM_IOCTL_GEM_OPEN, &o);
	if (ret) {
		printf("Failed to open bo %d: %s\n",
			   name, strerror(errno));
		return 0;
	}

	return o.handle;
}

uint32_t vc4_bo_alloc(int fd, uint32_t size, const char *name)
{
	assert(fd);
	assert(size);

	struct drm_vc4_create_bo create;
	int ret;

	uint32_t alignedSize = align(size, ARM_PAGE_SIZE);

	/*bo = vc4_bo_from_cache(screen, size, name);
		if (bo) {
				if (dump_stats) {
						fprintf(stderr, "Allocated %s %dkb from cache:\n",
								name, size / 1024);
						vc4_bo_dump_stats(screen);
				}
				return bo;
		}*/

	memset(&create, 0, sizeof(create));
	create.size = alignedSize;

	ret = drmIoctl(fd, DRM_IOCTL_VC4_CREATE_BO, &create);
	uint32_t handle = create.handle;

	if (ret != 0) {
		/*if (!list_empty(&screen->bo_cache.time_list) &&
					!cleared_and_retried) {
						cleared_and_retried = true;
						vc4_bo_cache_free_all(&screen->bo_cache);
						goto retry;
				}

				free(bo);*/
		return 0;
	}

	vc4_bo_label(fd, handle, name);

	return handle;
}

void vc4_bo_free(int fd, uint32_t bo, void* mappedAddr, uint32_t size)
{
	assert(fd);
	assert(bo);
	assert(size);

	if (mappedAddr) {
		munmap(mappedAddr, size);
		//VG(VALGRIND_FREELIKE_BLOCK(bo->map, 0));
	}

	struct drm_gem_close c;
	memset(&c, 0, sizeof(c));
	c.handle = bo;
	int ret = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
	if (ret != 0)
	{
		printf("close object %d: %s\n", bo, strerror(errno));
	}
}

int vc4_bo_unpurgeable(int fd, uint32_t bo, int hasMadvise)
{
	assert(fd);
	assert(bo);

	struct drm_vc4_gem_madvise arg = {
		.handle = bo,
				.madv = VC4_MADV_WILLNEED,
	};

	if (!hasMadvise)
		return 1;

	if (drmIoctl(fd, DRM_IOCTL_VC4_GEM_MADVISE, &arg))
		return 0;

	return arg.retained;
}

void vc4_bo_purgeable(int fd, uint32_t bo, int hasMadvise)
{
	assert(fd);
	assert(bo);

	struct drm_vc4_gem_madvise arg = {
		.handle = bo,
				.madv = VC4_MADV_DONTNEED,
	};

	if (hasMadvise)
	{
		drmIoctl(fd, DRM_IOCTL_VC4_GEM_MADVISE, &arg);
	}
}

void vc4_bo_label(int fd, uint32_t bo, const char* name)
{
	assert(fd);
	assert(bo);

	char* str = name;
	if(!str) str = "";

	//TODO don't use in release!

	struct drm_vc4_label_bo label = {
		.handle = bo,
				.len = strlen(str),
				.name = (uintptr_t)str,
	};
	drmIoctl(fd, DRM_IOCTL_VC4_LABEL_BO, &label);
}

int vc4_bo_get_dmabuf(int fd, uint32_t bo)
{
	assert(fd);
	assert(bo);

	int boFd;
	int ret = drmPrimeHandleToFD(fd, bo,
								 O_CLOEXEC, &boFd);
	if (ret != 0) {
		printf("Failed to export gem bo %d to dmabuf\n",
			   bo);
		return 0;
	}

	return boFd;
}

void* vc4_bo_map(int fd, uint32_t bo, uint32_t size)
{
	assert(fd);
	assert(bo);
	assert(size);

	void* map = vc4_bo_map_unsynchronized(fd, bo, size);

	//wait infinitely
	int ok = vc4_bo_wait(fd, bo, WAIT_TIMEOUT_INFINITE);
	if (!ok) {
		printf("BO wait for map failed\n");
		return 0;
	}

	return map;
}

void vc4_cl_submit(int fd, struct drm_vc4_submit_cl* submit, uint64_t* lastEmittedSeqno, uint64_t* lastFinishedSeqno)
{
	assert(fd);
	assert(submit);
	assert(lastEmittedSeqno);
	assert(lastFinishedSeqno);

	int ret = drmIoctl(fd, DRM_IOCTL_VC4_SUBMIT_CL, submit);

	static int warned = 0;
	if (ret && !warned) {
		printf("Draw call returned %s.  "
			   "Expect corruption.\n", strerror(errno));
		warned = 1;
	} else if (!ret) {
		*lastEmittedSeqno = submit->seqno;
	}

	if (*lastEmittedSeqno - *lastFinishedSeqno > 5) {
		uint64_t seqno = *lastFinishedSeqno - 5;
		if (!vc4_seqno_wait(fd,
							lastFinishedSeqno,
							seqno,
							WAIT_TIMEOUT_INFINITE))
		{
			printf("Job throttling failed\n");
		}
	}
}
