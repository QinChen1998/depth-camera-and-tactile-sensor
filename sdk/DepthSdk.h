#ifndef DCTS_DEPTH_SDK_H
#define DCTS_DEPTH_SDK_H

#include <stdint.h>

#ifdef DCTS_DEPTH_SDK_BUILD
#define DCTS_DEPTH_API __declspec(dllexport)
#else
#define DCTS_DEPTH_API __declspec(dllimport)
#endif
#define DCTS_DEPTH_CALL __cdecl

#ifdef __cplusplus
extern "C" {
#endif

#define DCTS_DEPTH_ABI_VERSION 1u

typedef struct DctsDepthSession DctsDepthSession;
typedef int32_t DctsDepthResult;
enum {
    DCTS_DEPTH_OK = 0,
    DCTS_DEPTH_INVALID_ARGUMENT = 1,
    DCTS_DEPTH_BUSY = 2,
    DCTS_DEPTH_WRONG_THREAD = 3,
    DCTS_DEPTH_DEVICE_NOT_FOUND = 4,
    DCTS_DEPTH_BUFFER_TOO_SMALL = 5,
    DCTS_DEPTH_UNAVAILABLE = 6
};

enum {
    DCTS_DEPTH_BOARD = 1,
    DCTS_DEPTH_BOARD_HIGH_ACCURACY = 2,
    DCTS_DEPTH_HOST_ASSISTED = 3
};
enum {
    DCTS_DEPTH_STREAM_VIDEO = 1,
    DCTS_DEPTH_STREAM_DEPTH = 2
};
enum {
    DCTS_DEPTH_PIXEL_RGB8 = 1,
    DCTS_DEPTH_PIXEL_F32_LE = 2,
    DCTS_DEPTH_PIXEL_U8 = 3
};
enum {
    DCTS_DEPTH_COORDINATE_NONE = 0,
    DCTS_DEPTH_COORDINATE_SENSOR_RADIAL = 1,
    DCTS_DEPTH_COORDINATE_CAMERA_Z = 2
};
enum {
    DCTS_DEPTH_CLOCK_HOST_MONOTONIC = 1,
    DCTS_DEPTH_CLOCK_BOARD_MONOTONIC = 2
};
enum {
    DCTS_DEPTH_EVENT_STATE_CHANGED = 1,
    DCTS_DEPTH_EVENT_VIDEO_CLEARED = 2,
    DCTS_DEPTH_EVENT_DEPTH_CLEARED = 3,
    DCTS_DEPTH_EVENT_BOARD_FILES_REQUIRED = 4,
    DCTS_DEPTH_EVENT_INFERENCE_CHANGED = 5,
    DCTS_DEPTH_EVENT_USB_CHANGED = 6
};

typedef struct DctsDepthOptions {
    uint32_t size;
    uint32_t abi_version;
    const char *runtime_directory_utf8; /* Absolute directory containing the SDK runtime. */
    const char *device_store_utf8;       /* Absolute path to a trusted devices.json. */
} DctsDepthOptions;

typedef struct DctsDepthImage {
    const void *data;
    uint64_t byte_count;
    uint32_t width;
    uint32_t height;
    uint32_t row_stride_bytes;
    uint32_t pixel_format;
} DctsDepthImage;

typedef struct DctsDepthFrame {
    uint32_t size;
    uint32_t stream;
    uint32_t mode;
    uint32_t coordinate;
    uint64_t session_epoch;
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint32_t clock_domain;
    uint32_t reserved;
    const char *device_id_utf8;
    const char *capture_generation_utf8;
    DctsDepthImage image;
    DctsDepthImage paired_rgb;
    DctsDepthImage estimated_mask;
    DctsDepthImage preview_prediction;
    DctsDepthImage preview_global_reasons;
    DctsDepthImage preview_local_reasons;
    const char *metadata_json_utf8;
} DctsDepthFrame;

/* Frames and all pointed-to data are read-only and valid only during the callback.
   A callback may query state. Return before issuing a control call or closing. */
typedef void (DCTS_DEPTH_CALL *DctsDepthFrameCallback)(void *user, const DctsDepthFrame *frame);
typedef void (DCTS_DEPTH_CALL *DctsDepthEventCallback)(void *user, uint32_t event);

DCTS_DEPTH_API uint32_t DCTS_DEPTH_CALL DctsDepth_AbiVersion(void);
/* Open and all session operations run on the owner thread. An existing Qt
   application must call Open on its main thread and use its own event loop. */
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Open(const DctsDepthOptions *options, DctsDepthSession **session);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Close(DctsDepthSession *session);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Subscribe(
    DctsDepthSession *session, DctsDepthFrameCallback frame, DctsDepthEventCallback event, void *user);
/* Pump pending events on the owner thread of a standalone session. */
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Poll(DctsDepthSession *session);

/* JSON calls report the required byte count including NUL in size. A null
   buffer queries the size. With a buffer, size initially holds its capacity. */
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_ListDevices(DctsDepthSession *session, char *buffer, uint32_t *size);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_GetState(DctsDepthSession *session, char *buffer, uint32_t *size);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_SelectDevice(DctsDepthSession *session, const char *device_id_utf8);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_SetMode(DctsDepthSession *session, uint32_t mode);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Start(DctsDepthSession *session);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Stop(DctsDepthSession *session);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_Reconnect(DctsDepthSession *session);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_DetectInferenceDevices(DctsDepthSession *session);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_ListInferenceDevices(DctsDepthSession *session, char *buffer, uint32_t *size);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_SelectInferenceDevice(
    DctsDepthSession *session, const char *backend_id_utf8, uint32_t remember);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_BenchmarkInferenceDevices(DctsDepthSession *session);
/* CheckUsb is read-only. EnrollUsb uses a check_id returned by GetUsbStatus.
   The checked attachment and registry are revalidated before changes. */
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_CheckUsb(DctsDepthSession *session);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_GetUsbStatus(DctsDepthSession *session, char *buffer, uint32_t *size);
DCTS_DEPTH_API DctsDepthResult DCTS_DEPTH_CALL DctsDepth_EnrollUsb(
    DctsDepthSession *session, uint64_t check_id, uint32_t start_after);

#ifdef __cplusplus
}
#endif
#endif
