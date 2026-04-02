#include "capture.h"

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct CaptureState {
    /* DXGI */
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGIOutputDuplication *duplication;
    ID3D11Texture2D *staging;
    int screen_width;
    int screen_height;

    /* Multi-monitor: which output to capture */
    int output_index;

    /* Threading */
    HANDLE thread;
    volatile int running;

    /* Region crop */
    int has_region;
    int region_x, region_y, region_w, region_h;

    /* Rate limiting */
    int target_fps;
    LONGLONG min_frame_interval;
    LONGLONG last_frame_time;
    LONGLONG perf_freq;

    /* Suspend/snap */
    int suspended;
    int snap_requested;

    /* Callbacks */
    frame_callback_t callback;
    error_callback_t error_callback;
    void *userdata;
    void *error_userdata;
};

static LONGLONG get_time(CaptureState *state) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1000000000LL / state->perf_freq;
}

static int init_dxgi(CaptureState *state) {
    D3D_FEATURE_LEVEL feature_level;
    HRESULT hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        0, NULL, 0, D3D11_SDK_VERSION,
        &state->device, &feature_level, &state->context);
    if (FAILED(hr)) return -1;

    IDXGIDevice *dxgi_device = NULL;
    hr = ID3D11Device_QueryInterface(state->device, &IID_IDXGIDevice,
                                     (void **)&dxgi_device);
    if (FAILED(hr)) return -1;

    IDXGIAdapter *adapter = NULL;
    hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    IDXGIDevice_Release(dxgi_device);
    if (FAILED(hr)) return -1;

    IDXGIOutput *output = NULL;
    hr = IDXGIAdapter_EnumOutputs(adapter, state->output_index, &output);
    IDXGIAdapter_Release(adapter);
    if (FAILED(hr)) return -1;

    IDXGIOutput1 *output1 = NULL;
    hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1,
                                    (void **)&output1);
    IDXGIOutput_Release(output);
    if (FAILED(hr)) return -1;

    hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)state->device,
                                       &state->duplication);
    IDXGIOutput1_Release(output1);
    if (FAILED(hr)) return -1;

    DXGI_OUTDUPL_DESC desc;
    IDXGIOutputDuplication_GetDesc(state->duplication, &desc);
    state->screen_width = desc.ModeDesc.Width;
    state->screen_height = desc.ModeDesc.Height;

    /* Create staging texture for CPU read */
    D3D11_TEXTURE2D_DESC tex_desc = {0};
    tex_desc.Width = state->screen_width;
    tex_desc.Height = state->screen_height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_STAGING;
    tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = ID3D11Device_CreateTexture2D(state->device, &tex_desc, NULL,
                                       &state->staging);
    if (FAILED(hr)) return -1;

    return 0;
}

static DWORD WINAPI capture_thread(LPVOID arg) {
    CaptureState *state = arg;
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    IDXGIResource *resource = NULL;

    while (state->running) {
        HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(
            state->duplication, 100, &frame_info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) break;

        /* Rate limiting */
        if (state->min_frame_interval > 0) {
            LONGLONG now = get_time(state);
            if ((now - state->last_frame_time) < state->min_frame_interval) {
                IDXGIOutputDuplication_ReleaseFrame(state->duplication);
                IDXGIResource_Release(resource);
                continue;
            }
            state->last_frame_time = now;
        }

        /* Suspend check */
        if (state->suspended && !state->snap_requested) {
            IDXGIOutputDuplication_ReleaseFrame(state->duplication);
            IDXGIResource_Release(resource);
            continue;
        }
        state->snap_requested = 0;

        /* Copy to staging texture */
        ID3D11Texture2D *frame_tex = NULL;
        hr = IDXGIResource_QueryInterface(resource, &IID_ID3D11Texture2D,
                                          (void **)&frame_tex);
        IDXGIResource_Release(resource);
        if (FAILED(hr)) {
            IDXGIOutputDuplication_ReleaseFrame(state->duplication);
            continue;
        }

        ID3D11DeviceContext_CopyResource(state->context,
                                         (ID3D11Resource *)state->staging,
                                         (ID3D11Resource *)frame_tex);
        ID3D11Texture2D_Release(frame_tex);

        /* Map and deliver frame */
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = ID3D11DeviceContext_Map(state->context,
                                     (ID3D11Resource *)state->staging,
                                     0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr) && state->callback) {
            uint8_t *data = mapped.pData;
            int width = state->screen_width;
            int height = state->screen_height;
            int stride = mapped.RowPitch;

            /* BGRA→RGBA conversion + optional region crop */
            if (state->has_region) {
                int rx = state->region_x;
                int ry = state->region_y;
                int rw = state->region_w;
                int rh = state->region_h;
                if (rx < 0) { rw += rx; rx = 0; }
                if (ry < 0) { rh += ry; ry = 0; }
                if (rx + rw > width) rw = width - rx;
                if (ry + rh > height) rh = height - ry;

                if (rw > 0 && rh > 0) {
                    int crop_stride = rw * 4;
                    uint8_t *crop = malloc(crop_stride * rh);
                    if (crop) {
                        for (int row = 0; row < rh; row++) {
                            uint8_t *src = data + (ry + row) * stride + rx * 4;
                            uint8_t *dst = crop + row * crop_stride;
                            for (int col = 0; col < rw; col++) {
                                dst[col * 4 + 0] = src[col * 4 + 2]; /* R */
                                dst[col * 4 + 1] = src[col * 4 + 1]; /* G */
                                dst[col * 4 + 2] = src[col * 4 + 0]; /* B */
                                dst[col * 4 + 3] = src[col * 4 + 3]; /* A */
                            }
                        }
                        state->callback(state->userdata, crop, rw, rh, crop_stride);
                        free(crop);
                    }
                }
            } else {
                /* Full frame BGRA→RGBA */
                uint8_t *rgba = malloc(width * 4 * height);
                if (rgba) {
                    for (int row = 0; row < height; row++) {
                        uint8_t *src = data + row * stride;
                        uint8_t *dst = rgba + row * width * 4;
                        for (int col = 0; col < width; col++) {
                            dst[col * 4 + 0] = src[col * 4 + 2];
                            dst[col * 4 + 1] = src[col * 4 + 1];
                            dst[col * 4 + 2] = src[col * 4 + 0];
                            dst[col * 4 + 3] = src[col * 4 + 3];
                        }
                    }
                    state->callback(state->userdata, rgba, width, height, width * 4);
                    free(rgba);
                }
            }

            ID3D11DeviceContext_Unmap(state->context,
                                      (ID3D11Resource *)state->staging, 0);
        }

        IDXGIOutputDuplication_ReleaseFrame(state->duplication);
    }
    return 0;
}

CaptureState *capture_create(void) {
    CaptureState *state = calloc(1, sizeof(CaptureState));
    if (!state) return NULL;
    state->target_fps = 30;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    state->perf_freq = freq.QuadPart;
    state->min_frame_interval = 1000000000LL / 30;
    return state;
}

int capture_start(CaptureState *state, frame_callback_t cb, void *userdata) {
    state->callback = cb;
    state->userdata = userdata;

    if (init_dxgi(state) < 0) {
        fprintf(stderr, "capture: DXGI init failed\n");
        return -1;
    }

    state->running = 1;
    state->thread = CreateThread(NULL, 0, capture_thread, state, 0, NULL);
    if (!state->thread) {
        state->running = 0;
        return -1;
    }
    return 0;
}

void capture_stop(CaptureState *state) {
    if (!state || !state->running) return;

    state->running = 0;
    WaitForSingleObject(state->thread, INFINITE);
    CloseHandle(state->thread);
    state->thread = NULL;

    if (state->staging) { ID3D11Texture2D_Release(state->staging); state->staging = NULL; }
    if (state->duplication) { IDXGIOutputDuplication_Release(state->duplication); state->duplication = NULL; }
    if (state->context) { ID3D11DeviceContext_Release(state->context); state->context = NULL; }
    if (state->device) { ID3D11Device_Release(state->device); state->device = NULL; }
}

void capture_destroy(CaptureState *state) {
    if (!state) return;
    capture_stop(state);
    free(state);
}

void capture_set_region(CaptureState *state, int x, int y, int w, int h) {
    state->region_x = x;
    state->region_y = y;
    state->region_w = w;
    state->region_h = h;
    state->has_region = 1;
}

void capture_set_rate(CaptureState *state, int fps) {
    state->target_fps = fps;
    state->min_frame_interval = fps > 0 ? 1000000000LL / fps : 0;
}

void capture_suspend(CaptureState *state) { state->suspended = 1; }
void capture_resume(CaptureState *state) { state->suspended = 0; }
void capture_snap(CaptureState *state) { state->snap_requested = 1; }

void capture_on_error(CaptureState *state, error_callback_t cb, void *userdata) {
    state->error_callback = cb;
    state->error_userdata = userdata;
}

int capture_set_display(CaptureState *state, const char *display_id) {
    if (!display_id) return -1;
    state->output_index = atoi(display_id);
    return 0;
}

int capture_list_displays(CaptureState *state,
                          CaptureDisplayInfo *out, int max_displays) {
    (void)state;
    if (!out || max_displays <= 0) return -1;

    /* Create a temporary D3D11 device to enumerate outputs */
    ID3D11Device *dev = NULL;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                    0, NULL, 0, D3D11_SDK_VERSION,
                                    &dev, &fl, NULL);
    if (FAILED(hr)) return -1;

    IDXGIDevice *dxgi_dev = NULL;
    hr = ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev);
    if (FAILED(hr)) { ID3D11Device_Release(dev); return -1; }

    IDXGIAdapter *adapter = NULL;
    hr = IDXGIDevice_GetAdapter(dxgi_dev, &adapter);
    IDXGIDevice_Release(dxgi_dev);
    if (FAILED(hr)) { ID3D11Device_Release(dev); return -1; }

    int count = 0;
    for (UINT i = 0; count < max_displays; i++) {
        IDXGIOutput *output = NULL;
        hr = IDXGIAdapter_EnumOutputs(adapter, i, &output);
        if (FAILED(hr)) break;

        DXGI_OUTPUT_DESC desc;
        IDXGIOutput_GetDesc(output, &desc);
        IDXGIOutput_Release(output);

        snprintf(out[count].connector, sizeof(out[count].connector), "%u", i);
        out[count].x = desc.DesktopCoordinates.left;
        out[count].y = desc.DesktopCoordinates.top;
        out[count].width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        out[count].height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        count++;
    }

    IDXGIAdapter_Release(adapter);
    ID3D11Device_Release(dev);
    return count;
}
