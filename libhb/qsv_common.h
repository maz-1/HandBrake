/* qsv_common.h
 *
 * Copyright (c) 2003-2013 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifndef HB_QSV_COMMON_H
#define HB_QSV_COMMON_H

#include "msdk/mfxvideo.h"
#include "libavcodec/avcodec.h"

/* Minimum Intel Media SDK version (currently 1.3, for Sandy Bridge support) */
#define HB_QSV_MINVERSION_MAJOR AV_QSV_MSDK_VERSION_MAJOR
#define HB_QSV_MINVERSION_MINOR AV_QSV_MSDK_VERSION_MINOR

/*
 * Get & store all available Intel Quick Sync information:
 *
 * - general availability
 * - available implementations (hardware-accelerated, software fallback, etc.)
 * - available codecs, filters, etc. for direct access (convenience)
 * - supported API version
 * - supported resolutions
 */
typedef struct hb_qsv_info_s
{
    // supported version-specific or hardware-specific capabilities
    int capabilities;
#define HB_QSV_CAP_H264_BPYRAMID     (1 << 0) // H.264: reference B-frames
#define HB_QSV_CAP_MSDK_API_1_6      (1 << 1) // Support for API 1.6 or later
#define HB_QSV_CAP_OPTION2_BRC       (1 << 2) // mfxExtCodingOption2: MBBRC/ExtBRC
#define HB_QSV_CAP_OPTION2_LOOKAHEAD (1 << 3) // mfxExtCodingOption2: LookAhead
#define HB_QSV_CAP_OPTION2_TRELLIS   (1 << 4) // mfxExtCodingOption2: Trellis

    // if a feature depends on the cpu generation
    enum
    {
        // list of microarchitecture codenames
        HB_CPU_PLATFORM_UNSPECIFIED = 0,
        HB_CPU_PLATFORM_INTEL_SNB,
        HB_CPU_PLATFORM_INTEL_IVB,
        HB_CPU_PLATFORM_INTEL_HSW,
    }
    cpu_platform;
    const char *cpu_name;

    // TODO: add available decoders, filters, encoders,
    //       maximum decode and encode resolution, etc.
} hb_qsv_info_t;

/* Global Intel QSV information for use by the UIs */
extern hb_qsv_info_t *hb_qsv_info;

/* Intel Quick Sync Video utilities */
int  hb_qsv_available();
int  hb_qsv_info_init();
void hb_qsv_info_print();

/* Intel Quick Sync Video DECODE utilities */
const char* hb_qsv_decode_get_codec_name(enum AVCodecID codec_id);
int hb_qsv_decode_is_enabled(hb_job_t *job);
int hb_qsv_decode_is_supported(enum AVCodecID codec_id, enum AVPixelFormat pix_fmt);

#define HB_QSV_CLIP3(min, max, val) ((val < min) ? min : (val > max) ? max : val)
int   hb_qsv_codingoption_xlat(int val);
int   hb_qsv_trellisvalue_xlat(int val);
int   hb_qsv_atoindex(const char* const *arr, const char *str, int *err);
int   hb_qsv_atobool (const char *str, int *err);
int   hb_qsv_atoi    (const char *str, int *err);
float hb_qsv_atof    (const char *str, int *err);

#endif
