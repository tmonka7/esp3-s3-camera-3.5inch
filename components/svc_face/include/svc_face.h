/* Face recognition, self-contained.
 *
 * There is no trained model in this firmware and nothing is downloaded. Faces
 * are described with local binary pattern histograms, so the reference data
 * is simply the faces you enrol: no weights, no model partition, no network.
 *
 * What that costs is accuracy. This is reliable at telling apart a handful of
 * people who stand roughly where they stood when they enrolled, in light of
 * roughly the same colour. It is not reliable at large angles, in the dark,
 * behind glasses that were not worn at enrolment, or against a stranger who
 * happens to resemble someone enrolled. It also cannot tell a face from a
 * photograph of one -- no RGB camera can.
 *
 * Treat it as "probably Kim" rather than proof, and see the README before
 * putting it on anything that matters. Swapping in esp-dl later means
 * replacing descriptor extraction and matching; everything else -- enrolment,
 * storage, gating, the UI -- stays.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_MAX_SUBJECTS   8
#define FACE_SAMPLES_PER    5       /* reference shots kept per person */
#define FACE_NAME_LEN       24

typedef struct {
    uint16_t id;                    /* stable, never reused within a db */
    char     name[FACE_NAME_LEN];
    uint8_t  samples;
    uint32_t added_epoch;
} face_subject_t;

/** What the recogniser saw in the last frame it looked at. */
typedef struct {
    bool     face_found;
    uint16_t x, y, w, h;            /* box, in analysis-frame pixels    */
    uint16_t frame_w, frame_h;      /* analysis frame size, for scaling */
    uint16_t match_id;              /* 0 when nothing matched           */
    char     match_name[FACE_NAME_LEN];
    uint8_t  confidence;            /* 0..100                           */
} face_status_t;

/**
 * Allocates buffers, loads the enrolled faces from the TF card and tells
 * svc_access that a recogniser exists. Call after svc_media_init().
 */
esp_err_t svc_face_init(void);

/** True once init succeeded; the UI hides the face controls when false. */
bool svc_face_ready(void);

/* ---- running ---------------------------------------------------------- *
 *
 * Follows settings.access_face_enabled; this forces it either way and is what
 * subscribes to camera frames, so leaving it off costs nothing.
 */
esp_err_t svc_face_set_enabled(bool enabled);
bool      svc_face_is_enabled(void);

/** Last result. Safe to call from the UI task. */
void svc_face_status(face_status_t *out);

/* ---- enrolment -------------------------------------------------------- *
 *
 * Capture runs on the camera: hold still and the recogniser takes
 * FACE_SAMPLES_PER shots a few hundred milliseconds apart, so the stored
 * reference covers a little natural movement rather than one frozen pose.
 */
esp_err_t svc_face_enroll_begin(const char *name);
void      svc_face_enroll_cancel(void);
bool      svc_face_enrolling(void);

/** Shots captured so far, 0..FACE_SAMPLES_PER. */
uint8_t   svc_face_enroll_progress(void);

/* ---- enrolled people -------------------------------------------------- */
size_t    svc_face_subject_count(void);
esp_err_t svc_face_subject_get(size_t index, face_subject_t *out);
esp_err_t svc_face_subject_remove(size_t index);

#ifdef __cplusplus
}
#endif
