/**
 * @file    app_camera.h
 * @brief   OV5640 Camera Capture Abstraction
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Wraps BSP_CAMERA functions for the MB1379 camera module (OV5640 sensor)
 * connected via DCMI interface on the B-U585I-IOT02A Discovery Kit.
 */

#ifndef __APP_CAMERA_H
#define __APP_CAMERA_H

#include <stdint.h>

/* Image resolution presets */
typedef enum {
    CAMERA_RES_QVGA = 0,   /* 320x240  — fast, low bandwidth */
    CAMERA_RES_VGA,         /* 640x480  — default for thesis */
    CAMERA_RES_SVGA,        /* 800x600  */
    CAMERA_RES_XGA,         /* 1024x768 */
} CameraResolution_t;

/* Camera status */
typedef enum {
    CAMERA_OK = 0,
    CAMERA_ERROR_INIT,
    CAMERA_ERROR_CAPTURE,
    CAMERA_ERROR_TIMEOUT,
} CameraStatus_t;

/**
 * @brief  Initialize the OV5640 camera module.
 * @param  resolution: Desired capture resolution.
 * @retval CAMERA_OK on success.
 */
CameraStatus_t Camera_Init(CameraResolution_t resolution);

/**
 * @brief  Check if the camera is already initialized (warm).
 * @retval 1 if initialized, 0 if not.
 */
uint8_t Camera_IsInitialized(void);

/**
 * @brief  Capture a single frame into the provided buffer.
 *         Includes warmup frames for cold-start AEC convergence.
 * @param  buffer: Pointer to destination buffer (must be large enough).
 * @param  buffer_size: Size of the buffer in bytes.
 * @param  captured_size: Output — actual number of bytes captured.
 * @retval CAMERA_OK on success.
 */
CameraStatus_t Camera_CaptureFrame(uint8_t *buffer, uint32_t buffer_size,
                                    uint32_t *captured_size);

/**
 * @brief  Zero-overhead warm capture — single frame, no init, no warmup.
 *         Use when camera is already initialized and AEC has converged.
 *         This is the enterprise fast-path for sub-second captures.
 * @param  buffer: Pointer to destination buffer.
 * @param  buffer_size: Size of the buffer in bytes.
 * @param  captured_size: Output — actual number of bytes captured.
 * @retval CAMERA_OK on success.
 */
CameraStatus_t Camera_WarmCapture(uint8_t *buffer, uint32_t buffer_size,
                                   uint32_t *captured_size);

/**
 * @brief  Deinitialize the camera to save power before entering sleep.
 * @retval CAMERA_OK on success.
 */
CameraStatus_t Camera_DeInit(void);

/**
 * @brief  Prepare a captured frame for upload, compressing it if possible.
 *
 * A VGA RGB565 frame (exactly 640*480*2 bytes) is JPEG-encoded into a static
 * buffer inside camera.c, cutting ~614 KB down to a few tens of KB on the
 * wire. Any other size (QVGA, hardware-JPEG mode, short capture) or an encode
 * failure falls back to the ORIGINAL raw buffer — the server auto-detects raw
 * RGB565 by exact payload size, so the raw path keeps working untouched.
 *
 * The encode is deterministic: calling this again with the same raw frame
 * yields byte-identical output, which is what makes the pending-upload retry
 * path safe (it re-encodes rather than caching a second copy).
 *
 * @param  frame     Captured frame buffer.
 * @param  frame_len Captured length in bytes.
 * @param  out_data  Output — pointer to the bytes to upload (never NULL on
 *                   return unless @p frame was NULL).
 * @param  out_len   Output — number of bytes to upload.
 */
void Camera_PrepareUpload(uint8_t *frame, uint32_t frame_len,
                          const uint8_t **out_data, uint32_t *out_len);

/**
 * @brief  Wipe the static JPEG upload buffer (hygiene, mirrors secure_erase
 *         of the raw frame buffer once an upload is finished).
 */
void Camera_ClearUploadBuffer(void);

#endif /* __APP_CAMERA_H */
