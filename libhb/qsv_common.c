/* qsv_common.c
 *
 * Copyright (c) 2003-2013 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#include "common.h"
#include "qsv_common.h"

// avoids a warning
#include "libavutil/cpu.h"
extern void ff_cpu_cpuid(int index, int *eax, int *ebx, int *ecx, int *edx);

// make the Intel QSV information available to the UIs
hb_qsv_info_t *hb_qsv_info = NULL;

// availability and versions
static mfxVersion qsv_hardware_version;
static mfxVersion qsv_software_version;
static mfxVersion qsv_minimum_version;
static int qsv_hardware_available = 0;
static int qsv_software_available = 0;
static char cpu_name_buf[48];

// check available Intel Media SDK version against a minimum
#define HB_CHECK_MFX_VERSION(MFX_VERSION, MAJOR, MINOR) \
    (MFX_VERSION.Major == MAJOR  && MFX_VERSION.Minor >= MINOR)

int hb_qsv_available()
{
    return hb_qsv_info != NULL && (qsv_hardware_available ||
                                   qsv_software_available);
}

int hb_qsv_info_init()
{
    static int init_done = 0;
    if (init_done)
        return (hb_qsv_info == NULL);
    init_done = 1;

    hb_qsv_info = calloc(1, sizeof(*hb_qsv_info));
    if (hb_qsv_info == NULL)
    {
        hb_error("hb_qsv_info_init: alloc failure");
        return -1;
    }

    hb_qsv_info->cpu_name = NULL;
    // detect the CPU platform to check for hardware-specific capabilities
    if (av_get_cpu_flags() & AV_CPU_FLAG_SSE)
    {
        int eax, ebx, ecx, edx;
        int family = 0, model = 0;

        ff_cpu_cpuid(1, &eax, &ebx, &ecx, &edx);
        family = ((eax >> 8) & 0xf) + ((eax >> 20) & 0xff);
        model  = ((eax >> 4) & 0xf) + ((eax >> 12) & 0xf0);

        // Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 2A
        // Figure 3-8: Determination of Support for the Processor Brand String
        // Table 3-17: Information Returned by CPUID Instruction
        ff_cpu_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
        if ((eax & 0x80000004) < 0x80000004)
        {
            ff_cpu_cpuid(0x80000002,
                         (int*)&cpu_name_buf[ 0], (int*)&cpu_name_buf[ 4],
                         (int*)&cpu_name_buf[ 8], (int*)&cpu_name_buf[12]);
            ff_cpu_cpuid(0x80000003,
                         (int*)&cpu_name_buf[16], (int*)&cpu_name_buf[20],
                         (int*)&cpu_name_buf[24], (int*)&cpu_name_buf[28]);
            ff_cpu_cpuid(0x80000004,
                         (int*)&cpu_name_buf[32], (int*)&cpu_name_buf[36],
                         (int*)&cpu_name_buf[40], (int*)&cpu_name_buf[44]);

            cpu_name_buf[47]      = '\0'; // just in case
            hb_qsv_info->cpu_name = (const char*)cpu_name_buf;
            while (isspace(*hb_qsv_info->cpu_name))
            {
                // skip leading whitespace to prettify
                hb_qsv_info->cpu_name++;
            }
        }
        
        // Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3C
        // Table 35-1: CPUID Signature Values of DisplayFamily_DisplayModel
        if (family == 0x06)
        {
            switch (model)
            {
                case 0x2A:
                case 0x2D:
                    hb_qsv_info->cpu_platform = HB_CPU_PLATFORM_INTEL_SNB;
                    break;
                case 0x3A:
                case 0x3E:
                    hb_qsv_info->cpu_platform = HB_CPU_PLATFORM_INTEL_IVB;
                    break;
                case 0x3C:
                case 0x45:
                case 0x46:
                    hb_qsv_info->cpu_platform = HB_CPU_PLATFORM_INTEL_HSW;
                    break;
                default:
                    hb_qsv_info->cpu_platform = HB_CPU_PLATFORM_UNSPECIFIED;
                    break;
            }
        }
    }

    mfxSession session;
    qsv_minimum_version.Major = HB_QSV_MINVERSION_MAJOR;
    qsv_minimum_version.Minor = HB_QSV_MINVERSION_MINOR;

    // check for software fallback
    if (MFXInit(MFX_IMPL_SOFTWARE,
                &qsv_minimum_version, &session) == MFX_ERR_NONE)
    {
        qsv_software_available = 1;
        // our minimum is supported, but query the actual version
        MFXQueryVersion(session, &qsv_software_version);
        MFXClose(session);
    }

    // check for actual hardware support
    if (MFXInit(MFX_IMPL_HARDWARE_ANY|MFX_IMPL_VIA_ANY,
                &qsv_minimum_version, &session) == MFX_ERR_NONE)
    {
        qsv_hardware_available = 1;
        // our minimum is supported, but query the actual version
        MFXQueryVersion(session, &qsv_hardware_version);
        MFXClose(session);
    }

    // check for version-specific or hardware-specific capabilities
    // we only use software as a fallback, so check hardware first
    if (qsv_hardware_available)
    {
        if (HB_CHECK_MFX_VERSION(qsv_hardware_version, 1, 6))
        {
            hb_qsv_info->capabilities |= HB_QSV_CAP_OPTION2_BRC;
            hb_qsv_info->capabilities |= HB_QSV_CAP_BITSTREAM_DTS;
        }
        if (HB_CHECK_MFX_VERSION(qsv_hardware_version, 1, 7))
        {
            if (hb_qsv_info->cpu_platform == HB_CPU_PLATFORM_INTEL_HSW)
            {
                hb_qsv_info->capabilities |= HB_QSV_CAP_OPTION2_LOOKAHEAD;
            }
        }
        if (hb_qsv_info->cpu_platform == HB_CPU_PLATFORM_INTEL_HSW)
        {
            hb_qsv_info->capabilities |= HB_QSV_CAP_H264_BPYRAMID;
        }
    }
    else if (qsv_software_available)
    {
        if (HB_CHECK_MFX_VERSION(qsv_software_version, 1, 6))
        {
            hb_qsv_info->capabilities |= HB_QSV_CAP_OPTION2_BRC;
            hb_qsv_info->capabilities |= HB_QSV_CAP_BITSTREAM_DTS;
            hb_qsv_info->capabilities |= HB_QSV_CAP_H264_BPYRAMID;
        }
    }

    // note: we pass a pointer to MFXInit but it never gets modified
    //       let's make sure of it just to be safe though
    if (qsv_minimum_version.Major != HB_QSV_MINVERSION_MAJOR ||
        qsv_minimum_version.Minor != HB_QSV_MINVERSION_MINOR)
    {
        hb_error("hb_qsv_info_init: minimum version (%d.%d) was modified",
                 qsv_minimum_version.Major,
                 qsv_minimum_version.Minor);
    }

    // success
    return 0;
}

// we don't need it beyond this point
#undef HB_CHECK_MFX_VERSION

void hb_qsv_info_print()
{
    if (hb_qsv_info == NULL)
        return;

    // is QSV available?
    hb_log("Intel Quick Sync Video support: %s",
           hb_qsv_available() ? "yes": "no");

    // print the hardware summary too
    hb_log(" - CPU name: %s", hb_qsv_info->cpu_name);
    switch (hb_qsv_info->cpu_platform)
    {
        // Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3C
        // Table 35-1: CPUID Signature Values of DisplayFamily_DisplayModel
        case HB_CPU_PLATFORM_INTEL_SNB:
            hb_log(" - Intel microarchitecture Sandy Bridge");
            break;
        case HB_CPU_PLATFORM_INTEL_IVB:
            hb_log(" - Intel microarchitecture Ivy Bridge");
            break;
        case HB_CPU_PLATFORM_INTEL_HSW:
            hb_log(" - Intel microarchitecture Haswell");
            break;
        default:
            break;
    }

    // if we have Quick Sync Video support, also print the details
    if (hb_qsv_available())
    {
        if (qsv_hardware_available)
        {
            hb_log(" - Intel Media SDK hardware: API %d.%d (minimum: %d.%d)",
                   qsv_hardware_version.Major,
                   qsv_hardware_version.Minor,
                   qsv_minimum_version.Major,
                   qsv_minimum_version.Minor);
        }
        if (qsv_software_available)
        {
            hb_log(" - Intel Media SDK software: API %d.%d (minimum: %d.%d)",
                   qsv_software_version.Major,
                   qsv_software_version.Minor,
                   qsv_minimum_version.Major,
                   qsv_minimum_version.Minor);
        }
    }
}

const char* hb_qsv_decode_get_codec_name(enum AVCodecID codec_id)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            return "h264_qsv";
            
        default:
            return NULL;
    }
}

int hb_qsv_decode_is_enabled(hb_job_t *job)
{
    return ((job != NULL && job->title->qsv_decode_support && job->qsv_decode) &&
            (job->vcodec & HB_VCODEC_QSV_MASK));
}

int hb_qsv_decode_is_supported(enum AVCodecID codec_id,
                               enum AVPixelFormat pix_fmt)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            return (pix_fmt == AV_PIX_FMT_YUV420P ||
                    pix_fmt == AV_PIX_FMT_YUVJ420P);

        default:
            return 0;
    }
}
