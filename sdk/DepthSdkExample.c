#include "DepthSdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

typedef struct CaptureCounts {
    uint64_t video;
    uint64_t depth;
    uint32_t mode;
    int valid;
} CaptureCounts;

static void DCTS_DEPTH_CALL count_frame(void *user, const DctsDepthFrame *frame)
{
    CaptureCounts *counts = user;
    const DctsDepthImage *image = &frame->image;
    if (frame->size < sizeof(*frame) || !image->data || !frame->sequence || !frame->timestamp_ns
        || frame->mode != counts->mode || !frame->device_id_utf8 || !*frame->device_id_utf8
        || !frame->capture_generation_utf8 || strlen(frame->capture_generation_utf8) != 32) {
        counts->valid = 0;
        return;
    }
    if (frame->stream == DCTS_DEPTH_STREAM_VIDEO) {
        counts->valid &= image->width == 2304 && image->height == 1296 && image->pixel_format == DCTS_DEPTH_PIXEL_RGB8;
        ++counts->video;
    } else if (frame->stream == DCTS_DEPTH_STREAM_DEPTH) {
        const int host = frame->mode == DCTS_DEPTH_HOST_ASSISTED;
        counts->valid &= image->width == (host ? 480u : 48u) && image->height == (host ? 640u : 32u)
            && image->pixel_format == DCTS_DEPTH_PIXEL_F32_LE
            && image->byte_count == (uint64_t)image->row_stride_bytes * image->height
            && frame->coordinate == (host ? DCTS_DEPTH_COORDINATE_CAMERA_Z : DCTS_DEPTH_COORDINATE_SENSOR_RADIAL)
            && frame->metadata_json_utf8;
        if (host) {
            counts->valid &= frame->paired_rgb.data && frame->paired_rgb.width == 480 && frame->paired_rgb.height == 640
                && frame->paired_rgb.pixel_format == DCTS_DEPTH_PIXEL_RGB8 && frame->preview_prediction.data
                && frame->preview_prediction.byte_count == image->byte_count
                && frame->preview_global_reasons.byte_count == 48 * 32 && frame->preview_local_reasons.byte_count == 48 * 32;
        }
        ++counts->depth;
    }
}

static char *utf8(const wchar_t *text)
{
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    char *result = size ? malloc(size) : NULL;
    if (result) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result, size, NULL, NULL);
    return result;
}

static int print_devices(DctsDepthSession *session)
{
    uint32_t size = 0;
    if (DctsDepth_ListDevices(session, NULL, &size) != DCTS_DEPTH_OK) return 0;
    char *json = malloc(size);
    const int ok = json && DctsDepth_ListDevices(session, json, &size) == DCTS_DEPTH_OK;
    if (ok) puts(json);
    free(json);
    return ok;
}

static int wait_for_frames(DctsDepthSession *session, CaptureCounts *counts, DWORD timeout)
{
    const uint64_t end = GetTickCount64() + timeout;
    while (GetTickCount64() < end && counts->valid) {
        if (DctsDepth_Poll(session) != DCTS_DEPTH_OK) break;
        if (counts->video && counts->depth) return 1;
        Sleep(10);
    }
    return 0;
}

static int stopped_without_frames(DctsDepthSession *session, const CaptureCounts *counts)
{
    const CaptureCounts before = *counts;
    const uint64_t end = GetTickCount64() + 300;
    while (GetTickCount64() < end) {
        if (DctsDepth_Poll(session) != DCTS_DEPTH_OK) return 0;
        Sleep(10);
    }
    return counts->valid && counts->video == before.video && counts->depth == before.depth;
}

static DctsDepthResult start_capture(DctsDepthSession *session)
{
    const uint64_t end = GetTickCount64() + 10000;
    do {
        DctsDepthResult result = DctsDepth_Start(session);
        if (result != DCTS_DEPTH_BUSY) return result;
        if (DctsDepth_Poll(session) != DCTS_DEPTH_OK) break;
        Sleep(10);
    } while (GetTickCount64() < end);
    return DCTS_DEPTH_BUSY;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3 || argc > 5) {
        fputs("Usage: DctsDepthSdkExample RUNTIME_DIRECTORY DEVICES_JSON [board|high|host [DEVICE_ID]]\n", stderr);
        return 2;
    }
    char *runtime = utf8(argv[1]);
    char *store = utf8(argv[2]);
    char *device = argc == 5 ? utf8(argv[4]) : NULL;
    const DctsDepthOptions options = {sizeof(options), DCTS_DEPTH_ABI_VERSION, runtime, store};
    DctsDepthSession *session = NULL;
    DctsDepthResult status = DctsDepth_Open(&options, &session);
    int passed = 0;
    if (status != DCTS_DEPTH_OK) goto finished;
    if (argc == 3) {
        passed = print_devices(session);
        goto finished;
    }
    const uint32_t mode = wcscmp(argv[3], L"board") == 0 ? DCTS_DEPTH_BOARD
        : wcscmp(argv[3], L"high") == 0 ? DCTS_DEPTH_BOARD_HIGH_ACCURACY
        : wcscmp(argv[3], L"host") == 0 ? DCTS_DEPTH_HOST_ASSISTED : 0;
    CaptureCounts counts = {0, 0, mode, 1};
    if (argc == 5 && (status = DctsDepth_SelectDevice(session, device)) != DCTS_DEPTH_OK) goto finished;
    if ((status = DctsDepth_SetMode(session, mode)) != DCTS_DEPTH_OK) goto finished;
    if ((status = DctsDepth_Subscribe(session, count_frame, NULL, &counts)) != DCTS_DEPTH_OK) goto finished;
    if ((status = start_capture(session)) != DCTS_DEPTH_OK || !wait_for_frames(session, &counts, 90000)) goto finished;
    printf("Initial video frames %llu, depth frames %llu\n", (unsigned long long)counts.video, (unsigned long long)counts.depth);
    if ((status = DctsDepth_Stop(session)) != DCTS_DEPTH_OK || !stopped_without_frames(session, &counts)) goto finished;
    counts.video = counts.depth = 0;
    if ((status = DctsDepth_Reconnect(session)) != DCTS_DEPTH_OK || !wait_for_frames(session, &counts, 90000)) goto finished;
    printf("Reconnected video frames %llu, depth frames %llu\n", (unsigned long long)counts.video, (unsigned long long)counts.depth);
    if ((status = DctsDepth_Stop(session)) != DCTS_DEPTH_OK) goto finished;
    passed = stopped_without_frames(session, &counts);
finished:
    if (session && !passed) {
        char state[4096];
        uint32_t size = sizeof(state);
        if (DctsDepth_GetState(session, state, &size) == DCTS_DEPTH_OK) fputs(state, stderr);
    }
    if (session) DctsDepth_Close(session);
    free(runtime);
    free(store);
    free(device);
    if (!passed) fprintf(stderr, "\nSDK example failed (result %d)\n", status);
    return passed ? 0 : 1;
}
