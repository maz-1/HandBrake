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

#include <stdarg.h>
#include "hb.h"
#include "enc_qsv.h"
#include "h264_common.h"

/**
 * @brief Convert profile string name into profile_e enum with mapping of supported profiles
 *
 * @param[in]   str     string name of profile
 * @retval      mapped  profile_e enum value
 */
int profile_string_to_int( const char *str )
{
    if( !strcasecmp( str, "baseline" ) )
        return PROFILE_BASELINE;
    if( !strcasecmp( str, "main" ) )
        return PROFILE_MAIN;
    if( !strcasecmp( str, "high" ) )
        return PROFILE_HIGH;
    if( !strcasecmp( str, "high10" ) )
        return PROFILE_HIGH;
    if( !strcasecmp( str, "high422" ) )
        return PROFILE_HIGH;
    if( !strcasecmp( str, "high444" ) )
        return PROFILE_HIGH;
    return -1;
}

int qsv_h264_levels[sizeof(hb_h264_level_names)] = {
                                                        0, // "auto"
                                                        MFX_LEVEL_AVC_1,
                                                        MFX_LEVEL_AVC_1b,
                                                        MFX_LEVEL_AVC_11,
                                                        MFX_LEVEL_AVC_12,
                                                        MFX_LEVEL_AVC_13,
                                                        MFX_LEVEL_AVC_2,
                                                        MFX_LEVEL_AVC_21,
                                                        MFX_LEVEL_AVC_22,
                                                        MFX_LEVEL_AVC_3,
                                                        MFX_LEVEL_AVC_31,
                                                        MFX_LEVEL_AVC_32,
                                                        MFX_LEVEL_AVC_4,
                                                        MFX_LEVEL_AVC_41,
                                                        MFX_LEVEL_AVC_42,
                                                        MFX_LEVEL_AVC_5,
                                                        MFX_LEVEL_AVC_51,
                                                        0, // MFX_LEVEL_AVC_52 if MSDK API MFX_VERSION_MAJOR/MFX_VERSION_MINOR is 1/6
                                                        0
                                                    };

int  encqsvInit( hb_work_object_t *, hb_job_t * );
int  encqsvWork( hb_work_object_t *, hb_buffer_t **, hb_buffer_t ** );
void encqsvClose( hb_work_object_t * );

hb_work_object_t hb_encqsv =
{
    WORK_ENCQSV,
    "H.264/AVC encoder (Intel QSV)",
    encqsvInit,
    encqsvWork,
    encqsvClose
};

#define SPSPPS_SIZE     1024

struct hb_work_private_s
{
    hb_job_t       *job;
    hb_esconfig_t  *config;
    uint32_t       frames_in;
    uint32_t       frames_out;

    mfxExtCodingOptionSPSPPS    *sps_pps;

    int codec_profile;
    int codec_level;

    hb_list_t*  delayed_processing;

    // options
    av_qsv_config qsv_config;

    int async_depth;
    int max_async_depth;

    // is only encode/system memory used
    int is_sys_mem;
    struct SwsContext* sws_context_to_nv12;

    // is to expect VPP before
    int is_vpp_present;

    //if set - MB BRC option
    mfxU16  mbbrc;
    mfxU16  extbrc;
    mfxExtCodingOption2  qsv_coding_option2_config;
    mfxExtCodingOption   qsv_coding_option_config;
    int     mbbrx_param_idx;
};

extern char* get_codec_id(hb_work_private_t *pv);

int qsv_enc_init( av_qsv_context* qsv, hb_work_private_t * pv ){
    mfxStatus sts;
    int i = 0;
    int is_encode_only = 0;

    hb_job_t       * job  = pv->job;

    if(!qsv){
        int x = 0;
        for(;x < hb_list_count( job->list_work );x++){
            hb_work_object_t * w = hb_list_item( job->list_work, x );
            if(w->id == WORK_DECAVCODECV || w->id == WORK_DECMPEG2){
                char *codec = get_codec_id(w->private_data);
                is_encode_only = strstr(codec,"qsv") == 0 ? 1 : 0;
                if(is_encode_only)
                    qsv = av_mallocz( sizeof( av_qsv_context ) );
                else
                    hb_error( "Wrong settings for decode/encode" );
                break;
            }
        }
    }
    if(!qsv && !is_encode_only) return 3;

    av_qsv_space* qsv_encode = qsv->enc_space;

    if(qsv_encode == NULL){
        qsv_encode = av_mallocz( sizeof( av_qsv_space ) );
        // if only for encode
        if(is_encode_only){
            // no need to use additional sync as encode only -> single thread
            av_qsv_add_context_usage(qsv,0);

            qsv->impl = MFX_IMPL_AUTO;

            memset(&qsv->mfx_session, 0, sizeof(mfxSession));
            qsv->ver.Major = AV_QSV_MSDK_VERSION_MAJOR;
            qsv->ver.Minor = AV_QSV_MSDK_VERSION_MINOR;

            sts = MFXInit(qsv->impl, &qsv->ver, &qsv->mfx_session);
            AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

            pv->is_sys_mem = 1;
            job->qsv = qsv;
        }
        else{
            av_qsv_add_context_usage(qsv,HAVE_THREADS);
            pv->is_sys_mem = 0;
        }

        qsv->enc_space = qsv_encode;
    }

    if(qsv_encode->is_init_done) return 1;

    if(!pv->is_sys_mem){

        if(!pv->is_vpp_present && job->list_filter){
            int x = 0;
            for(;x < hb_list_count( job->list_filter );x++){
                hb_filter_object_t * filter = hb_list_item( job->list_filter, x );
                if( filter->id == HB_FILTER_QSV_PRE  ||
                    filter->id == HB_FILTER_QSV_POST ||
                    filter->id == HB_FILTER_QSV ){
                        pv->is_vpp_present = 1;
                        break;
                }
            }
        }

        if(pv->is_vpp_present){
            if(!qsv->vpp_space) return 2;

            for(i = 0; i < av_qsv_list_count(qsv->vpp_space); i++){
                av_qsv_space *vpp = av_qsv_list_item(qsv->vpp_space,i);
                if(!vpp->is_init_done)
                     return 2;
            }
        }

        if(!qsv->dec_space) return 2;

        av_qsv_space *dec_space = qsv->dec_space;
        if(!dec_space->is_init_done)
             return 2;
    }
    else{
        pv->sws_context_to_nv12 = hb_sws_get_context(
                            job->width, job->height, AV_PIX_FMT_YUV420P,
                            job->width, job->height, AV_PIX_FMT_NV12,
                            SWS_LANCZOS|SWS_ACCURATE_RND);
    }

    sts = MFXQueryIMPL(qsv->mfx_session,&qsv->impl);
    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    char* impl_name = (MFX_IMPL_SOFTWARE == qsv->impl) ? "software" : "hardware";
    hb_log("qsv: Intel QSV/MediaSDK uses %s implementation", impl_name);

    if(pv->is_sys_mem)
        hb_log("qsv: Using for encode only.");


    AV_QSV_ZERO_MEMORY(qsv_encode->m_mfxVideoParam);
    AV_QSV_ZERO_MEMORY(qsv_encode->m_mfxVideoParam.mfx);

    qsv_param_set_defaults(&pv->qsv_config, hb_qsv_info);

    hb_dict_t *qsv_opts_dict = NULL;
    if( job->advanced_opts != NULL && *job->advanced_opts != '\0' )
        qsv_opts_dict = hb_encopts_to_dict( job->advanced_opts, job->vcodec );

    int ret;
    hb_dict_entry_t *entry = NULL;
    while( ( entry = hb_dict_next( qsv_opts_dict, entry ) ) )
    {
        ret = qsv_param_parse( &pv->qsv_config, entry->key, entry->value );
        if( ret == QSV_PARAM_BAD_NAME )
            hb_log( "QSV options: Unknown suboption %s", entry->key );
        else
        if( ret == QSV_PARAM_BAD_VALUE )
            hb_log( "QSV options: Bad argument %s=%s", entry->key, entry->value ? entry->value : "(null)" );
    }

    qsv_encode->m_mfxVideoParam.mfx.TargetUsage     = pv->qsv_config.target_usage;
    qsv_encode->m_mfxVideoParam.mfx.CodecId         = MFX_CODEC_AVC;
    qsv_encode->m_mfxVideoParam.mfx.CodecProfile    = MFX_PROFILE_AVC_HIGH;
    qsv_encode->m_mfxVideoParam.mfx.CodecLevel      = pv->codec_level;

    if(pv->codec_profile>=0)
        qsv_encode->m_mfxVideoParam.mfx.CodecProfile = pv->codec_profile;

    qsv_encode->m_mfxVideoParam.mfx.TargetKbps              = 2000;
    qsv_encode->m_mfxVideoParam.mfx.RateControlMethod       = MFX_RATECONTROL_VBR;

    if (job->vquality >= 0)
    {
        int cqp_offset_i,cqp_offset_p,cqp_offset_b;
        int is_enforcing = 0;

        if ((entry = hb_dict_get(qsv_opts_dict, QSV_NAME_cqp_offset_i)) != NULL && entry->value != NULL)
        {
            cqp_offset_i = atoi(entry->value);
        }
        else
            cqp_offset_i = 0;
         if ((entry = hb_dict_get(qsv_opts_dict, QSV_NAME_cqp_offset_p)) != NULL && entry->value != NULL)
        {
            cqp_offset_p = atoi(entry->value);
        }
        else
            cqp_offset_p = 2;
        if ((entry = hb_dict_get(qsv_opts_dict, QSV_NAME_cqp_offset_b)) != NULL && entry->value != NULL)
        {
            cqp_offset_b = atoi(entry->value);
            is_enforcing = 1;
        }
        else
            cqp_offset_b = 4;

        if( pv->qsv_config.gop_ref_dist == 4 && !is_enforcing )
        {
            cqp_offset_b = cqp_offset_p;
        }
        qsv_encode->m_mfxVideoParam.mfx.RateControlMethod   = MFX_RATECONTROL_CQP;
        qsv_encode->m_mfxVideoParam.mfx.QPI                 = job->vquality + cqp_offset_i;
        qsv_encode->m_mfxVideoParam.mfx.QPP                 = job->vquality + cqp_offset_p;
        qsv_encode->m_mfxVideoParam.mfx.QPB                 = job->vquality + cqp_offset_b;
    }
    else if (job->vbitrate > 0)
    {
        // note: if there is a speed or quality difference,
        //       we could set MFX_RATECONTROL_AVBR here and
        //       set MFX_RATECONTROL_VBR in one of the clauses below
        //
        qsv_encode->m_mfxVideoParam.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
        qsv_encode->m_mfxVideoParam.mfx.TargetKbps        = job->vbitrate;

        // make it work like x264 for ease of use
        // (more convenient if switching back and forth)
        //
        // see http://mewiki.project357.com/wiki/X264_Settings#vbv-maxrate
        //
        if (((entry = hb_dict_get(qsv_opts_dict, QSV_NAME_vbv_bufsize)) != NULL && entry->value != NULL) ||
            (job->h264_level != NULL && !strcasecmp(job->h264_level, "auto")))
        {
            qsv_encode->m_mfxVideoParam.mfx.BufferSizeInKB = atoi(entry->value) / 8;
        }
        if (((entry = hb_dict_get(qsv_opts_dict, QSV_NAME_vbv_maxrate)) != NULL && entry->value != NULL) ||
            (job->h264_level != NULL && !strcasecmp(job->h264_level, "auto")))
        {
            if (job->vbitrate == atoi(entry->value))
            {
                qsv_encode->m_mfxVideoParam.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
            }
            else
            {
                qsv_encode->m_mfxVideoParam.mfx.MaxKbps = atoi(entry->value);
            }
        }
        if ((entry = hb_dict_get(qsv_opts_dict, QSV_NAME_vbv_init)) != NULL && entry->value != NULL)
        {
            if (atof(entry->value) < 1)
            {
                // FIXME: we may not know BufferSizeInKB here,
                //        depending on what was set above?
                // maybe just forget about this special case
                qsv_encode->m_mfxVideoParam.mfx.InitialDelayInKB = atof(entry->value) * qsv_encode->m_mfxVideoParam.mfx.BufferSizeInKB;
            }
            else
            {
                qsv_encode->m_mfxVideoParam.mfx.InitialDelayInKB = atoi(entry->value) / 8;
            }
        }
    }
    else
    {
        hb_error("wrong parameters setting");
    }

    // version-specific encoder options
    if (hb_qsv_info->features & HB_QSV_FEATURE_CODEC_OPTIONS_2)
    {
        if ((entry = hb_dict_get(qsv_opts_dict, QSV_NAME_mbbrc)) != NULL && entry->value != NULL)
        {
            pv->mbbrc =atoi(entry->value);
        }
        else
            pv->mbbrc = 1; // MFX_CODINGOPTION_ON

        if ((entry = hb_dict_get(qsv_opts_dict, QSV_NAME_extbrc)) != NULL && entry->value != NULL)
        {
            pv->extbrc =atoi(entry->value);
        }
        else
            pv->extbrc = 2; //MFX_CODINGOPTION_OFF
    }

    hb_dict_free( &qsv_opts_dict );

    if(pv->qsv_config.async_depth)
        qsv_encode->m_mfxVideoParam.AsyncDepth = pv->qsv_config.async_depth;

    pv->max_async_depth = pv->qsv_config.async_depth;
    pv->async_depth     = 0;

    char *rc_method = NULL;
    switch (qsv_encode->m_mfxVideoParam.mfx.RateControlMethod){
        case MFX_RATECONTROL_CBR    : rc_method = "MFX_RATECONTROL_CBR";
                                      break;
        case MFX_RATECONTROL_AVBR   : rc_method = "MFX_RATECONTROL_AVBR";
                                      break;
        case MFX_RATECONTROL_VBR    : rc_method = "MFX_RATECONTROL_VBR";
                                      break;
        case MFX_RATECONTROL_CQP    : rc_method = "MFX_RATECONTROL_CQP";
                                      break;
        default                     : rc_method = "unknown";
    };

    if( qsv_encode->m_mfxVideoParam.mfx.RateControlMethod == MFX_RATECONTROL_CQP )
        hb_log("qsv: RateControlMethod:%s(I:%d/P:%d/B:%d)",rc_method,
                                                qsv_encode->m_mfxVideoParam.mfx.QPI, qsv_encode->m_mfxVideoParam.mfx.QPP, qsv_encode->m_mfxVideoParam.mfx.QPB );
    else
        hb_log("qsv: RateControlMethod:%s TargetKbps:%d",rc_method , qsv_encode->m_mfxVideoParam.mfx.TargetKbps );

    hb_log("qsv: TargetUsage:%d AsyncDepth:%d", qsv_encode->m_mfxVideoParam.mfx.TargetUsage,qsv_encode->m_mfxVideoParam.AsyncDepth);
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.FrameRateExtN    = job->vrate;
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.FrameRateExtD    = job->vrate_base;
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.AspectRatioW     = job->anamorphic.par_width;
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.AspectRatioH     = job->anamorphic.par_height;
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.FourCC           = MFX_FOURCC_NV12;
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.ChromaFormat     = MFX_CHROMAFORMAT_YUV420;
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.PicStruct        = MFX_PICSTRUCT_PROGRESSIVE;
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.CropX            = 0;
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.CropY            = 0;
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.CropW            = job->width;
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.CropH            = job->height;

    qsv_encode->m_mfxVideoParam.mfx.GopRefDist                 = pv->qsv_config.gop_ref_dist;
    qsv_encode->m_mfxVideoParam.mfx.GopPicSize                 = pv->qsv_config.gop_pic_size;
    qsv_encode->m_mfxVideoParam.mfx.NumRefFrame                = pv->qsv_config.num_ref_frame;

    hb_log("qsv: GopRefDist:%d GopPicSize:%d NumRefFrame:%d",
       qsv_encode->m_mfxVideoParam.mfx.GopRefDist,
       qsv_encode->m_mfxVideoParam.mfx.GopPicSize,
       qsv_encode->m_mfxVideoParam.mfx.NumRefFrame);

    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of progressive
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.Width  =  AV_QSV_ALIGN16(qsv_encode->m_mfxVideoParam.mfx.FrameInfo.CropW);
    qsv_encode->m_mfxVideoParam.mfx.FrameInfo.Height = (MFX_PICSTRUCT_PROGRESSIVE == qsv_encode->m_mfxVideoParam.mfx.FrameInfo.PicStruct)?
                                                            AV_QSV_ALIGN16(qsv_encode->m_mfxVideoParam.mfx.FrameInfo.CropH) :
                                                            AV_QSV_ALIGN32(qsv_encode->m_mfxVideoParam.mfx.FrameInfo.CropH);

    if(pv->is_sys_mem)
        qsv_encode->m_mfxVideoParam.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    else
        qsv_encode->m_mfxVideoParam.IOPattern = MFX_IOPATTERN_IN_OPAQUE_MEMORY;

    // if not explicit value given - making atleast one
    int tasks_amount = pv->max_async_depth ? pv->max_async_depth : 1;
    qsv_encode->tasks = av_qsv_list_init(HAVE_THREADS);
    qsv_encode->p_buf_max_size = AV_QSV_BUF_SIZE_DEFAULT;

    for (i = 0; i < tasks_amount; i++){
            av_qsv_task* task = av_mallocz(sizeof(av_qsv_task));
            task->bs = av_mallocz(sizeof(mfxBitstream));
            task->bs->Data  = av_mallocz(qsv_encode->p_buf_max_size*sizeof(uint8_t));
            task->bs->DataLength    = 0;
            task->bs->DataOffset = 0;
            task->bs->MaxLength = qsv_encode->p_buf_max_size;
            av_qsv_list_add( qsv_encode->tasks, task );
    }

    // allocation of surfaces to work with
    memset(&qsv_encode->request, 0, sizeof(mfxFrameAllocRequest)*2);
    sts = MFXVideoENCODE_QueryIOSurf(qsv->mfx_session, &qsv_encode->m_mfxVideoParam, &qsv_encode->request);
    AV_QSV_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    if(pv->is_sys_mem){

        qsv_encode->surface_num = FFMIN( qsv_encode->request[0].NumFrameSuggested + pv->qsv_config.async_depth,AV_QSV_SURFACE_NUM );
        if(qsv_encode->surface_num <= 0 )
            qsv_encode->surface_num = AV_QSV_SURFACE_NUM;

        for (i = 0; i < qsv_encode->surface_num; i++){
            qsv_encode->p_surfaces[i] = av_mallocz( sizeof(mfxFrameSurface1) );
            AV_QSV_CHECK_POINTER(qsv_encode->p_surfaces[i], MFX_ERR_MEMORY_ALLOC);
            memcpy(&(qsv_encode->p_surfaces[i]->Info), &(qsv_encode->request[0].Info), sizeof(mfxFrameInfo));
        }
    }

    qsv_encode->sync_num = qsv_encode->surface_num? FFMIN( qsv_encode->surface_num,AV_QSV_SYNC_NUM ) : AV_QSV_SYNC_NUM;
    for (i = 0; i < qsv_encode->sync_num; i++){
        qsv_encode->p_syncp[i] = av_mallocz(sizeof(av_qsv_sync));
        AV_QSV_CHECK_POINTER(qsv_encode->p_syncp[i], MFX_ERR_MEMORY_ALLOC);
        qsv_encode->p_syncp[i]->p_sync = av_mallocz(sizeof(mfxSyncPoint));
        AV_QSV_CHECK_POINTER(qsv_encode->p_syncp[i]->p_sync, MFX_ERR_MEMORY_ALLOC);
    }

    qsv_encode->p_ext_param_num = 0;
    if(!pv->is_sys_mem)
        qsv_encode->p_ext_param_num = 1; // for MFX_EXTBUFF_OPAQUE_SURFACE_ALLOCATION

    if(pv->mbbrc || pv->extbrc){
        qsv_encode->p_ext_param_num++;
    }

    qsv_encode->p_ext_param_num++; // for MFX_EXTBUFF_CODING_OPTION

    qsv_encode->m_mfxVideoParam.NumExtParam = qsv_encode->p_ext_param_num;

    if(qsv_encode->m_mfxVideoParam.NumExtParam){
        int cur_idx = 0;
        qsv_encode->p_ext_params = av_mallocz(sizeof(mfxExtBuffer *)*qsv_encode->p_ext_param_num);
        AV_QSV_CHECK_POINTER(qsv_encode->p_ext_params, MFX_ERR_MEMORY_ALLOC);

        // MFX_EXTBUFF_CODING_OPTION, supported from MSDK API 1.0 , so no additional check for now
        pv->qsv_coding_option_config.Header.BufferId  = MFX_EXTBUFF_CODING_OPTION;
        pv->qsv_coding_option_config.Header.BufferSz  = sizeof(mfxExtCodingOption);
        pv->qsv_coding_option_config.AUDelimiter      = MFX_CODINGOPTION_OFF;
        pv->qsv_coding_option_config.PicTimingSEI     = MFX_CODINGOPTION_OFF;
        qsv_encode->p_ext_params[cur_idx++]            = (mfxExtBuffer*)&pv->qsv_coding_option_config;

        if(!pv->is_sys_mem){
            memset(&qsv_encode->ext_opaque_alloc, 0, sizeof(mfxExtOpaqueSurfaceAlloc));
            qsv_encode->ext_opaque_alloc.Header.BufferId    = MFX_EXTBUFF_OPAQUE_SURFACE_ALLOCATION;
            qsv_encode->ext_opaque_alloc.Header.BufferSz    = sizeof(mfxExtOpaqueSurfaceAlloc);
                qsv_encode->p_ext_params[cur_idx++]             = (mfxExtBuffer*)&qsv_encode->ext_opaque_alloc;

            av_qsv_space *in_space = 0;
            // if we have any VPP , we will use its details , if not - failback to decode's settings
            if(pv->is_vpp_present)
                in_space = av_qsv_list_item(qsv->vpp_space, av_qsv_list_count(qsv->vpp_space)-1);
            else
                in_space = qsv->dec_space;

            qsv_encode->ext_opaque_alloc.In.Surfaces    = in_space->p_surfaces;
            qsv_encode->ext_opaque_alloc.In.NumSurface  = in_space->surface_num;
            qsv_encode->ext_opaque_alloc.In.Type        = qsv_encode->request[0].Type;
        }

        if(pv->mbbrc || pv->extbrc){
            pv->qsv_coding_option2_config.Header.BufferId  = MFX_EXTBUFF_CODING_OPTION2;
            pv->qsv_coding_option2_config.Header.BufferSz  = sizeof(mfxExtCodingOption2);
            pv->qsv_coding_option2_config.MBBRC            = pv->mbbrc << 4;
                                                            // default is off
            pv->qsv_coding_option2_config.ExtBRC           = pv->extbrc == 0 ? MFX_CODINGOPTION_OFF : pv->extbrc << 4;

            // reset if out of the ranges
            if(pv->qsv_coding_option2_config.MBBRC > MFX_CODINGOPTION_ADAPTIVE)
                pv->qsv_coding_option2_config.MBBRC = MFX_CODINGOPTION_UNKNOWN;
            if(pv->qsv_coding_option2_config.ExtBRC > MFX_CODINGOPTION_ADAPTIVE)
                pv->qsv_coding_option2_config.ExtBRC = MFX_CODINGOPTION_OFF;

            qsv_encode->p_ext_params[cur_idx++]    = (mfxExtBuffer*)&pv->qsv_coding_option2_config;
            pv->mbbrx_param_idx = cur_idx;
        }
    }
    qsv_encode->m_mfxVideoParam.ExtParam           = qsv_encode->p_ext_params;

    for(;;){
    sts = MFXVideoENCODE_Init(qsv->mfx_session, &qsv_encode->m_mfxVideoParam);
        // if not supported first time we will reset
        if( sts < MFX_ERR_NONE ){
            if( pv->mbbrx_param_idx ){
                pv->qsv_coding_option2_config.MBBRC     = MFX_CODINGOPTION_UNKNOWN;
                pv->qsv_coding_option2_config.ExtBRC    = MFX_CODINGOPTION_OFF;
                    pv->mbbrx_param_idx = 0;
            }
        }
        else
            break;
    }
    AV_QSV_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // SPS/PPS retrieval with NAL prefix: 00 00 00 01 for each
    mfxVideoParam               videoParam;
    memset(&videoParam, 0, sizeof(mfxVideoParam));
    pv->sps_pps                  = av_mallocz(sizeof(mfxExtCodingOptionSPSPPS));
    pv->sps_pps->Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
    pv->sps_pps->PPSId      = 0;
    pv->sps_pps->SPSId      = 0;
    pv->sps_pps->SPSBuffer  = av_mallocz(SPSPPS_SIZE);
    pv->sps_pps->SPSBufSize = SPSPPS_SIZE;
    pv->sps_pps->PPSBuffer  = av_mallocz(SPSPPS_SIZE);
    pv->sps_pps->PPSBufSize = SPSPPS_SIZE;

    videoParam.NumExtParam  = 1;
    videoParam.ExtParam     = (mfxExtBuffer**)&pv->sps_pps;

    sts = MFXVideoENCODE_GetVideoParam(qsv->mfx_session, &videoParam);
    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // Sequence Parameter Set
    memcpy(pv->config->h264.sps, pv->sps_pps->SPSBuffer + sizeof(ff_prefix_code), pv->sps_pps->SPSBufSize - sizeof(ff_prefix_code));
    pv->config->h264.sps_length = pv->sps_pps->SPSBufSize - sizeof(ff_prefix_code);

    // Picture Parameter Set
    memcpy(pv->config->h264.pps, pv->sps_pps->PPSBuffer + sizeof(ff_prefix_code), pv->sps_pps->PPSBufSize - sizeof(ff_prefix_code));
    pv->config->h264.pps_length = pv->sps_pps->PPSBufSize - sizeof(ff_prefix_code);

    pv->qsv_config.gop_ref_dist = videoParam.mfx.GopRefDist;

    qsv_encode->is_init_done = 1;

    return 0;
}

/***********************************************************************
 * encqsvInit
 ***********************************************************************
 *
 **********************************************************************/
int encqsvInit( hb_work_object_t * w, hb_job_t * job )
{
    mfxStatus sts = MFX_ERR_NONE;

    hb_work_private_t * pv = calloc( 1, sizeof( hb_work_private_t ) );
    w->private_data = pv;

    pv->delayed_processing = hb_list_init();
    pv->frames_in = 0;
    pv->frames_out = 0;

    pv->job = job;
    pv->config = w->config;
    av_qsv_context *qsv = job->qsv;
    av_qsv_space* qsv_encode = 0;

    pv->codec_level = MFX_LEVEL_AVC_3;
    if(job->h264_level){
        int i = 0;
        for (i = 0; hb_h264_level_names[i]; i++)
        {
            if (job->h264_level && !strcmp(hb_h264_level_names[i], job->h264_level))
            {
                // skipping "auto" for default one
                if(i)
                    pv->codec_level = qsv_h264_levels[i];
                break;
            }
        }
    }

    int profile = PROFILE_HIGH;
    pv->codec_profile = -1;
    if(job->h264_profile){
        profile = profile_string_to_int(job->h264_profile);
        switch(profile){
            case PROFILE_BASELINE:  pv->codec_profile = MFX_PROFILE_AVC_BASELINE;
                                    break;
            case PROFILE_MAIN:      pv->codec_profile = MFX_PROFILE_AVC_MAIN;
                                    break;
            case PROFILE_HIGH:      pv->codec_profile = MFX_PROFILE_AVC_HIGH;
                                    break;
            default:                pv->codec_profile = -1;
        }
    }

    // tbd make it very properly
    w->config->h264.sps[1] = profile;
    w->config->h264.sps[2] = 0;
    w->config->h264.sps[3] = pv->codec_level;
    w->config->h264.sps_length = 0;
    w->config->h264.pps_length = 0;

    pv->is_vpp_present = 0;

    // FIXME: please let this be temporary
    // note: MKV only has PTS so it's unaffected
    if ((job->mux & HB_MUX_MASK_MP4)  &&
        (profile != PROFILE_BASELINE) &&
        (hb_qsv_info->features & HB_QSV_FEATURE_DECODE_TIMESTAMPS) == 0)
    {
        hb_error("encqsvInit: MediaSDK < 1.6, unsupported feature B-frames in MP4 container;"
                 " please update your driver, use the MKV container or Baseline profile");
        *job->die = 1;
        return -1;
    }

    return 0;
}

void encqsvClose( hb_work_object_t * w )
{
    int i = 0;
    hb_work_private_t * pv = w->private_data;

    hb_log( "enc_qsv done: frames: %u in, %u out", pv->frames_in, pv->frames_out );

    // if system memory ( encode only ) additional free(s) for surfaces
    if( pv && pv->job && pv->job->qsv &&
        pv->job->qsv->is_context_active ){

        av_qsv_context *qsv = pv->job->qsv;

        if(qsv && qsv->enc_space){
        av_qsv_space* qsv_encode = qsv->enc_space;
        if(qsv_encode->is_init_done){
            if(pv->is_sys_mem){
                if( qsv_encode && qsv_encode->surface_num > 0)
                    for (i = 0; i < qsv_encode->surface_num; i++){
                        if( qsv_encode->p_surfaces[i]->Data.Y){
                            free(qsv_encode->p_surfaces[i]->Data.Y);
                            qsv_encode->p_surfaces[i]->Data.Y = 0;
                        }
                        if( qsv_encode->p_surfaces[i]->Data.VU){
                            free(qsv_encode->p_surfaces[i]->Data.VU);
                            qsv_encode->p_surfaces[i]->Data.VU = 0;
                        }
                        if(qsv_encode->p_surfaces[i])
                            av_freep(qsv_encode->p_surfaces[i]);
                    }
                qsv_encode->surface_num = 0;

                sws_freeContext(pv->sws_context_to_nv12);
            }

            for (i = av_qsv_list_count(qsv_encode->tasks); i > 1; i--){
                av_qsv_task* task = av_qsv_list_item(qsv_encode->tasks,i-1);
                if(task && task->bs){
                    av_freep(&task->bs->Data);
                    av_freep(&task->bs);
                    av_qsv_list_rem(qsv_encode->tasks,task);
                }
            }
            av_qsv_list_close(&qsv_encode->tasks);

            for (i = 0; i < qsv_encode->surface_num; i++){
                av_freep(&qsv_encode->p_surfaces[i]);
            }
            qsv_encode->surface_num = 0;

                if( qsv_encode->p_ext_param_num || qsv_encode->p_ext_params )
                    av_freep(&qsv_encode->p_ext_params);

            for (i = 0; i < qsv_encode->sync_num; i++){
                    av_freep(&qsv_encode->p_syncp[i]->p_sync);
                    av_freep(&qsv_encode->p_syncp[i]);
            }
            qsv_encode->sync_num = 0;

                qsv_encode->is_init_done = 0;
        }
        }
        if(qsv->enc_space)
        av_freep(&qsv->enc_space);

        if(qsv){
        // closing the commong stuff
        av_qsv_context_clean(qsv);

        if(pv->is_sys_mem){
            av_freep(&qsv);
        }
        }

        av_freep(&pv->sps_pps->SPSBuffer);
        av_freep(&pv->sps_pps->PPSBuffer);
        av_freep(&pv->sps_pps);
    }

    free( pv );
    w->private_data = NULL;
}

int encqsvWork( hb_work_object_t * w, hb_buffer_t ** buf_in,
                  hb_buffer_t ** buf_out )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t * job = pv->job;
    hb_buffer_t * in = *buf_in, *buf;
    av_qsv_context *qsv = job->qsv;
    av_qsv_space* qsv_encode;
    hb_buffer_t *last_buf = NULL;
    mfxStatus sts = MFX_ERR_NONE;
    int is_end = 0;
    av_qsv_list* received_item = 0;
    av_qsv_stage* stage = 0;

    while(1){
        int ret = qsv_enc_init(qsv, pv);
        qsv = job->qsv;
        qsv_encode = qsv->enc_space;
        if(ret >= 2)
            av_qsv_sleep(1);
        else
            break;
    }
    *buf_out = NULL;

    if( in->size <= 0 )
    {
        // do delayed frames yet
        *buf_in = NULL;
        is_end = 1;
    }

    // input from decode, as called - we always have some to proceed with
    while( 1 ){

    {
        mfxFrameSurface1 *work_surface = NULL;

        if(!is_end) {
            if(pv->is_sys_mem){
                int surface_idx = av_qsv_get_free_surface(qsv_encode, qsv, &qsv_encode->request[0].Info, QSV_PART_ANY);
                work_surface = qsv_encode->p_surfaces[surface_idx];

                if(!work_surface->Data.Y){
                    // if nv12 and 422 12bits per pixel
                    work_surface->Data.Y = calloc( 1, qsv_encode->m_mfxVideoParam.mfx.FrameInfo.Width *  qsv_encode->m_mfxVideoParam.mfx.FrameInfo.Height );
                    work_surface->Data.VU = calloc( 1, qsv_encode->m_mfxVideoParam.mfx.FrameInfo.Width *  qsv_encode->m_mfxVideoParam.mfx.FrameInfo.Height /2);
                    work_surface->Data.Pitch = qsv_encode->m_mfxVideoParam.mfx.FrameInfo.Width;
                }
                work_surface->Data.TimeStamp = in->s.start;
                av_qsv_dts_ordered_insert(qsv, 0, 0, work_surface->Data.TimeStamp ,0 );
                qsv_yuv420_to_nv12(pv->sws_context_to_nv12,work_surface,in);
            }
            else{
                received_item = in->qsv_details.qsv_atom;
                stage = av_qsv_get_last_stage( received_item );
                work_surface = stage->out.p_surface;
            }
        }
        else{
            work_surface = NULL;
            received_item = NULL;
        }
        int sync_idx = av_qsv_get_free_sync( qsv_encode, qsv );

        av_qsv_task *task = av_qsv_list_item( qsv_encode->tasks, pv->async_depth );

        for (;;)
        {
            // Encode a frame asychronously (returns immediately)
            sts = MFXVideoENCODE_EncodeFrameAsync(qsv->mfx_session, NULL, work_surface, task->bs, qsv_encode->p_syncp[sync_idx]->p_sync );

            if (MFX_ERR_MORE_DATA == sts || (MFX_ERR_NONE <= sts && MFX_WRN_DEVICE_BUSY != sts))
                if (work_surface && !pv->is_sys_mem)
                    ff_qsv_atomic_dec(&work_surface->Data.Locked);

            if( MFX_ERR_MORE_DATA == sts ){
                ff_qsv_atomic_dec(&qsv_encode->p_syncp[sync_idx]->in_use);
                if(work_surface && received_item)
                    hb_list_add(pv->delayed_processing, received_item);
                break;
            }

            AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

            if (MFX_ERR_NONE <= sts /*&& !syncpE*/) // repeat the call if warning and no output
            {
                if (MFX_WRN_DEVICE_BUSY == sts){
                    av_qsv_sleep(10); // wait if device is busy
                    continue;
                }

                av_qsv_stage* new_stage = av_qsv_stage_init();
                new_stage->type = AV_QSV_ENCODE;
                new_stage->in.p_surface = work_surface;
                new_stage->out.sync = qsv_encode->p_syncp[sync_idx];

                new_stage->out.p_bs = task->bs;//qsv_encode->bs;
                task->stage = new_stage;

                pv->async_depth++;

                if(received_item){
                    av_qsv_add_stagee( &received_item, new_stage,HAVE_THREADS );
                }
                else{
                   // flushing the end
                    int pipe_idx = av_qsv_list_add( qsv->pipes, av_qsv_list_init(HAVE_THREADS) );
                    av_qsv_list* list_item = av_qsv_list_item( qsv->pipes, pipe_idx );
                    av_qsv_add_stagee( &list_item, new_stage,HAVE_THREADS );
                }

                int i = 0;
                for(i=hb_list_count(pv->delayed_processing); i > 0;i--){
                    hb_list_t *item = hb_list_item(pv->delayed_processing,i-1);
                    if(item){
                        hb_list_rem(pv->delayed_processing,item);
                        av_qsv_flush_stages(qsv->pipes, &item);
                    }
                }

                break;
            }

            ff_qsv_atomic_dec(&qsv_encode->p_syncp[sync_idx]->in_use);

            if (MFX_ERR_NOT_ENOUGH_BUFFER == sts)
                DEBUG_ASSERT( 1,"The bitstream buffer size is insufficient." );

            break;
        }
    }

    buf = NULL;

    do{

    if(pv->async_depth==0) break;

    // working properly with sync depth approach of MediaSDK OR flushing, if at the end
    if( (pv->async_depth >= pv->max_async_depth) || is_end ){

        pv->async_depth--;

        av_qsv_task *task = av_qsv_list_item( qsv_encode->tasks, 0 );
        av_qsv_stage* stage = task->stage;
        av_qsv_list*  this_pipe = av_qsv_pipe_by_stage(qsv->pipes,stage);
        sts = MFX_ERR_NONE;

        // only here we need to wait on operation been completed, therefore SyncOperation is used,
        // after this step - we continue to work with bitstream, muxing ...
        av_qsv_wait_on_sync( qsv,stage );

        if(task->bs->DataLength>0){
                av_qsv_flush_stages( qsv->pipes, &this_pipe );

                // see nal_encode
                buf = hb_video_buffer_init( job->width, job->height );
                buf->size = 0;
                buf->s.frametype = 0;

                // maping of FrameType(s)
                if(task->bs->FrameType & MFX_FRAMETYPE_IDR ) buf->s.frametype = HB_FRAME_IDR;
                else
                if(task->bs->FrameType & MFX_FRAMETYPE_I )   buf->s.frametype = HB_FRAME_I;
                else
                if(task->bs->FrameType & MFX_FRAMETYPE_P )   buf->s.frametype = HB_FRAME_P;
                else
                if(task->bs->FrameType & MFX_FRAMETYPE_B )   buf->s.frametype = HB_FRAME_B;

                if(task->bs->FrameType & MFX_FRAMETYPE_REF ) buf->s.flags      = HB_FRAME_REF;

                parse_nalus(task->bs->Data + task->bs->DataOffset,task->bs->DataLength, buf, pv->frames_out);

                if ( last_buf == NULL )
                    *buf_out = buf;
                else
                    last_buf->next = buf;
                last_buf = buf;

                // simple for now but check on TimeStampCalc from MSDK
                int64_t duration  = ((double)qsv_encode->m_mfxVideoParam.mfx.FrameInfo.FrameRateExtD /
                                     (double)qsv_encode->m_mfxVideoParam.mfx.FrameInfo.FrameRateExtN) * 90000.;

                // start        -> PTS
                // renderOffset -> DTS
                buf->s.start = buf->s.renderOffset = task->bs->TimeStamp;
                buf->s.stop  = buf->s.start + duration;
                if (hb_qsv_info->features & HB_QSV_FEATURE_DECODE_TIMESTAMPS)
                    buf->s.renderOffset = task->bs->DecodeTimeStamp;

                if(pv->qsv_config.gop_ref_dist > 1)
                    pv->qsv_config.gop_ref_dist--;
                else{
                    av_qsv_dts_pop(qsv);
                }

                // shift for fifo
                if(pv->async_depth){
                    av_qsv_list_rem(qsv_encode->tasks,task);
                    av_qsv_list_add(qsv_encode->tasks,task);
                }

                task->bs->DataLength    = 0;
                task->bs->DataOffset    = 0;
                task->bs->MaxLength = qsv_encode->p_buf_max_size;
                task->stage        = 0;
                pv->frames_out++;
            }
        }
    }while(is_end);


    if(is_end){
        if( !buf && MFX_ERR_MORE_DATA == sts )
            break;

    }
    else
        break;

    }

    if(!is_end)
        ++pv->frames_in;

    if(is_end){
        *buf_in = NULL;
        if(last_buf){
            last_buf->next = in;
        }
        else
            *buf_out = in;
        return HB_WORK_DONE;
    }
    else{
        return HB_WORK_OK;
    }
}

int nal_find_start_code(uint8_t** pb, size_t* size){
    if ((int) *size < 4 )
        return 0;

    // find start code by MSDK , see ff_prefix_code[]
    while ((4 <= *size) &&
        ((0 != (*pb)[0]) ||
         (0 != (*pb)[1]) ||
         (1 != (*pb)[2]) ))
    {
        *pb += 1;
        *size -= 1;
    }

    if (4 <= *size)
        return (((*pb)[0] << 24) | ((*pb)[1] << 16) | ((*pb)[2] << 8) | ((*pb)[3]));

    return 0;
}

void parse_nalus(uint8_t *nal_inits, size_t length, hb_buffer_t *buf, uint32_t frame_num){
    uint8_t *offset = nal_inits;
    size_t size     = length;

    if( nal_find_start_code(&offset,&size) == 0 )
        size = 0;

    while( size > 0 ){

            uint8_t* current_nal = offset + sizeof(ff_prefix_code)-1;
            int nal_ref_idc= current_nal[0]>>5;

            uint8_t *next_offset = offset + sizeof(ff_prefix_code);
            size_t next_size     = size - sizeof(ff_prefix_code);
            size_t current_size  = next_size;
            if( nal_find_start_code(&next_offset,&next_size) == 0 ){
                size = 0;
                current_size += 1;
            }
            else{
                current_size -= next_size;
                if( next_offset > 0 && *(next_offset-1) != 0  )
                    current_size += 1;
            }
            {
                char size_position[4] = {0,0,0,0};
                size_position[1] = (current_size >> 24) & 0xFF;
                size_position[1] = (current_size >> 16) & 0xFF;
                size_position[2] = (current_size >> 8)  & 0xFF;
                size_position[3] =  current_size        & 0xFF;

                memcpy(buf->data + buf->size,&size_position ,sizeof(size_position));
                buf->size += sizeof(size_position);

                memcpy(buf->data + buf->size,current_nal ,current_size);
                buf->size += current_size;
            }

            if(size){
                size   = next_size;
                offset = next_offset;
            }
        }
}

int qsv_param_parse( av_qsv_config* config, const char *name, const char *value){
    int ret = QSV_PARAM_OK;

    if(!config)
        return QSV_PARAM_BAD_CONFIG;

    if(!strcmp(name,QSV_NAME_async_depth))
        config->async_depth = FFMAX( atoi(value),0 );
    else
    if(!strcmp(name,QSV_NAME_target_usage))
        config->target_usage = FFMAX( atoi(value),0 );
    else
    if(!strcmp(name,QSV_NAME_num_ref_frame))
        config->num_ref_frame = FFMAX( atoi(value),0 );
    else
    if(!strcmp(name,QSV_NAME_gop_ref_dist))
        config->gop_ref_dist = FFMAX( FFMIN(atoi(value),32),0 );
    else
    if(!strcmp(name,QSV_NAME_gop_pic_size))
        config->gop_pic_size = FFMAX(atoi(value),0);
    else
    if(!strcmp(name,QSV_NAME_vbv_bufsize) ||
       !strcmp(name,QSV_NAME_vbv_maxrate) ||
       !strcmp(name,QSV_NAME_vbv_init)    ||
       !strcmp(name,QSV_NAME_mbbrc)       ||
       !strcmp(name,QSV_NAME_extbrc)      ||
       !strcmp(name,QSV_NAME_cqp_offset_i)      ||
       !strcmp(name,QSV_NAME_cqp_offset_p)      ||
       !strcmp(name,QSV_NAME_cqp_offset_b)
       )
        ret = QSV_PARAM_OK;
    else
        ret = QSV_PARAM_BAD_NAME;

    return ret;
}

void qsv_param_set_defaults( av_qsv_config* config, hb_qsv_info_t *qsv_info ){
    if(!config)
        return;

    config->async_depth     = AV_QSV_ASYNC_DEPTH_DEFAULT;
    config->target_usage    = MFX_TARGETUSAGE_BEST_QUALITY + 1;
    config->gop_ref_dist    = 4;
    config->gop_pic_size    = 32;
}
