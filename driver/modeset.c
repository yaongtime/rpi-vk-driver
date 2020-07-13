#include "modeset.h"

#include "fifo.h"

#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

static modeset_saved_state modeset_saved_states[32];

typedef struct vsyncData
{
	_image* i;
	modeset_display_surface* s;
	uint32_t flipPending;
	uint64_t seqno;
} vsyncData;

#define FLIP_FIFO_SIZE (2)

static uint32_t refCount = 0;
static pthread_t flipQueueThread = 0;
static Fifo flipQueueFifo;
static vsyncData dataMem[FLIP_FIFO_SIZE];
static FifoElem fifoMem[FLIP_FIFO_SIZE];
static sem_t flipQueueSem;

static sem_t savedStateSem;

#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

static int get_plane_id(int drm_fd, int crtc_index)
{
	drmModePlaneResPtr plane_resources;
	uint32_t i, j;
	int ret = -EINVAL;
	int found_primary = 0;

	plane_resources = drmModeGetPlaneResources(drm_fd);
	if (!plane_resources) {
		printf("drmModeGetPlaneResources failed: %s\n", strerror(errno));
		return -1;
	}

	for (i = 0; (i < plane_resources->count_planes) && !found_primary; i++) {
		uint32_t id = plane_resources->planes[i];
		drmModePlanePtr plane = drmModeGetPlane(drm_fd, id);
		if (!plane) {
			printf("drmModeGetPlane(%u) failed: %s\n", id, strerror(errno));
			continue;
		}

		if (plane->possible_crtcs & (1 << crtc_index)) {
			drmModeObjectPropertiesPtr props =
				drmModeObjectGetProperties(drm_fd, id, DRM_MODE_OBJECT_PLANE);

			/* primary or not, this plane is good enough to use: */
			ret = id;

			for (j = 0; j < props->count_props; j++) {
				drmModePropertyPtr p =
					drmModeGetProperty(drm_fd, props->props[j]);

				if ((strcmp(p->name, "type") == 0) &&
						(props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)) {
					/* found our primary plane, lets use that: */
					found_primary = 1;
				}
				drmModeFreeProperty(p);
			}
			drmModeFreeObjectProperties(props);
		}
		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_resources);

	return ret;
}

static int init_drm(struct drm *drm)
{
    drmModeRes *resources;
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    int i;

    resources = drmModeGetResources(drm->fd);
	if (resources == NULL) {
        printf("could not get resources: %s\n", strerror(errno));
		return -1;
    }

    /* find a connected connector: */
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm->fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			/* it's connected, let's use this! */
			drm->connector_id = connector->connector_id;
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}

    if (!connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		printf("no connected connector!\n");
		return -1;
	}

    printf("find a connect\n");

    /* find preferred mode or the highest resolution mode: */
	if (!drm->mode) {
        int area;
		for (i = 0, area = 0; i < connector->count_modes; i++) {
			drmModeModeInfo *current_mode = &connector->modes[i];

			if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
				drm->mode = current_mode;
				break;
			}

			int current_area = current_mode->hdisplay * current_mode->vdisplay;
			if (current_area > area) {
				drm->mode = current_mode;
				area = current_area;
			}
		}
	}

    if (!drm->mode) {
		printf("could not find mode!\n");
		goto err;
	}

    printf("use mode:  %s %d %d %d %d %d %d %d %d %d %d\n",
	       drm->mode->name,
	       drm->mode->vrefresh,
	       drm->mode->hdisplay,
	       drm->mode->hsync_start,
	       drm->mode->hsync_end,
	       drm->mode->htotal,
	       drm->mode->vdisplay,
	       drm->mode->vsync_start,
	       drm->mode->vsync_end,
	       drm->mode->vtotal,
	       drm->mode->clock);

    /* find encoder: */
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(drm->fd, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (encoder) {
		drm->crtc_id = encoder->crtc_id;
		drmModeFreeEncoder(encoder);
	} else {
        printf("Can't get a encoder\n");
		goto err;
	}

	for (i = 0; i < resources->count_crtcs; i++) {
		if (resources->crtcs[i] == drm->crtc_id) {
			drm->crtc_index = i;
			break;
		}
	}

	drm->plane_id = get_plane_id(drm->fd, drm->crtc_index);

		/* We only do single plane to single crtc to single connector, no
	 * fancy multi-monitor or multi-plane stuff.  So just grab the
	 * plane/crtc/connector property info for one of each:
	 */
	drm->plane = calloc(1, sizeof(*drm->plane));
	drm->crtc = calloc(1, sizeof(*drm->crtc));
	drm->connector = calloc(1, sizeof(*drm->connector));

#define get_resource(type, Type, id) do { 					\
		drm->type->type = drmModeGet##Type(drm->fd, id);			\
		if (!drm->type->type) {						\
			printf("could not get %s %i: %s\n",			\
					#type, id, strerror(errno));		\
			return NULL;						\
		}								\
	} while (0)

	get_resource(plane, Plane, drm->plane_id);
	get_resource(crtc, Crtc, drm->crtc_id);
	get_resource(connector, Connector, drm->connector_id);

#define get_properties(type, TYPE, id) do {					\
		uint32_t i;							\
		drm->type->props = drmModeObjectGetProperties(drm->fd,		\
				id, DRM_MODE_OBJECT_##TYPE);			\
		if (!drm->type->props) {						\
			printf("could not get %s %u properties: %s\n", 		\
					#type, id, strerror(errno));		\
			return NULL;						\
		}								\
		drm->type->props_info = calloc(drm->type->props->count_props,	\
				sizeof(*drm->type->props_info));			\
		for (i = 0; i < drm->type->props->count_props; i++) {		\
			drm->type->props_info[i] = drmModeGetProperty(drm->fd,	\
					drm->type->props->props[i]);		\
		}								\
	} while (0)

	get_properties(plane, PLANE, drm->plane_id);
	get_properties(crtc, CRTC, drm->crtc_id);
	get_properties(connector, CONNECTOR, drm->connector_id);

err:
	// drmModeFreeConnector(connector);
    drmModeFreeResources(resources);

    return 0;
}

static int add_connector_property(struct drm *drm, drmModeAtomicReq *req, uint32_t obj_id,
					const char *name, uint64_t value)
{
	struct connector *obj = drm->connector;
	unsigned int i;
	int prop_id = 0;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0) {
		printf("no connector property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_crtc_property(struct drm *drm, drmModeAtomicReq *req, uint32_t obj_id,
				const char *name, uint64_t value)
{
	struct crtc *obj = drm->crtc;
	unsigned int i;
	int prop_id = -1;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id < 0) {
		printf("no crtc property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_plane_property(struct drm *drm, drmModeAtomicReq *req, uint32_t obj_id,
				const char *name, uint64_t value)
{
	struct plane *obj = drm->plane;
	unsigned int i;
	int prop_id = -1;

	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}


	if (prop_id < 0) {
		printf("no plane property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int drm_atomic_commit(uint32_t fb_id, uint32_t flags, struct drm *drm, uint64_t user_data)
{
	drmModeAtomicReq *req;
	uint32_t plane_id = drm->plane_id;
	uint32_t blob_id;
	int ret;

	struct drm_event_vblank buf;

	req = drmModeAtomicAlloc();

	if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
		if (add_connector_property(drm, req, drm->connector_id, "CRTC_ID",
						drm->crtc_id) < 0)
				return -1;

		if (drmModeCreatePropertyBlob(drm->fd, drm->mode, sizeof(*drm->mode),
					      &blob_id) != 0)
			return -1;

		if (add_crtc_property(drm, req, drm->crtc_id, "MODE_ID", blob_id) < 0)
			return -1;

		if (add_crtc_property(drm, req, drm->crtc_id, "ACTIVE", 1) < 0)
			return -1;
	}

	add_plane_property(drm, req, plane_id, "FB_ID", fb_id);
	add_plane_property(drm, req, plane_id, "CRTC_ID", drm->crtc_id);
	add_plane_property(drm, req, plane_id, "SRC_X", 0);
	add_plane_property(drm, req, plane_id, "SRC_Y", 0);
	add_plane_property(drm, req, plane_id, "SRC_W", drm->mode->hdisplay << 16);
	add_plane_property(drm, req, plane_id, "SRC_H", drm->mode->vdisplay << 16);
	add_plane_property(drm, req, plane_id, "CRTC_X", 0);
	add_plane_property(drm, req, plane_id, "CRTC_Y", 0);
	add_plane_property(drm, req, plane_id, "CRTC_W", drm->mode->hdisplay);
	add_plane_property(drm, req, plane_id, "CRTC_H", drm->mode->vdisplay);

	if (drm->kms_in_fence_fd != -1) {
		add_crtc_property(drm, req, drm->crtc_id, "OUT_FENCE_PTR",
				VOID2U64(&drm->kms_out_fence_fd));
		add_plane_property(drm, req, plane_id, "IN_FENCE_FD", drm->kms_in_fence_fd);
	}

	ret = drmModeAtomicCommit(drm->fd, req, flags, user_data);
	if (ret)
		goto out;

	if (drm->kms_in_fence_fd != -1) {
		close(drm->kms_in_fence_fd);
		drm->kms_in_fence_fd = -1;
	}

	// ret = read(drm->fd, &buf, sizeof(buf));
	// if(ret < 0 ) {
	// 	printf("vblank read fail\n");
	// } else {
	// 	printf("vblank %u %u\n", buf.base.type, buf.base.length);
	// }

	// ret = 0;

out:
	drmModeAtomicFree(req);

	return ret;
}

static void* flipQueueThreadFunction(void* vargp)
{
	uint32_t run = 1;
	uint64_t lastFinishedSeqno = 0;
	int threadFD = *(int*)vargp;

	while(run)
	{
		uint64_t seqno = 0;

		sem_wait(&flipQueueSem);
		{
			vsyncData* d = fifoGetLast(&flipQueueFifo);
			if(d)
			{
				seqno = d->seqno;
			}

			run = refCount;
		}
		sem_post(&flipQueueSem);

		if(seqno)
		{
			uint64_t timeOut = 1000000; //1 ms in ns
			int ret = vc4_seqno_wait(threadFD, &lastFinishedSeqno, seqno, &timeOut);

			if(ret > 0)
			{
				sem_wait(&flipQueueSem);
				{
					vsyncData* d = fifoGetLast(&flipQueueFifo);

					d->seqno = 0; //done!

					if(d->i->presentMode == VK_PRESENT_MODE_FIFO_KHR)
					{
						d->flipPending = 1;
#ifdef DRM_ATOMIC_MODE_ENABLE
						if(d->s->drm_atmoic.count == 0) {
							drm_atomic_commit(d->i->fb, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET, &d->s->drm_atmoic, (uint64_t)d);
						} else {
							drm_atomic_commit(d->i->fb, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, &d->s->drm_atmoic, (uint64_t)d);
						}
#else
						drmModePageFlip(threadFD, d->s->crtc->crtc_id, d->i->fb, DRM_MODE_PAGE_FLIP_EVENT, d);
#endif
					}
					else if(d->i->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
					{
#ifdef DRM_ATOMIC_MODE_ENABLE
						if(d->s->drm_atmoic.count == 0) {
							drm_atomic_commit(d->i->fb, DRM_MODE_ATOMIC_ALLOW_MODESET, &d->s->drm_atmoic, 0);
						} else {
							drm_atomic_commit(d->i->fb, 0, &d->s->drm_atmoic, 0);
						}
#else
						drmModePageFlip(threadFD, d->s->crtc->crtc_id, d->i->fb, DRM_MODE_PAGE_FLIP_ASYNC, 0);
#endif
					}

					// d->s->drm_atmoic.count++;
				}
				sem_post(&flipQueueSem);
			}
		}
	}

	return 0;
}

void modeset_enum_displays(int fd, uint32_t* numDisplays, modeset_display* displays)
{
	 drmModeResPtr resPtr = drmModeGetResources(fd);

	 uint32_t tmpNumDisplays = 0;
	 modeset_display tmpDisplays[16];

#ifdef DRM_ATOMIC_MODE_ENABLE
	 drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	 drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
#endif

	 for(int c = 0; c < resPtr->count_connectors; ++c)
	 {
		 drmModeConnectorPtr connPtr = drmModeGetConnector(fd, resPtr->connectors[c]);

		 if(connPtr->connection != DRM_MODE_CONNECTED)
		 {
			 drmModeFreeConnector(connPtr);
			 continue; //skip unused connector
		 }

		 if(!connPtr->count_modes)
		 {
			 drmModeFreeConnector(connPtr);
			 continue; //skip connectors with no valid modes
		 }

		//  memcpy(tmpDisplays[tmpNumDisplays].name, connPtr->modes[0].name, 32);

		 switch (connPtr->connector_type)
		 {
		 case DRM_MODE_CONNECTOR_HDMIA:
			 strcpy(tmpDisplays[tmpNumDisplays].name, "HDMI");
			 break;
		 case DRM_MODE_CONNECTOR_Composite:
		 	 strcpy(tmpDisplays[tmpNumDisplays].name, "TV-OUT");
			 break;
		 case DRM_MODE_CONNECTOR_DSI:
		 	 strcpy(tmpDisplays[tmpNumDisplays].name, "DSI");
		 	 break;
		 default:
			 break;
		 }

		 tmpDisplays[tmpNumDisplays].mmWidth = connPtr->mmWidth;
		 tmpDisplays[tmpNumDisplays].mmHeight = connPtr->mmHeight;
		 tmpDisplays[tmpNumDisplays].resWidth = connPtr->modes[0].hdisplay;
		 tmpDisplays[tmpNumDisplays].resHeight = connPtr->modes[0].vdisplay;
		 tmpDisplays[tmpNumDisplays].connectorID = connPtr->connector_id;

		 tmpNumDisplays++;

		 assert(tmpNumDisplays < 16);

		 drmModeFreeConnector(connPtr);
	 }

	 drmModeFreeResources(resPtr);

	 *numDisplays = tmpNumDisplays;

	 memcpy(displays, tmpDisplays, tmpNumDisplays * sizeof(modeset_display));
}

void modeset_enum_modes_for_display(int fd, uint32_t display, uint32_t* numModes, modeset_display_mode* modes)
{
	drmModeResPtr resPtr = drmModeGetResources(fd);

	drmModeConnectorPtr connPtr = drmModeGetConnector(fd, display);

	if(!connPtr)
	{
		*numModes = 0;
		return;
	}

	uint32_t tmpNumModes = 0;
	modeset_display_mode tmpModes[1024];

	for(int c = 0; c < connPtr->count_modes; ++c)
	{
		uint32_t found = 0;
		for(uint32_t d = 0; d < tmpNumModes; ++d)
		{
			if(tmpModes[d].refreshRate == connPtr->modes[c].vrefresh &&
			   tmpModes[d].resWidth == connPtr->modes[c].hdisplay &&
			   tmpModes[d].resHeight == connPtr->modes[c].vdisplay)
			{
				found = 1;
				break;
			}
		}

		if(found)
		{
			//skip modes that have lower "clock"
			//but otherwise same resolution and vrefresh
			continue;
		}

		tmpModes[tmpNumModes].connectorID = display;
		tmpModes[tmpNumModes].modeID = c;
		tmpModes[tmpNumModes].refreshRate = connPtr->modes[c].vrefresh;
		tmpModes[tmpNumModes].resWidth = connPtr->modes[c].hdisplay;
		tmpModes[tmpNumModes].resHeight = connPtr->modes[c].vdisplay;

		//explained here:
		// https://01.org/linuxgraphics/gfx-docs/drm/API-struct-drm-display-mode.html

//		char* typestr = 0;
//		switch(connPtr->modes[c].type)
//		{
//		case DRM_MODE_TYPE_BUILTIN:
//			typestr = "DRM_MODE_TYPE_BUILTIN";
//			break;
//		case DRM_MODE_TYPE_CLOCK_C:
//			typestr = "DRM_MODE_TYPE_CLOCK_C";
//			break;
//		case DRM_MODE_TYPE_CRTC_C:
//			typestr = "DRM_MODE_TYPE_CRTC_C";
//			break;
//		case DRM_MODE_TYPE_PREFERRED:
//			typestr = "DRM_MODE_TYPE_PREFERRED";
//			break;
//		case DRM_MODE_TYPE_DEFAULT:
//			typestr = "DRM_MODE_TYPE_DEFAULT";
//			break;
//		case DRM_MODE_TYPE_USERDEF:
//			typestr = "DRM_MODE_TYPE_USERDEF";
//			break;
//		case DRM_MODE_TYPE_DRIVER:
//			typestr = "DRM_MODE_TYPE_DRIVER";
//			break;
//		default:
//			typestr = "UNKNOWN";
//			break;
//		}

//		fprintf(stderr, "\nflags ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_PHSYNC) fprintf(stderr, "DRM_MODE_FLAG_PHSYNC ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_NHSYNC) fprintf(stderr, "DRM_MODE_FLAG_NHSYNC ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_PVSYNC) fprintf(stderr, "DRM_MODE_FLAG_PVSYNC ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_NVSYNC) fprintf(stderr, "DRM_MODE_FLAG_NVSYNC ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_INTERLACE) fprintf(stderr, "DRM_MODE_FLAG_INTERLACE ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_DBLSCAN) fprintf(stderr, "DRM_MODE_FLAG_DBLSCAN ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_CSYNC) fprintf(stderr, "DRM_MODE_FLAG_CSYNC ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_PCSYNC) fprintf(stderr, "DRM_MODE_FLAG_PCSYNC ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_NCSYNC) fprintf(stderr, "DRM_MODE_FLAG_NCSYNC ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_HSKEW) fprintf(stderr, "DRM_MODE_FLAG_HSKEW ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_BCAST) fprintf(stderr, "DRM_MODE_FLAG_BCAST ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_PIXMUX) fprintf(stderr, "DRM_MODE_FLAG_PIXMUX ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_DBLCLK) fprintf(stderr, "DRM_MODE_FLAG_DBLCLK ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_CLKDIV2) fprintf(stderr, "DRM_MODE_FLAG_CLKDIV2 ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_3D_MASK) fprintf(stderr, "DRM_MODE_FLAG_3D_MASK ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_3D_NONE) fprintf(stderr, "DRM_MODE_FLAG_3D_NONE ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_3D_FRAME_PACKING) fprintf(stderr, "DRM_MODE_FLAG_3D_FRAME_PACKING ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_3D_FIELD_ALTERNATIVE) fprintf(stderr, "DRM_MODE_FLAG_3D_FIELD_ALTERNATIVE ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_3D_LINE_ALTERNATIVE) fprintf(stderr, "DRM_MODE_FLAG_3D_LINE_ALTERNATIVE ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL) fprintf(stderr, "DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_3D_L_DEPTH) fprintf(stderr, "DRM_MODE_FLAG_3D_L_DEPTH ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_3D_L_DEPTH_GFX_GFX_DEPTH) fprintf(stderr, "DRM_MODE_FLAG_3D_L_DEPTH_GFX_GFX_DEPTH ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_3D_TOP_AND_BOTTOM) fprintf(stderr, "DRM_MODE_FLAG_3D_TOP_AND_BOTTOM ");
//		if(connPtr->modes[c].flags & DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF) fprintf(stderr, "DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF ");

//		fprintf(stderr, "\nclock %u vrefresh %u type %s\n", connPtr->modes[c].clock, connPtr->modes[c].vrefresh, typestr);
//		fprintf(stderr, "hdisplay %u hsync_start %u hsync_end %u htotal %u hskew %u\n", connPtr->modes[c].hdisplay, connPtr->modes[c].hsync_start, connPtr->modes[c].hsync_end, connPtr->modes[c].htotal, connPtr->modes[c].hskew);
//		fprintf(stderr, "vdisplay %u vsync_start %u vsync_end %u vtotal %u vscan %u\n", connPtr->modes[c].vdisplay, connPtr->modes[c].vsync_start, connPtr->modes[c].vsync_end, connPtr->modes[c].vtotal, connPtr->modes[c].vscan);

		tmpNumModes++;

		assert(tmpNumModes < 1024);
	}

	drmModeFreeConnector(connPtr);
	drmModeFreeResources(resPtr);

	*numModes = tmpNumModes;
	memcpy(modes, tmpModes, tmpNumModes * sizeof(modeset_display_mode));
}

void modeset_create_surface_for_atomic_mode(int fd, uint32_t display, uint32_t mode, modeset_display_surface* surface)
{
	if(!refCount)
	{
		flipQueueFifo = createFifo(dataMem, fifoMem, FLIP_FIFO_SIZE, sizeof(vsyncData));
		pthread_create(&flipQueueThread, 0, flipQueueThreadFunction, &fd);
		sem_init(&flipQueueSem, 0, 0);
		sem_post(&flipQueueSem);

		sem_init(&savedStateSem, 0, 0);
		sem_post(&savedStateSem);
	}

	refCount++;

	surface->drm_atmoic.kms_in_fence_fd = -1;
	surface->drm_atmoic.fd = fd;
	init_drm(&surface->drm_atmoic);

	surface->modeID = mode;
	surface->savedState = 0;
}

void modeset_create_surface_for_mode(int fd, uint32_t display, uint32_t mode, modeset_display_surface* surface)
{
//	modeset_debug_print(fd);

	if(!refCount)
	{
		flipQueueFifo = createFifo(dataMem, fifoMem, FLIP_FIFO_SIZE, sizeof(vsyncData));
		pthread_create(&flipQueueThread, 0, flipQueueThreadFunction, &fd);
		sem_init(&flipQueueSem, 0, 0);
		sem_post(&flipQueueSem);

		sem_init(&savedStateSem, 0, 0);
		sem_post(&savedStateSem);
	}

	refCount++;

	surface->savedState = 0;

	drmModeResPtr resPtr = drmModeGetResources(fd);

	drmModeConnectorPtr connPtr = drmModeGetConnector(fd, display);

	if(!connPtr)
	{
		return;
	}

	uint32_t found = 0;

	//if current encoder is valid, try to use that
	if(connPtr->encoder_id)
	{
		drmModeEncoderPtr encPtr = drmModeGetEncoder(fd, connPtr->encoder_id);

		if(encPtr && encPtr->crtc_id)
		{
			surface->connector = connPtr;
			surface->modeID = mode;
			surface->crtc = drmModeGetCrtc(fd, encPtr->crtc_id);

			// get plane ID that belong to crtc_id
			for (int i = 0; i < resPtr->count_crtcs; i++) {
				if (resPtr->crtcs[i] == encPtr->crtc_id) {
					surface->plane_id = get_plane_id(fd, i);
					break;
				}
			}

			found = 1;
		}

		drmModeFreeEncoder(encPtr);
	}

	if(!found)
	{
//		To find a suitable CRTC, you need to iterate over the list of encoders that are available
//		for each connector. Each encoder contains a list of CRTCs that it can work with and you
//		simply select one of these CRTCs. If you later program the CRTC to control a connector,
//		it automatically selects the best encoder

//		TODO if we were to output to multiple displays, we'd need to make sure we don't use a CRTC
//		that we'd be using to drive the other screen

		for(int c = 0; c < connPtr->count_encoders; ++c)
		{
			drmModeEncoderPtr encPtr = drmModeGetEncoder(fd, connPtr->encoders[c]);

			for(uint32_t d = 0; d < 32; ++d)
			{
				if(encPtr->possible_crtcs & (1 << d))
				{
					surface->connector = connPtr;
					surface->modeID = mode;
					surface->crtc = drmModeGetCrtc(fd, resPtr->crtcs[d]);

					found = 1;
					break;
				}
			}

			if(found)
			{
				break;
			}
		}
	}

	drmModeFreeResources(resPtr);

	//fprintf(stderr, "connector id %i, crtc id %i\n", connPtr->connector_id, encPtr->crtc_id);
}

uint32_t drm_mode_legacy_fb_format(uint32_t bpp, uint32_t depth)
{
	uint32_t fmt = 0;

	switch (bpp) {
	case 8:
		if (depth == 8)
			fmt = DRM_FORMAT_C8;
		break;

	case 16:
		switch (depth) {
		case 15:
			fmt = DRM_FORMAT_XRGB1555;
			break;
		case 16:
			fmt = DRM_FORMAT_RGB565;
			break;
		default:
			break;
		}
		break;

	case 24:
		if (depth == 24)
			fmt = DRM_FORMAT_RGB888;
		break;

	case 32:
		switch (depth) {
		case 24:
			fmt = DRM_FORMAT_XRGB8888;
			break;
		case 30:
			fmt = DRM_FORMAT_XRGB2101010;
			break;
		case 32:
			fmt = DRM_FORMAT_ARGB8888;
			break;
		default:
			break;
		}
		break;

	default:
		break;
	}

	return fmt;
}

void modeset_create_fb_for_surface(int fd, _image* buf, modeset_display_surface* surface)
{


	// int bpp = buf->format == VK_FORMAT_B8G8R8A8_UNORM ? 32 : 16;
	// int ret = drmModeAddFB(fd, buf->width, buf->height, bpp, bpp, buf->stride, buf->boundMem->bo, &buf->fb);

	uint32_t bpp = buf->format == VK_FORMAT_B8G8R8A8_UNORM ? 32 : 16;
	uint32_t bo_handles[4] = {}, pitches[4] = {}, offsets[4] = {};

	bo_handles[0] = buf->boundMem->bo;
	pitches[0] = buf->stride;
	int ret = drmModeAddFB2(fd, buf->width, buf->height, drm_mode_legacy_fb_format(bpp, bpp), bo_handles, pitches, offsets, &buf->fb, 0);
	if(ret)
	{
		buf->fb = 0;
		fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
	}

	printf("-- %s %u\n", __func__, buf->fb);
}

void modeset_destroy_fb(int fd, _image* buf)
{
	// delete framebuffer
	drmModeRmFB(fd, buf->fb);
}

static void modeset_page_flip_event(int fd, unsigned int frame,
					unsigned int sec, unsigned int usec,
					void *data)
{
	if(data)
	{
		sem_wait(&flipQueueSem);
		{
			if(data)
			{
				vsyncData* d = data;

				d->flipPending = 0;
			}
		}
		sem_post(&flipQueueSem);
	}
}

void modeset_acquire_image(int fd, _image** buf, modeset_display_surface** surface)
{
	uint32_t pending = 1, gpuprocessing = 1;

	sem_wait(&flipQueueSem);

	vsyncData* last = fifoGetLast(&flipQueueFifo);

	if(last)
	{
		pending = last->flipPending;
		gpuprocessing = last->seqno > 0;
	}
	else
	{
		//fifo empty, just use any image
		*buf = 0;
		*surface = 0;

		sem_post(&flipQueueSem);

		return;
	}

	sem_post(&flipQueueSem);

	while(pending || gpuprocessing)
	{
		if(pending)
		{
			drmEventContext ev;
			memset(&ev, 0, sizeof(ev));
			ev.version = 2;
			ev.page_flip_handler = modeset_page_flip_event;
			drmHandleEvent(fd, &ev);
		}

		sem_wait(&flipQueueSem);
		{
			vsyncData* d = fifoGetLast(&flipQueueFifo);

			//a frame must be in flight
			//so fifo must contain something
			assert(d);

			pending = d->flipPending;
			gpuprocessing = d->seqno > 0;
		}
		sem_post(&flipQueueSem);
	}

	sem_wait(&flipQueueSem);
	{
		vsyncData d;

		fifoRemove(&flipQueueFifo, &d);

		*buf = d.i;
		*surface = d.s;
	}
	sem_post(&flipQueueSem);
}

void modeset_present_atmoic(int fd, _image *buf, modeset_display_surface* surface, uint64_t seqno)
{
	if(!surface->savedState)
	{
		sem_wait(&savedStateSem);
		{
			for(uint32_t c = 0; c < 32; ++c)
			{
				if(!modeset_saved_states[c].used)
				{
					drmModeConnectorPtr tmpConnPtr = drmModeGetConnector(fd, surface->drm_atmoic.connector->connector->connector_id);
					drmModeCrtcPtr tmpCrtcPtr = drmModeGetCrtc(fd, surface->drm_atmoic.crtc->crtc->crtc_id);
					modeset_saved_states[c].used = 1;
					modeset_saved_states[c].conn = tmpConnPtr;
					modeset_saved_states[c].crtc = tmpCrtcPtr;
					surface->savedState = c;
					break;
				}
			}
			surface->drm_atmoic.count = 0;
		}
		sem_post(&savedStateSem);
	}

	//TODO presenting needs to happen *after* the gpu is done with rendering to an image
	//then immediate mode flips the page immediately, but fifo will post it for next vblank
	//otherwise fifo would tear too
	//so we should add images to the flip queue
	//then wait for the first submitted (first out) seqno to finish
	//then perform pageflip for that image
	//
	//drmModePageFlip(fd, surface->crtc->crtc_id, buf->fb, DRM_MODE_PAGE_FLIP_EVENT, first);
	//drmModePageFlip(fd, surface->crtc->crtc_id, buf->fb, DRM_MODE_PAGE_FLIP_ASYNC, 0);

	vsyncData d;

	d.i = buf;
	d.s = surface;
	d.flipPending = 0;
	d.seqno = seqno;

	// printf("%s Send to %lu fifo\n", __func__, seqno);
	// printf("%s: seqno = %lu\n", __func__, seqno);

	uint32_t added = 0;

	while(!added)
	{
		sem_wait(&flipQueueSem);
		{
			//try to add request to queue
			added = fifoAdd(&flipQueueFifo, &d);
		}

		vsyncData* back = fifoGetFirst(&flipQueueFifo);

		sem_post(&flipQueueSem);
	}

	//modeset_debug_print(fd);
}

void modeset_present(int fd, _image *buf, modeset_display_surface* surface, uint64_t seqno)
{
	if(!surface->savedState)
	{
		sem_wait(&savedStateSem);
		{
			for(uint32_t c = 0; c < 32; ++c)
			{
				if(!modeset_saved_states[c].used)
				{
					drmModeConnectorPtr tmpConnPtr = drmModeGetConnector(fd, surface->connector->connector_id);
					drmModeCrtcPtr tmpCrtcPtr = drmModeGetCrtc(fd, surface->crtc->crtc_id);
					modeset_saved_states[c].used = 1;
					modeset_saved_states[c].conn = tmpConnPtr;
					modeset_saved_states[c].crtc = tmpCrtcPtr;
					surface->savedState = c;
					break;
				}
			}

			int ret = drmModeSetCrtc(fd, surface->crtc->crtc_id, buf->fb, 0, 0, &surface->connector->connector_id, 1, &surface->connector->modes[surface->modeID]);
			if(ret)
			{
				fprintf(stderr, "cannot set CRTC for connector %u: %m\n",
					   surface->connector->connector_id, errno);
			}
		}
		sem_post(&savedStateSem);
	}

	//TODO presenting needs to happen *after* the gpu is done with rendering to an image
	//then immediate mode flips the page immediately, but fifo will post it for next vblank
	//otherwise fifo would tear too
	//so we should add images to the flip queue
	//then wait for the first submitted (first out) seqno to finish
	//then perform pageflip for that image
	//
	//drmModePageFlip(fd, surface->crtc->crtc_id, buf->fb, DRM_MODE_PAGE_FLIP_EVENT, first);
	//drmModePageFlip(fd, surface->crtc->crtc_id, buf->fb, DRM_MODE_PAGE_FLIP_ASYNC, 0);

	vsyncData d;
	d.i = buf;
	d.s = surface;
	d.flipPending = 0;
	d.seqno = seqno;

	uint32_t added = 0;

	while(!added)
	{
		sem_wait(&flipQueueSem);
		{
			//try to add request to queue
			added = fifoAdd(&flipQueueFifo, &d);
		}
		sem_post(&flipQueueSem);
	}

	//modeset_debug_print(fd);
}

void modeset_destroy_surface(int fd, modeset_display_surface *surface)
{
	//restore old state
	drmModeSetCrtc(fd, modeset_saved_states[surface->savedState].crtc->crtc_id,
			modeset_saved_states[surface->savedState].crtc->buffer_id,
			modeset_saved_states[surface->savedState].crtc->x,
			modeset_saved_states[surface->savedState].crtc->y,
			&modeset_saved_states[surface->savedState].conn->connector_id,
			1,
			&modeset_saved_states[surface->savedState].crtc->mode);

	{
		refCount--;

		sem_wait(&savedStateSem);
		{
			drmModeFreeConnector(modeset_saved_states[surface->savedState].conn);
			drmModeFreeCrtc(modeset_saved_states[surface->savedState].crtc);
			modeset_saved_states[surface->savedState].used = 0;
		}
		sem_post(&savedStateSem);

		if(!refCount)
		{
			destroyFifo(&flipQueueFifo);
			pthread_join(flipQueueThread, 0);
		}
	}

	drmModeFreeConnector(surface->connector);
	drmModeFreeCrtc(surface->crtc);
}

void modeset_debug_print(int fd)
{
	drmModeResPtr resPtr = drmModeGetResources(fd);
	printf("res min width %i height %i\n", resPtr->min_width, resPtr->min_height);
	printf("res max width %i height %i\n", resPtr->max_width, resPtr->max_height);

	printf("\ncrtc count %i\n", resPtr->count_crtcs);
	for(int c = 0; c < resPtr->count_crtcs; ++c)
	{
		drmModeCrtcPtr tmpCrtcPtr = drmModeGetCrtc(fd, resPtr->crtcs[c]);
		printf("crtc id %i, buffer id %i\n", tmpCrtcPtr->crtc_id, tmpCrtcPtr->buffer_id);
		drmModeFreeCrtc(tmpCrtcPtr);
	}

	printf("\nfb count %i\n", resPtr->count_fbs);
	for(int c = 0; c < resPtr->count_fbs; ++c)
	{
	   drmModeFBPtr tmpFBptr = drmModeGetFB(fd, resPtr->fbs[c]);
	   printf("fb id %i, handle %i\n", tmpFBptr->fb_id, tmpFBptr->handle);
	   drmModeFreeFB(tmpFBptr);
	}

	printf("\nencoder count %i\n", resPtr->count_encoders);
	for(int c = 0; c < resPtr->count_encoders; ++c)
	{
	   drmModeEncoderPtr tmpEncoderPtr = drmModeGetEncoder(fd, resPtr->encoders[c]);
	   printf("encoder id %i, crtc id %i\n", tmpEncoderPtr->encoder_id, tmpEncoderPtr->crtc_id);
	   printf("possible crtcs: ");
	   for(uint32_t c = 0; c < 32; ++c)
	   {
		   if(tmpEncoderPtr->possible_crtcs & (1 << c))
		   {
			   printf("%i, ", c);
		   }
	   }
	   printf("\n");

	   printf("possible clones: ");
	   for(uint32_t c = 0; c < 32; ++c)
	   {
		   if(tmpEncoderPtr->possible_clones & (1 << c))
		   {
			   printf("%i, ", c);
		   }
	   }
	   printf("\n");

	   drmModeFreeEncoder(tmpEncoderPtr);
	}

	printf("\nconnector count %i\n", resPtr->count_connectors);
	for(int c = 0; c < resPtr->count_connectors; ++c)
	{
		drmModeConnectorPtr tmpConnPtr = drmModeGetConnector(fd, resPtr->connectors[c]);
		printf("connector id %i, encoder id %i\n", tmpConnPtr->connector_id, tmpConnPtr->encoder_id);
		drmModeFreeConnector(tmpConnPtr);
	}

	drmModeFreeResources(resPtr);
}
