/* ********************************************************************* *\

Copyright (C) 2013 Intel Corporation.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
- Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.
- Neither the name of Intel Corporation nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY INTEL CORPORATION "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL INTEL CORPORATION BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

\* ********************************************************************* */

#ifndef ENC_QSV_H
#define ENC_QSV_H

#include "hb.h"
#include "hb_dict.h"
#include "libavcodec/qsv.h"

int nal_find_start_code(uint8_t** pb, size_t* size);
void parse_nalus( uint8_t *nal_inits, size_t length, hb_buffer_t *buf, uint32_t frame_num);


#define QSV_NAME_async_depth    "async-depth"
#define QSV_NAME_target_usage   "target-usage"
#define QSV_NAME_num_ref_frame  "num-ref-frame"
#define QSV_NAME_gop_ref_dist   "gop-ref-dist"
#define QSV_NAME_gop_pic_size   "gop-pic-size"
#define QSV_NAME_vbv_bufsize    "vbv-bufsize"
#define QSV_NAME_vbv_maxrate    "vbv-maxrate"
#define QSV_NAME_vbv_init       "vbv-init"
#define QSV_NAME_mbbrc          "mbbrc"
#define QSV_NAME_extbrc         "extbrc"
#define QSV_NAME_cqp_offset_i   "cqp-offset-i"
#define QSV_NAME_cqp_offset_p   "cqp-offset-p"
#define QSV_NAME_cqp_offset_b   "cqp-offset-b"

typedef enum {
    QSV_PARAM_OK            = 0,
    QSV_PARAM_BAD_NAME      = -1,
    QSV_PARAM_BAD_VALUE     = -2,
    QSV_PARAM_BAD_CONFIG    = -3,
} qsv_param_errors;

int qsv_param_parse( av_qsv_config* config, const char *name, const char *value);
void qsv_param_set_defaults( av_qsv_config* config);

#endif //ENC_QSV_H