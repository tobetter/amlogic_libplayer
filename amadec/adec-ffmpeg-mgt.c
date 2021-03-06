/*
 * Copyright (C) 2010 Amlogic Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <dlfcn.h>
#include <errno.h>

#include <adec-pts-mgt.h>
#include <adec_write.h>
#include <Amsysfsutils.h>
#include <audio-dec.h>
#include <amthreadpool.h>
#ifdef ANDROID
#include <adec_omx_brige.h>
#include <cutils/properties.h>
#endif
#include <adec_assoc_audio.h>

#define HEADER_LENGTH_AFTER_IEC61937 0x4
static char UDCInOutMix[] = "media.udc.inoutmix";

extern int read_buffer(unsigned char *buffer, int size);
void *audio_decode_loop(void *args);
void *audio_dtsdecode_loop(void *args);
void *audio_getpackage_loop(void *args);
static int set_sysfs_int(const char *path, int val);
static void stop_decode_thread(aml_audio_dec_t *audec);

/*audio decoder list structure*/
typedef struct {
    //enum CodecID codec_id;
    int codec_id;
    char    name[64];
} audio_lib_t;

audio_lib_t audio_lib_list[] = {
    {ACODEC_FMT_AAC, "libfaad.so"},
    {ACODEC_FMT_AAC_LATM, "libfaad.so"},
    {ACODEC_FMT_APE, "libape.so"},
    {ACODEC_FMT_MPEG, "libammad.so"},
    {ACODEC_FMT_MPEG2, "libammad.so"},
    {ACODEC_FMT_MPEG1, "libammad.so"},
    {ACODEC_FMT_FLAC, "libflac.so"},
    {ACODEC_FMT_COOK, "libcook.so"},
    {ACODEC_FMT_RAAC, "libraac.so"},
    {ACODEC_FMT_AMR, "libamr.so"},

    {ACODEC_FMT_PCM_S16BE, "libpcm.so"},
    {ACODEC_FMT_PCM_S16LE, "libpcm.so"},
    {ACODEC_FMT_PCM_U8, "libpcm.so"},
    {ACODEC_FMT_PCM_BLURAY, "libpcm.so"},
    {ACODEC_FMT_WIFIDISPLAY, "libpcm.so"},
    {ACODEC_FMT_ALAW, "libpcm.so"},
    {ACODEC_FMT_MULAW, "libpcm.so"},
    {ACODEC_FMT_ADPCM, "libadpcm.so"},
    {ACODEC_FMT_DRA, "libdra.so"},
#ifndef ANDROID
    {ACODEC_FMT_VORBIS, "libamffmpegdec.so"},
    {ACODEC_FMT_WMA, "libamffmpegdec.so"},
    {ACODEC_FMT_WMAPRO, "libamffmpegdec.so"},
    {ACODEC_FMT_AC3,"libdcv.so"},
    {ACODEC_FMT_EAC3,"libdcv.so"},
    {ACODEC_FMT_DTS,"libdtscore.so"},
#endif
    {ACODEC_FMT_OPUS, "libopus.so"},
    NULL
} ;


int find_audio_lib(aml_audio_dec_t *audec)
{
    int i;
    int num;
    audio_lib_t *f;
    void *fd = NULL;
    adec_print("[%s %d]audec->format/%d audec->codec_id/0x%x\n", __FUNCTION__, __LINE__, audec->format, audec->codec_id);
    num = ARRAY_SIZE(audio_lib_list);
    audio_decoder_operations_t *adec_ops = audec->adec_ops;
    //-------------------------
	#ifdef ANDROID
    if (find_omx_lib(audec)) {
        return 0;
    }
	#endif
    //-----------------------
    for (i = 0; i < num; i++) {
        f = &audio_lib_list[i];
        if (f->codec_id == audec->format) {
#ifndef ANDROID
            if (audec->format == ACODEC_FMT_FLAC) {
                fd = dlopen("libavcodec.so", RTLD_NOW | RTLD_GLOBAL);
                if (!fd) {
                    adec_print("cant load libavcodec.so (%s)\n", dlerror());
                }
            }
#endif
            fd = dlopen(audio_lib_list[i].name, RTLD_NOW);
            if (fd != NULL) {
                adec_ops->init    = dlsym(fd, "audio_dec_init");
                adec_ops->decode  = dlsym(fd, "audio_dec_decode");
                adec_ops->release = dlsym(fd, "audio_dec_release");
                adec_ops->getinfo = dlsym(fd, "audio_dec_getinfo");
                adec_print("audec->format/%d decoder load success \n", audec->format);
            } else {
                adec_print("cant find decoder lib %s\n", dlerror());
                return -1;
            }
            return 0;
        }
    }
    return -1;
}



static int FFmpegDecoderInit(audio_decoder_operations_t *adec_ops)
{
    return 0;
}
static int FFmpegDecode(audio_decoder_operations_t *adec_ops, char *outbuf, int *outlen, char *inbuf, int inlen)
{
    int ret;
    return ret;
}
static int FFmpegDecoderRelease(audio_decoder_operations_t *adec_ops)
{
    aml_audio_dec_t *audec = (aml_audio_dec_t *)(adec_ops->priv_data);
    return 0;
}
audio_decoder_operations_t AudioFFmpegDecoder = {
    .name = "FFmpegDecoder",
    .nAudioDecoderType = AUDIO_FFMPEG_DECODER,
    .init = FFmpegDecoderInit,
    .decode = FFmpegDecode,
    .release = FFmpegDecoderRelease,
    .getinfo = NULL,
};

int package_list_free(aml_audio_dec_t * audec)
{
    lp_lock(&(audec->pack_list.tslock));
    while (audec->pack_list.pack_num) {
        struct package * p = audec->pack_list.first;
        audec->pack_list.first = audec->pack_list.first->next;
        free(p->data);
        free(p);
        audec->pack_list.pack_num--;
    }
    lp_unlock(&(audec->pack_list.tslock));
    return 0;
}

int package_list_init(aml_audio_dec_t * audec)
{
    audec->pack_list.first = NULL;
    audec->pack_list.pack_num = 0;
    audec->pack_list.current = NULL;
    lp_lock_init(&(audec->pack_list.tslock), NULL);
    return 0;
}

int package_add(aml_audio_dec_t * audec, char * data, int size)
{
    lp_lock(&(audec->pack_list.tslock));
    if (audec->pack_list.pack_num == 4) { //enough
        lp_unlock(&(audec->pack_list.tslock));
        return -2;
    }
    struct package *p = malloc(sizeof(struct package));
    if (!p) { //malloc failed
        lp_unlock(&(audec->pack_list.tslock));
        return -1;
    }
    p->data = data;
    p->size = size;
    if (audec->pack_list.pack_num == 0) { //first package
        audec->pack_list.first = p;
        audec->pack_list.current = p;
        audec->pack_list.pack_num = 1;
    } else {
        audec->pack_list.current->next = p;
        audec->pack_list.current = p;
        audec->pack_list.pack_num++;
    }
    lp_unlock(&(audec->pack_list.tslock));
    return 0;
}

struct package * package_get(aml_audio_dec_t * audec) {
    lp_lock(&(audec->pack_list.tslock));
    if (audec->pack_list.pack_num == 0) {
        lp_unlock(&(audec->pack_list.tslock));
        return NULL;
    }
    struct package *p = audec->pack_list.first;
    if (audec->pack_list.pack_num == 1) {
        audec->pack_list.first = NULL;
        audec->pack_list.pack_num = 0;
        audec->pack_list.current = NULL;
    } else if (audec->pack_list.pack_num > 1) {
        audec->pack_list.first = audec->pack_list.first->next;
        audec->pack_list.pack_num--;
    }
    lp_unlock(&(audec->pack_list.tslock));
    return p;
}


int armdec_stream_read(dsp_operations_t *dsp_ops, char *buffer, int size)
{
    int read_size = 0;
    aml_audio_dec_t *audec = (aml_audio_dec_t *)dsp_ops->audec;
    read_size = read_pcm_buffer(buffer, audec->g_bst, size);
    audec->out_len_after_last_valid_pts += read_size;
    return read_size;
}

int armdec_stream_read_raw(dsp_operations_t *dsp_ops, char *buffer, int size)
{
    aml_audio_dec_t *audec = (aml_audio_dec_t *)dsp_ops->audec;
    return read_pcm_buffer(buffer, audec->g_bst_raw, size);
}
unsigned long  armdec_get_pts(dsp_operations_t *dsp_ops)
{
    unsigned long val, offset;
    unsigned long pts;
    int data_width, channels, samplerate;
    unsigned long long frame_nums ;
    unsigned long delay_pts;
    char value[PROPERTY_VALUE_MAX];
    aml_audio_dec_t *audec = (aml_audio_dec_t *)dsp_ops->audec;
    audio_out_operations_t * aout_ops = &audec->aout_ops;
    float  track_speed = 1.0f;
    if (aout_ops->track_rate != 8.8f)
        track_speed = aout_ops->track_rate;
    switch (audec->g_bst->data_width) {
    case AV_SAMPLE_FMT_U8:
        data_width = 8;
        break;
    case AV_SAMPLE_FMT_S16:
        data_width = 16;
        break;
    case AV_SAMPLE_FMT_S32:
        data_width = 32;
        break;
    default:
        data_width = 16;
    }

    int pts_delta = 0;
    if ( property_get("media.libplayer.pts_delta",value,NULL) > 0)
    {
        pts_delta = atoi(value);
    }

    channels = audec->g_bst->channels;
    samplerate = audec->g_bst->samplerate;
    if (!channels || !samplerate) {
        adec_print("warning ::::zero  channels %d, sample rate %d \n", channels, samplerate);
        if (!samplerate) {
            samplerate = 48000;
        }
        if (!channels) {
            channels = 2;
        }
    }
    offset = audec->decode_offset;
    if (dsp_ops->dsp_file_fd >= 0) {
        if (audec->g_bst->format != ACODEC_FMT_COOK && audec->g_bst->format != ACODEC_FMT_RAAC) {
            //when first  look up apts,set offset 0
            if (!audec->first_apts_lookup_over) {
                offset = 0;
            }
            ioctl(dsp_ops->dsp_file_fd, AMSTREAM_IOC_APTS_LOOKUP, &offset);
        }
        //for cook/raac should wait to get first apts from decoder
        else {
            int wait_count = 10;
            while (offset == 0xffffffff && wait_count-- > 0) {
                amthreadpool_thread_usleep(10000);
            }
            offset = audec->decode_offset;
            if (offset == 0xffffffff) {
                adec_print(" cook/raac get apts 100 ms timeout \n");
            }

        }
    }else if (!audec->use_hardabuf) {
            if (get_apts(audec,&offset) != 0)
                offset = 0;
    } else {
        adec_print("====abuf have not open!\n", val);
    }

    if (am_getconfig_bool("media.arm.audio.apts_add")) {
        offset = 0;
    }

    pts = offset;
    if (!audec->first_apts_lookup_over) {
        audec->last_valid_pts = pts;
        audec->first_apts_lookup_over = 1;
        return pts;
    }
    if (pts == 0) {
        if (audec->last_valid_pts) {
            pts = audec->last_valid_pts;
        }
        frame_nums = (audec->out_len_after_last_valid_pts * 8 / (data_width * channels));
        pts += (frame_nums * 90000 / samplerate);
        pts += (90000 / 1000 * pts_delta);
        if (pts < 0)
            pts = 0;
        //adec_print("decode_offset:%d out_pcm:%d   pts:%d \n",audec->decode_offset,audec->out_len_after_last_valid_pts,pts);
        return pts;
    }

    int len = audec->g_bst->buf_level + audec->pcm_cache_size + audec->alsa_cache_size;
    frame_nums = (len * 8 / (data_width * channels));
    delay_pts = frame_nums * 90000 / samplerate;
    delay_pts = delay_pts*track_speed;
    if (pts > delay_pts) {
        pts -= delay_pts;
    } else {
        pts = 0;
    }
    val = pts;
    if (audec->last_valid_pts > pts) {
        adec_print("audec->last_valid_pts:%ld  > pts:%ld \n",audec->last_valid_pts,pts);
        val = pts = audec->last_valid_pts;
        //val = -1;
    }
    audec->last_valid_pts = pts;
    audec->out_len_after_last_valid_pts = 0;
    //adec_print("====get pts:%ld offset:%ld frame_num:%lld delay:%ld \n",val,audec->decode_offset,frame_nums,delay_pts);

    val += 90000 / 1000 * pts_delta; // for a/v sync test,some times audio ahead video +28ms.so add +15ms to apts to .....
    if (val < 0)
        val = 0;
    return val;
}

unsigned long  armdec_get_pcrscr(dsp_operations_t *dsp_ops)
{
    unsigned long val;
    if (dsp_ops->dsp_file_fd < 0) {
        adec_print("read error!! audiodsp have not opened\n");
        return -1;
    }
    ioctl(dsp_ops->dsp_file_fd, AMSTREAM_IOC_PCRSCR, &val);
    return val;
}
unsigned long  armdec_set_pts(dsp_operations_t *dsp_ops, unsigned long apts)
{
    if (dsp_ops->dsp_file_fd < 0) {
        adec_print("armdec_set_apts err!\n");
        return -1;
    }
    ioctl(dsp_ops->dsp_file_fd, AMSTREAM_IOC_SET_APTS, &apts);
    return 0;
}
int armdec_set_skip_bytes(dsp_operations_t* dsp_ops, unsigned int bytes)
{
    return  0;
}
static int set_sysfs_int(const char *path, int val)
{
    return amsysfs_set_sysfs_int(path, val);
}
int acodec_buffer_write(void *p, char* buffer, size_t bytes){
     aml_audio_dec_t *audec = (aml_audio_dec_t *)p;
     int ret = -1;

     ret = buffer_write(&audec->buf,buffer,bytes);
     if (ret > 0) {
       audec->write_offset += bytes;
       return ret;
     } else {
        amthreadpool_thread_usleep(10000);
        errno = EAGAIN;
        return -EAGAIN;
     }
}
int get_abuf_state(void *p,struct buf_status *buf){

    aml_audio_dec_t *audec = (aml_audio_dec_t *)p;

    if (audec->buf.wr >= audec->buf.rd) {
        buf->data_len = audec->buf.wr  - audec->buf.rd;
    } else {
        buf->data_len = audec->buf.size - (audec->buf.rd - audec->buf.wr);
    }
    if (buf->data_len > audec->adec_ops->nInBufSize)
        return 0;
    else
        return -1;
}
int ptsnode_list_free(aml_audio_dec_t * audec)
{
    lp_lock(&(audec->pts_list.tslock));
    while (audec->pts_list.node_num) {
        struct PtsNode * p = audec->pts_list.first;
        audec->pts_list.first = audec->pts_list.first->next;
        free(p);
        audec->pts_list.node_num--;
    }
    lp_unlock(&(audec->pts_list.tslock));
    return 0;
}

int ptsnode_list_init(aml_audio_dec_t * audec)
{
    audec->pts_list.first = NULL;
    audec->pts_list.node_num = 0;
    audec->pts_list.current = NULL;
    lp_lock_init(&(audec->pts_list.tslock), NULL);
    return 0;
}

int checkin_pts(void *p, unsigned long pts){
    aml_audio_dec_t *audec = (aml_audio_dec_t *)p;
    int ret = ptsnode_add(audec,pts,audec->write_offset);
    if ( ret !=  0) {
          adec_print("checkin_pts fail return %d \n",ret);
    }else {
         adec_print("%s offset:%ld pts:%ld \n",__FUNCTION__,audec->write_offset,pts);
    }
     return ret;

}
int acodec_get_apts(void *p,unsigned long *pts){
   aml_audio_dec_t *audec = (aml_audio_dec_t *)p;
   if (audec->pts_list.node_num  == 0)
        return -1;
   *pts = adec_calc_pts(audec);
   //adec_print("acodec_get_apts *pts:%d \n",*pts);
   return 0;
}
int get_apts(aml_audio_dec_t * audec,unsigned long *pts){
    lp_lock(&(audec->pts_list.tslock));
    unsigned long decode_offset = audec->decode_offset;
    PtsNode *ptsnode= audec->pts_list.first;
    while ( ptsnode ) {
     if (ptsnode->write_offset < decode_offset) {
        ptsnode = ptsnode->next;
     } else {
       *pts =  ptsnode->pts;
       //adec_print("ptsnode->write_offset:%ld decode_offset:%ld ptsnode->pts:%ld\n",
       //ptsnode->write_offset,decode_offset,ptsnode->pts);
       lp_unlock(&(audec->pts_list.tslock));
       return 0;
     }
    }
    //adec_print("%s do not find pts offset:%d \n",__FUNCTION__,decode_offset);
    lp_unlock(&(audec->pts_list.tslock));
    return -1;



}

int ptsnode_add(aml_audio_dec_t * audec, unsigned long pts, int64_t offset)
{
    lp_lock(&(audec->pts_list.tslock));
    PtsNode *p = malloc(sizeof(PtsNode));
    if (!p) { //malloc failed
        lp_unlock(&(audec->pts_list.tslock));
        return -1;
    }
    p->pts = pts;
    p->write_offset = offset;
    p->next = NULL;
    if (audec->pts_list.node_num == 0) { //first package
        audec->pts_list.first = p;
        audec->pts_list.current = p;
        audec->pts_list.node_num = 1;
    } else {
        audec->pts_list.current->next = p;
        audec->pts_list.current = p;
        if (audec->pts_list.node_num == 100) {
          p = audec->pts_list.first;
          audec->pts_list.first = audec->pts_list.first->next;
          adec_print("ptsnode_add > 100 first = %p", audec->pts_list.first);
          free(p);
        } else {
           audec->pts_list.node_num++;
        }
    }
    lp_unlock(&(audec->pts_list.tslock));
    return 0;
}


int get_decoder_status(void *p, struct adec_status *adec)
{
    aml_audio_dec_t *audec = (aml_audio_dec_t *)p;
    if (audec && audec->g_bst) {
        adec->channels = audec->g_bst->channels;
        adec->sample_rate = audec->g_bst->samplerate;
        adec->resolution = audec->g_bst->data_width;
        adec->error_count = audec->nDecodeErrCount; //need count
        adec->status = (audec->state > INITTED) ? 1 : 0;
        return 0;
    } else {
        return -1;
    }
}

/**
 * \brief register audio decoder
 * \param audec pointer to audec ,codec_type
 * \return 0 on success otherwise -1 if an error occurred
 */
int RegisterDecode(aml_audio_dec_t *audec, int type)
{
    switch (type) {
    case AUDIO_ARM_DECODER:
        audec->adec_ops = (audio_decoder_operations_t *)malloc(sizeof(audio_decoder_operations_t));
        if (!audec->adec_ops) {
            return -1;
        }
        memset((char *)(audec->adec_ops), 0, sizeof(audio_decoder_operations_t));
        audec->adec_ops->name = "FFmpegDecoder";
        audec->adec_ops->nAudioDecoderType = AUDIO_ARM_DECODER;
        if (0 != find_audio_lib(audec)) {
            return -1;;
        }
        audec->adec_ops->priv_data = audec;
        break;
    case AUDIO_FFMPEG_DECODER:
        audec->adec_ops = &AudioFFmpegDecoder;
        audec->adec_ops->priv_data = audec;
        break;
    default:
        audec->adec_ops = &AudioFFmpegDecoder;
        audec->adec_ops->priv_data = audec;
        break;
    }
    return 0;
}
static int tmp_buffer_init(circle_buffer *tmp, int buffer_size) {
    circle_buffer *buf = tmp;
    pthread_mutex_lock(&buf->lock);
    buf->size = buffer_size;
    buf->start_add = malloc(buffer_size * sizeof(char));
    if (buf->start_add == NULL) {
        adec_print("%s, Malloc out buffer error!\n", __FUNCTION__);
        pthread_mutex_unlock(&buf->lock);
        return -1;
    }
    buf->rd = buf->start_add;
    buf->wr = buf->start_add ;//+ buf->size / 2;

    pthread_mutex_unlock(&buf->lock);
    return 0;
}

static int tmp_buffer_release(circle_buffer *tmp) {
    circle_buffer *buf = tmp;
    pthread_mutex_lock(&buf->lock);

    if (buf->start_add != NULL) {
        free(buf->start_add);
        buf->start_add = NULL;
    }
    buf->rd = NULL;
    buf->wr = NULL;
    buf->size = 0;

    pthread_mutex_unlock(&buf->lock);
    return 0;
}

static int tmp_buffer_reset(circle_buffer *tmp) {
    circle_buffer *buf = tmp;
    buf->rd = buf->wr + buf->size / 2;
    if (buf->rd >= (buf->start_add + buf->size))
        buf->rd -= buf->size;
    return 0;
}

static int InBufferInit(aml_audio_dec_t *audec)
{
    if (audec->use_hardabuf) {
        int ret = uio_init(audec);
        if (ret < 0) {
            adec_print("uio init error! \n");
            return -1;
        }
        return 0;
    } else {
        //init software buf
        int ret;
        adec_print("%s, init software buf!\n", __FUNCTION__);
        pthread_mutex_init(&audec->buf.lock ,NULL);
        ret = tmp_buffer_init(&audec->buf, 256*1024);
        if (ret < 0) {
            adec_print("%s, malloc temp buffer error!\n", __FUNCTION__);
        }

   }
}
static int InBufferRelease(aml_audio_dec_t *audec)
{
    //    close(audec->fd_uio);
    //    audec->fd_uio=-1;
    if (audec->use_hardabuf) {
        uio_deinit(audec);
        return 0;
     } else {
        //release software buf
        tmp_buffer_release (&audec->buf);
        return 0;
     }
}

static int OutBufferInit(aml_audio_dec_t *audec)
{
    audec->pcm_buf_tmp = malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);
    if (!audec->pcm_buf_tmp) {
        adec_print("[%s %d]pcm_buf_tmp malloc failed! \n", __FUNCTION__, __LINE__);
        return -1;
    }

    audec->g_bst = malloc(sizeof(buffer_stream_t));
    if (!audec->g_bst) {
        adec_print("[%s %d]g_bst malloc failed! \n", __FUNCTION__, __LINE__);
        audec->g_bst = NULL;
        return -1;
    } else {
        adec_print("[%s %d] audec->g_bst/%p", __FUNCTION__, __LINE__, audec->g_bst);
    }

    memset(audec->g_bst, 0, sizeof(buffer_stream_t));

    if (audec->adec_ops->nOutBufSize <= 0) { //set default if not set
        audec->adec_ops->nOutBufSize = DEFAULT_PCM_BUFFER_SIZE;
    }

    int ret = init_buff(audec->g_bst, audec->adec_ops->nOutBufSize);
    if (ret == -1) {
        adec_print("[%s %d]pcm buffer init failed !\n", __FUNCTION__, __LINE__);
        return -1;
    }
    adec_print("[%s %d]pcm buffer init ok buf_size:%d\n", __FUNCTION__, __LINE__, audec->g_bst->buf_length);

    audec->g_bst->data_width = audec->data_width = AV_SAMPLE_FMT_S16;

    if (audec->channels > 0) {
        audec->g_bst->channels = audec->channels;
    }

    if (audec->samplerate > 0) {
        audec->g_bst->samplerate = audec->samplerate;
    }
    audec->g_bst->format = audec->format;

    return 0;
}
static int OutBufferInit_raw(aml_audio_dec_t *audec)
{
    audec->g_bst_raw = malloc(sizeof(buffer_stream_t));
    if (!audec->g_bst_raw) {
        adec_print("[%s %d]g_bst_raw malloc failed!\n", __FUNCTION__, __LINE__);
        audec->g_bst_raw = NULL;
        return -1;
    } else {
        adec_print("[%s %d] audec->audec->g_bst_raw/%p", __FUNCTION__, __LINE__, audec->g_bst_raw);
    }

    if (audec->adec_ops->nOutBufSize <= 0) { //set default if not set
        audec->adec_ops->nOutBufSize = DEFAULT_PCM_BUFFER_SIZE;
    }
    if ((audec->format == ACODEC_FMT_DTS || audec->format == ACODEC_FMT_TRUEHD) && amsysfs_get_sysfs_int("/sys/class/audiodsp/digital_raw") == 2) {
        audec->adec_ops->nOutBufSize *= 2;
    }
    int ret = init_buff(audec->g_bst_raw, audec->adec_ops->nOutBufSize);
    if (ret == -1) {
        adec_print("[%s %d]raw_buf init failed !\n", __FUNCTION__, __LINE__);
        return -1;
    }
    adec_print("[%s %d]raw buffer init ok buf_size/%d\n", __FUNCTION__, __LINE__, audec->g_bst_raw->buf_length);

    audec->g_bst_raw->data_width = audec->data_width = AV_SAMPLE_FMT_S16;
    if (audec->channels > 0) {
        audec->g_bst_raw->channels = audec->channels;
    } else {
        audec->g_bst_raw->channels = audec->channels = 2;
    }

    if (audec->samplerate > 0) {
        audec->g_bst_raw->samplerate = audec->samplerate;
    } else {
        audec->g_bst_raw->samplerate = audec->samplerate = 48000;
    }

    return 0;
}
static int OutBufferRelease(aml_audio_dec_t *audec)
{
    if (audec->g_bst) {
        adec_print("[%s %d] audec->g_bst/%p", __FUNCTION__, __LINE__, audec->g_bst);
        release_buffer(audec->g_bst);
        audec->g_bst = NULL;
    }

    if (audec->pcm_buf_tmp) {
        free(audec->pcm_buf_tmp);
        audec->pcm_buf_tmp = NULL;
    }
    return 0;
}

static int OutBufferRelease_raw(aml_audio_dec_t *audec)
{
    if (audec->g_bst_raw) {
        adec_print("[%s %d] audec->g_bst_raw/%p", __FUNCTION__, __LINE__, audec->g_bst_raw);
        release_buffer(audec->g_bst_raw);
        audec->g_bst_raw = NULL;
    }
    return 0;
}

static int enable_raw_output(aml_audio_dec_t *audec)
{
    int enable = 0;
    enable = amsysfs_get_sysfs_int("/sys/class/audiodsp/digital_raw");
    if (enable) {
        if (audec->format == ACODEC_FMT_AC3 || audec->format == ACODEC_FMT_EAC3 || audec->format == ACODEC_FMT_DTS ||
            audec->format == ACODEC_FMT_TRUEHD) {
            return 1;
        }
    }
    return 0;
}

static int audio_codec_init(aml_audio_dec_t *audec)
{
    //reset static&global
    audec->exit_decode_thread = 0;
    audec->exit_decode_thread_success = 0;
    audec->decode_offset = 0;
    audec->decode_sum_size = 0;
    audec->nDecodeErrCount = 0;
    audec->g_bst = NULL;
    audec->g_bst_raw = NULL;
    audec->fd_uio = -1;
    audec->last_valid_pts = 0;
    audec->out_len_after_last_valid_pts = 0;
    audec->pcm_cache_size = 0;
    audec->sn_threadid = -1;
    audec->sn_getpackage_threadid = -1;
    audec->OmxFirstFrameDecoded = 0;
    audec->write_offset =0;

    package_list_init(audec);
    ptsnode_list_init(audec);
    while (0 != set_sysfs_int(DECODE_ERR_PATH, DECODE_NONE_ERR)) {
        adec_print("[%s %d]set codec fatal failed ! \n", __FUNCTION__, __LINE__);
        amthreadpool_thread_usleep(100000);
    }

    adec_print("[%s %d]param:data_width:%d samplerate:%d channel:%d \n",
               __FUNCTION__, __LINE__, audec->data_width, audec->samplerate, audec->channels);

    audec->data_width = AV_SAMPLE_FMT_S16;
    if (audec->channels > 0) {
        int NumChSave = audec->channels;
        audec->channels = (audec->channels > 2 ? 2 : audec->channels);
        audec->adec_ops->channels = audec->channels;
        if (audec->format == ACODEC_FMT_PCM_S16BE  || audec->format == ACODEC_FMT_PCM_S16LE  ||
            audec->format == ACODEC_FMT_PCM_U8     || audec->format == ACODEC_FMT_PCM_BLURAY ||
            audec->format == ACODEC_FMT_WIFIDISPLAY || audec->format == ACODEC_FMT_ALAW       ||
            audec->format == ACODEC_FMT_MULAW      || audec->format == ACODEC_FMT_ADPCM ||
            audec->format == ACODEC_FMT_WMAPRO) {
            audec->adec_ops->channels = NumChSave;
        }
    }
    //for raac/cook audio pts are updated from audio decoder,so set a invalid pts default.
    else if (audec->format == ACODEC_FMT_RAAC || audec->format == ACODEC_FMT_COOK) {
        audec->decode_offset = 0xffffffff;
    }
    if (audec->samplerate > 0) {
        audec->adec_ops->samplerate = audec->samplerate;
    } else {
        //   audec->adec_ops->samplerate=audec->samplerate=48000;
    }
    switch (audec->data_width) {
    case AV_SAMPLE_FMT_U8:
        audec->adec_ops->bps = 8;
        break;
    case AV_SAMPLE_FMT_S16:
        audec->adec_ops->bps = 16;
        break;
    case AV_SAMPLE_FMT_S32:
        audec->adec_ops->bps = 32;
        break;
    default:
        audec->adec_ops->bps = 16;
    }
    adec_print("[%s %d]param_applied: bps:%d samplerate:%d channel:%d \n",
               __FUNCTION__, __LINE__, audec->adec_ops->bps, audec->adec_ops->samplerate, audec->adec_ops->channels);

    audec->adec_ops->extradata_size = audec->extradata_size;
    if (audec->extradata_size > 0) {
        memcpy(audec->adec_ops->extradata, audec->extradata, audec->extradata_size);
    }

    int ret = 0;
    if (!audec->StageFrightCodecEnableType) { //1-decoder init
        ret = audec->adec_ops->init(audec->adec_ops);
    }
    if (ret == -1) {
        adec_print("[%s %d]adec_ops init err\n", __FUNCTION__, __LINE__);
        goto err1;
    }

    ret = OutBufferInit(audec); //2-pcm_buffer init
    if (ret == -1) {
        adec_print("[%s %d]out buffer init err\n", __FUNCTION__, __LINE__);
        goto err2;
    }
    if (enable_raw_output(audec)) {
        ret = OutBufferInit_raw(audec);
        if (ret == -1) {
            adec_print("[%s %d]out_raw buffer init err\n", __FUNCTION__, __LINE__);
            OutBufferRelease_raw(audec);
            goto err2;
        }
    }
    ret = InBufferInit(audec); //3-init uio
    if (ret == -1) {
        adec_print("====in buffer  init err \n");
        goto err3;
    }

/*if the stream contains the main and associate data, we will init the associate buffer
  *assoc_bst_ptr to store the point of associate bst
  *assoc_enable to store  the en/dis-able of associate
  *associate_dec_supported
        *as 1>media.udc.inoutmix as "input:2,output:1,mix:1,"
            set the media.udc.mixinglevel as the (mixing_level*64/100-32)
        *as 0>media.udc.inoutmix as "input:1,output:1,mix:1,"
  */
    adec_print("%s::%d-[associate_dec_supported:%d]\n", __FUNCTION__, __LINE__, audec->associate_dec_supported);
    if (audec->associate_dec_supported == 1) {
        ret = InAssocBufferInit(audec);
        if (ret == -1) {
            adec_print("====in buffer  init err \n");
            goto err3;
        }
        if ((audec->format == ACODEC_FMT_AC3) || (audec->format == ACODEC_FMT_EAC3)) {
            amsysfs_write_prop(UDCInOutMix,"input:2,output:1,mix:1,");
        }
        //set the mixing level between main and assoc
        audio_set_mixing_level_between_main_assoc(audec->mixing_level);
    }
    else {
        if ((audec->format == ACODEC_FMT_AC3) || (audec->format == ACODEC_FMT_EAC3)) {
            amsysfs_write_prop(UDCInOutMix,"input:1,output:1,mix:1,");
        }
    }

    //4-other init
    audec->adsp_ops.dsp_on = 1;
    audec->adsp_ops.dsp_read = armdec_stream_read;
    audec->adsp_ops.get_cur_pts = armdec_get_pts;
    audec->adsp_ops.get_cur_pcrscr =  armdec_get_pcrscr;
    audec->adsp_ops.set_cur_apts    = armdec_set_pts;
    audec->adsp_ops.set_skip_bytes = armdec_set_skip_bytes;
    audec->adsp_ops.dsp_read_raw = armdec_stream_read_raw;
    audec->pcm_bytes_readed = 0;
    audec->raw_bytes_readed = 0;
    audec->raw_frame_size = 0;
    audec->pcm_frame_size = 0;
    audec->i2s_iec958_sync_flag = 1;
    audec->i2s_iec958_sync_gate = 0;
    audec->codec_type = 0;
#ifdef ANDROID	
    audec->parm_omx_codec_read_assoc_data = read_assoc_data;
#endif
    return 0;

err1:
    audec->adec_ops->release(audec->adec_ops);
    return -1;
err2:
    audec->adec_ops->release(audec->adec_ops);
    OutBufferRelease(audec);
    return -1;
err3:
    audec->adec_ops->release(audec->adec_ops);
    free(audec->adec_ops);
    OutBufferRelease(audec);
    InBufferRelease(audec);
    OutBufferRelease_raw(audec);
    InAssocBufferRelease(audec);
    return -1;
}
int audio_codec_release(aml_audio_dec_t *audec)
{
    //1-decode thread quit
    if (!audec->StageFrightCodecEnableType) {
        stop_decode_thread(audec);//1-decode thread quit
        audec->adec_ops->release(audec->adec_ops);//2-decoder release
    } else {
    #ifdef ANDROID
        stop_decode_thread_omx(audec);
	#endif
    }
    if (audec->associate_dec_supported == 1) {
        if ((audec->format == ACODEC_FMT_AC3) || (audec->format == ACODEC_FMT_EAC3)) {
            amsysfs_write_prop(UDCInOutMix,"input:1,output:1,mix:1,");
        }
    }

    InBufferRelease(audec);//3-uio uninit
    OutBufferRelease(audec);//4-outbufferrelease
    OutBufferRelease_raw(audec);
    InAssocBufferRelease(audec);
    audec->adsp_ops.dsp_on = -1;//5-other release
    audec->adsp_ops.dsp_read = NULL;
    audec->adsp_ops.get_cur_pts = NULL;
    audec->adsp_ops.dsp_file_fd = -1;
    audec->parm_omx_codec_read_assoc_data = NULL;

    return 0;
}


static int audio_hardware_ctrl(hw_command_t cmd)
{
    int fd;
    fd = open(AUDIO_CTRL_DEVICE, O_RDONLY);
    if (fd < 0) {
        adec_print("Open Device %s Failed!", AUDIO_CTRL_DEVICE);
        return -1;
    }

    switch (cmd) {
    case HW_CHANNELS_SWAP:
        ioctl(fd, AMAUDIO_IOC_SET_CHANNEL_SWAP, 0);
        break;

    case HW_LEFT_CHANNEL_MONO:
        ioctl(fd, AMAUDIO_IOC_SET_LEFT_MONO, 0);
        break;

    case HW_RIGHT_CHANNEL_MONO:
        ioctl(fd, AMAUDIO_IOC_SET_RIGHT_MONO, 0);
        break;

    case HW_STEREO_MODE:
        ioctl(fd, AMAUDIO_IOC_SET_STEREO, 0);
        break;

    default:
        adec_print("Unknow Command %d!", cmd);
        break;

    };

    close(fd);
    return 0;
}

static int get_first_apts_flag(dsp_operations_t *dsp_ops)
{
    int val;
    if (dsp_ops->dsp_file_fd < 0) {
        adec_print("[%s %d]read error!! audiodsp have not opened\n", __FUNCTION__, __LINE__);
        return -1;
    }
    ioctl(dsp_ops->dsp_file_fd, GET_FIRST_APTS_FLAG, &val);
    return val;
}


/**
 * \brief start audio dec when receive START command.
 * \param audec pointer to audec
 */
static int start_adec(aml_audio_dec_t *audec)
{
    int ret;
    audio_out_operations_t *aout_ops = &audec->aout_ops;
    dsp_operations_t *dsp_ops = &audec->adsp_ops;
    unsigned long  vpts, apts;
    int times = 0;
    char buf[32];
    apts = vpts = 0;
    audec->no_first_apts = 0;
    audec->apts_start_flag = 0;
    audec->first_apts = 0;

    char value[PROPERTY_VALUE_MAX] = {0};
    int wait_count = 100;
    if (property_get("media.amadec.wait_count", value, NULL) > 0) {
        wait_count = atoi(value);
    }
    adec_print("wait first apts count :%d \n", wait_count);

    if (audec->state == INITTED) {
        //get info from the audiodsp == can get from amstreamer
        while ((!get_first_apts_flag(dsp_ops)) && (!audec->need_stop) && (!audec->no_first_apts)) {
            adec_print("wait first pts checkin complete !");
            if (amthreadpool_on_requare_exit(pthread_self())) {
                adec_print("[%s:%d] quick interrupt \n", __FUNCTION__, __LINE__);
                break;
            }
            times++;
            if (times >= wait_count) {
                amsysfs_get_sysfs_str(TSYNC_VPTS, buf, sizeof(buf));// read vpts
                if (sscanf(buf, "0x%lx", &vpts) < 1) {
                    adec_print("unable to get vpts from: %s", buf);
                    return -1;
                }
                // save vpts to apts
                if (vpts == 0) { // vpts invalid too
                    times = 0; // loop again
                    continue;
                }
                adec_print("## can't get first apts, save vpts to apts,vpts=%lx, \n", vpts);
                sprintf(buf, "0x%lx", vpts);
                amsysfs_set_sysfs_str(TSYNC_APTS, buf);
                audec->no_first_apts = 1;
            }
            amthreadpool_thread_usleep(100000);
        }
        adec_print("get first apts ok, times:%d need_stop:%d auto_mute %d\n", times, audec->need_stop, audec->auto_mute);
        if (audec->need_stop) {
            return 0;
        }

        /*start  the  the pts scr,...*/
        if (audec->use_hardabuf)
          ret = adec_pts_start(audec);
        if (audec->auto_mute) {
            avsync_en(0);
            adec_pts_pause();
            while ((!audec->need_stop) && track_switch_pts(audec)) {
                amthreadpool_thread_usleep(1000);
            }
            avsync_en(1);
            adec_pts_resume();
            audec->auto_mute = 0;
        }
        if (audec->tsync_mode == TSYNC_MODE_PCRMASTER) {
            adec_print("[wcs-%s]-before audio track start,sleep 200ms\n",__FUNCTION__);
            amthreadpool_thread_usleep(200 * 1000); //200ms
        }
        aout_ops->start(audec);
        audec->state = ACTIVE;
    } else {
        adec_print("amadec status invalid, start adec failed \n");
        return -1;
    }

    return 0;
}

/**
 * \brief pause audio dec when receive PAUSE command.
 * \param audec pointer to audec
 */
static void pause_adec(aml_audio_dec_t *audec)
{
    audio_out_operations_t *aout_ops = &audec->aout_ops;
    if (audec->state == ACTIVE) {
        audec->state = PAUSED;
        adec_pts_pause();
        if (audec->use_render_add)
            vdec_pts_pause();
        aout_ops->pause(audec);
    }
}

/**
 * \brief resume audio dec when receive RESUME command.
 * \param audec pointer to audec
 */
static void resume_adec(aml_audio_dec_t *audec)
{
    audio_out_operations_t *aout_ops = &audec->aout_ops;
    if (audec->state == PAUSED) {
        audec->state = ACTIVE;
        aout_ops->resume(audec);
        adec_pts_resume();
        if (audec->use_render_add)
            vdec_pts_resume();
    }
}

/**
 * \brief stop audio dec when receive STOP command.
 * \param audec pointer to audec
 */
static void stop_adec(aml_audio_dec_t *audec)
{
    audio_out_operations_t *aout_ops = &audec->aout_ops;
    adec_print("[%s %d]audec->state/%d\n", __FUNCTION__, __LINE__, audec->state);
    if (audec->state > INITING) {
        char buf[64];

        audec->state = STOPPED;
        // buildroot need not this add by lianlian.zhu
        //aout_ops->mute(audec, 1); //mute output, some repeat sound in audioflinger after stop
        aout_ops->stop(audec);
        audio_codec_release(audec);

        sprintf(buf, "0x%lx", 0);
        amsysfs_set_sysfs_str(TSYNC_FIRSTAPTS, buf);
    }
}

/**
 * \brief release audio dec when receive RELEASE command.
 * \param audec pointer to audec
 */
static void release_adec(aml_audio_dec_t *audec)
{
    audec->state = TERMINATED;
}

/**
 * \brief mute audio dec when receive MUTE command.
 * \param audec pointer to audec
 * \param en 1 = mute, 0 = unmute
 */
static void mute_adec(aml_audio_dec_t *audec, int en)
{
    audio_out_operations_t *aout_ops = &audec->aout_ops;
    if (aout_ops->mute) {
        adec_print("%s the output !\n", (en ? "mute" : "unmute"));
        aout_ops->mute(audec, en);
        audec->muted = en;
    }
}

/**
 * \brief set volume to audio dec when receive SET_VOL command.
 * \param audec pointer to audec
 * \param vol volume value
 */
static void adec_set_volume(aml_audio_dec_t *audec, float vol)
{
    audio_out_operations_t *aout_ops = &audec->aout_ops;
    if (aout_ops->set_volume) {
        adec_print("set audio volume! vol = %f\n", vol);
        aout_ops->set_volume(audec, vol);
    }
}

/**
 * \brief set volume to audio dec when receive SET_LRVOL command.
 * \param audec pointer to audec
 * \param lvol left channel volume value
 * \param rvol right channel volume value
 */
static void adec_set_lrvolume(aml_audio_dec_t *audec, float lvol, float rvol)
{
    audio_out_operations_t *aout_ops = &audec->aout_ops;
    if (aout_ops->set_lrvolume) {
        adec_print("set audio volume! left vol = %f,right vol:%f\n", lvol, rvol);
        aout_ops->set_lrvolume(audec, lvol, rvol);
    }
}
static void adec_flag_check(aml_audio_dec_t *audec)
{
    audio_out_operations_t *aout_ops = &audec->aout_ops;
    if (audec->auto_mute && (audec->state > INITTED)) {
        aout_ops->pause(audec);
        amthreadpool_thread_usleep(10000);
        while ((!audec->need_stop) && track_switch_pts(audec)) {
            amthreadpool_thread_usleep(1000);
        }
        aout_ops->resume(audec);
        audec->auto_mute = 0;
    }
}

static void start_decode_thread(aml_audio_dec_t *audec)
{
    if (audec->state != INITTED) {
        adec_print("decode not inited quit \n");
        return -1;
    }

    pthread_t    tid;
    int ret = amthreadpool_pthread_create_name(&tid, NULL, (void *)audio_getpackage_loop, (void *)audec, "getpackagelp");
    audec->sn_getpackage_threadid = tid;
    adec_print("[%s]Create get package thread success! tid = %d\n", __FUNCTION__, tid);

    ret = amthreadpool_pthread_create_name(&tid, NULL, (void *)audio_decode_loop, (void *)audec, "decodeloop");
    if (ret != 0) {
        adec_print("[%s]Create ffmpeg decode thread failed!\n", __FUNCTION__);
        return ret;
    }
    audec->sn_threadid = tid;
    audec->audio_decoder_enabled = 0x1;
    pthread_setname_np(tid, "AmadecDecodeLP");
    adec_print("[%s]Create ffmpeg decode thread success! tid = %d\n", __FUNCTION__, tid);
}
static void stop_decode_thread(aml_audio_dec_t *audec)
{
    audec->exit_decode_thread = 1;
    int ret;
    if (audec->audio_decoder_enabled == 0x1) {
        ret = amthreadpool_pthread_join(audec->sn_threadid, NULL);
        adec_print("[%s]decode thread exit success\n", __FUNCTION__);
        ret = amthreadpool_pthread_join(audec->sn_getpackage_threadid, NULL);
        adec_print("[%s]get package thread exit success\n", __FUNCTION__);

    }
    audec->exit_decode_thread = 0;
    audec->sn_threadid = -1;
    audec->sn_getpackage_threadid = -1;
}

/* --------------------------------------------------------------------------*/
/**
* @brief  getNextFrameSize  Get next frame size
*
* @param[in]   format     audio format
*
* @return      -1: no frame_size  use default   0: need get again  non_zero: success
*/
/* --------------------------------------------------------------------------*/

static int get_frame_size(aml_audio_dec_t *audec)
{
    int frame_szie = 0;
    int ret = 0;
    int extra_data = 8; //?
    StartCode *start_code = &audec->start_code;

    if (start_code->status == 0 || start_code->status == 3) {
        memset(start_code, 0, sizeof(StartCode));
    }
    /*ape case*/
    if (audec->format == ACODEC_FMT_APE) {
        if (start_code->status == 0) { //have not get the sync data
            ret = read_buffer(start_code->buff, 4);
            if (ret <= 0) {
                return 0;
            }
            start_code->size = 4;
            start_code->status = 1;
        }

        if (start_code->status == 1) { //start find sync word
            if (start_code->size < 4) {
                ret = read_buffer(start_code->buff + start_code->size, 4 - start_code->size);
                if (ret <= 0) {
                    return 0;
                }
                start_code->size = 4;
            }
            if (start_code->size == 4) {
                if ((start_code->buff[0] == 'A') && (start_code->buff[1] == 'P') && (start_code->buff[2] == 'T') && (start_code->buff[3] == 'S')) {
                    start_code->size = 0;
                    start_code->status = 2; //sync word found ,start find frame size
                } else {
                    start_code->size = 3;
                    start_code->buff[0] = start_code->buff[1];
                    start_code->buff[1] = start_code->buff[2];
                    start_code->buff[2] = start_code->buff[3];
                    return 0;
                }
            }

        }

        if (start_code->status == 2) {
            ret = read_buffer(start_code->buff, 4);
            if (ret <= 0) {
                return 0;
            }
            start_code->size = 4;
            frame_szie  = start_code->buff[3] << 24 | start_code->buff[2] << 16 | start_code->buff[1] << 8 | start_code->buff[0] + extra_data;
            frame_szie  = (frame_szie + 3) & (~3);
            start_code->status = 3; //found frame size
            return frame_szie;
        }
    }
    return -1;
}
// check if audio format info changed,if changed, apply new parameters to audio track
static void check_audio_info_changed(aml_audio_dec_t *audec)
{
    buffer_stream_t *g_bst = audec->g_bst;
    AudioInfo   g_AudioInfo = {0};
    int BufLevelAllowDoFmtChg = 0;
    audio_decoder_operations_t *adec_ops  = audec->adec_ops;
    adec_ops->getinfo(audec->adec_ops, &g_AudioInfo);
    if (g_AudioInfo.channels != 0 && g_AudioInfo.samplerate != 0) {
        if ((g_AudioInfo.channels != g_bst->channels) || (g_AudioInfo.samplerate != g_bst->samplerate)) {
            // the first time we get sample rate/channel num info,we use that to set audio track.
            if (audec->channels == 0 || audec->samplerate == 0) {
                g_bst->channels = audec->channels = g_AudioInfo.channels;
                g_bst->samplerate = audec->samplerate = g_AudioInfo.samplerate;
            } else {
                //experienc value:0.2 Secs
                BufLevelAllowDoFmtChg = audec->samplerate * audec->channels * (audec->adec_ops->bps >> 3) / 5;
                while ((audec->format_changed_flag || g_bst->buf_level > BufLevelAllowDoFmtChg) && !audec->exit_decode_thread) {
                    amthreadpool_thread_usleep(20000);
                }
                if (!audec->exit_decode_thread) {
                    adec_print("[%s]Info Changed: src:sample:%d  channel:%d dest sample:%d  channel:%d PCMBufLevel:%d\n",
                               __FUNCTION__, audec->samplerate, audec->channels, g_AudioInfo.samplerate, g_AudioInfo.channels, g_bst->buf_level);
                    g_bst->channels = g_AudioInfo.channels;
                    g_bst->samplerate = g_AudioInfo.samplerate;
                    if (audec->state == ACTIVE)
                        audec->aout_ops.pause(audec);
                    audec->format_changed_flag = 1;
                }
            }
        }
    }
}
void *audio_getpackage_loop(void *args)
{
    int ret;
    aml_audio_dec_t *audec;
    audio_decoder_operations_t *adec_ops;
    int nNextFrameSize = 0; //next read frame size
    int inlen = 0;//real data size in in_buf
    int nInBufferSize = 0; //full buffer size
    unsigned char *inbuf = NULL;//real buffer
    int rlen = 0;//read buffer ret size
    int nAudioFormat;
    unsigned wfd = 0;
    if (am_getconfig_bool("media.libplayer.wfd"))  {
        wfd = 1;
    }

    adec_print("[%s]adec_getpackage_loop start!\n", __FUNCTION__);
    audec = (aml_audio_dec_t *)args;
    adec_ops = audec->adec_ops;
    nAudioFormat = audec->format;
    inlen = 0;
    nNextFrameSize = adec_ops->nInBufSize;
    while (1) {
exit_decode_loop:
        if (audec->exit_decode_thread) { /*detect quit condition*/
            if (inbuf) {
                free(inbuf);
            }
            package_list_free(audec);
            ptsnode_list_free(audec);
            break;
        }

        nNextFrameSize = get_frame_size(audec); /*step 2  get read buffer size*/
        if (nNextFrameSize == -1) {
            nNextFrameSize = adec_ops->nInBufSize;
        } else if (nNextFrameSize == 0) {
            amthreadpool_thread_usleep(1000);
            continue;
        }

        nInBufferSize = nNextFrameSize;/*step 3  read buffer*/
        if (inbuf != NULL) {
            free(inbuf);
            inbuf = NULL;
        }
        inbuf = malloc(nInBufferSize);

        int nNextReadSize = nInBufferSize;
        int nRet = 0;
        int nReadErrCount = 0;
        int nCurrentReadCount = 0;
        int nReadSizePerTime = 1 * 1024;
        rlen = 0;
        int sleeptime = 0;
        while (nNextReadSize > 0 && !audec->exit_decode_thread) {
            if (nNextReadSize <= nReadSizePerTime) {
                nReadSizePerTime = nNextReadSize;
            }
            if (audec->use_hardabuf) {
                nRet = read_buffer(inbuf + rlen, nReadSizePerTime); //read 10K per time
            } else {
                nRet = buffer_read(&audec->buf,inbuf + rlen,nReadSizePerTime);
                //adec_print("buffer_read %d nReadSizePerTime:%d \n",nRet,nReadSizePerTime)
            }
            if (nRet <= 0) {
                sleeptime++;
                amthreadpool_thread_usleep(1000);
                continue;
            }
            rlen += nRet;
            nNextReadSize -= nRet;
            if (wfd && nAudioFormat == ACODEC_FMT_AAC) {
                if (rlen > 300) {
                    break;
                }
            }
        }
        //      adec_print(" read data %d,sleep time %d ms    \n",  rlen,sleeptime);

        sleeptime = 0;
        nCurrentReadCount = rlen;
        rlen += inlen;
        ret = -1;
        while ((ret = package_add(audec, inbuf, rlen)) && !audec->exit_decode_thread) {
            amthreadpool_thread_usleep(1000);
        }
        if (ret) {
            free(inbuf);
        }
        inbuf = NULL;
    }
QUIT:
    adec_print("[%s]Exit adec_getpackage_loop Thread finished!", __FUNCTION__);
    pthread_exit(NULL);
    return NULL;
}

void *audio_decode_loop(void *args)
{
    int ret;
    aml_audio_dec_t *audec;
    audio_out_operations_t *aout_ops;
    audio_decoder_operations_t *adec_ops;
    int nNextFrameSize = 0; //next read frame size
    int inlen = 0;//real data size in in_buf
    int nRestLen = 0; //left data after last decode
    int nInBufferSize = 0; //full buffer size
    //int nStartDecodePoint=0;//start decode point in in_buf
    unsigned char *inbuf = NULL;//real buffer
    int rlen = 0;//read buffer ret size
    char *pRestData = NULL;
    char *inbuf2;

    int dlen = 0;//decode size one time
    int declen = 0;//current decoded size
    int nCurrentReadCount = 0;
    int needdata = 0;
    char startcode[5];
    int extra_data = 8;
    int nCodecID;
    int nAudioFormat;
    unsigned char *outbuf;
    int outlen = 0;
    struct package *p_Package;
    buffer_stream_t *g_bst;
    buffer_stream_t *g_bst_raw;//for ac3&eac3 passthrough
    AudioInfo g_AudioInfo = {0};
    adec_print("[%s]adec_armdec_loop start!\n",__FUNCTION__);
    int dgraw = amsysfs_get_sysfs_int("/sys/class/audiodsp/digital_raw");

    audec = (aml_audio_dec_t *)args;
    outbuf = audec->pcm_buf_tmp;
    aout_ops = &audec->aout_ops;
    adec_ops=audec->adec_ops;
    memset(outbuf, 0, AVCODEC_MAX_AUDIO_FRAME_SIZE);
    g_bst=audec->g_bst;
    if (dgraw) {
        g_bst_raw=audec->g_bst_raw;
        g_bst_raw->format = audec->format;
    }

    nAudioFormat=audec->format;
    g_bst->format = audec->format;

    inlen = 0;
    nNextFrameSize = adec_ops->nInBufSize;
    adec_ops->nAudioDecoderType = audec->format;
    while (1) {

exit_decode_loop:

        if (audec->exit_decode_thread) { //detect quit condition
            if (inbuf) {
                free(inbuf);
                inbuf = NULL;
            }
            if (pRestData) {
                free(pRestData);
                pRestData = NULL;
            }
            audec->exit_decode_thread_success = 1;
            break;
        }
        if (audec->state != ACTIVE  && audec->decode_sum_size > 0) {
            amthreadpool_thread_usleep(1000);
            continue;
        }
        //step 2  get read buffer size
        p_Package = package_get(audec);
        if (!p_Package) {
            amthreadpool_thread_usleep(1000);
            continue;
        }
        if (inbuf != NULL) {
            free(inbuf);
            inbuf = NULL;
        }

        if (inlen && pRestData) {
            rlen = p_Package->size + inlen;
            inbuf = malloc(rlen);
            memcpy(inbuf, pRestData, inlen);
            memcpy(inbuf + inlen, p_Package->data, p_Package->size);
            free(pRestData);
            free(p_Package->data);
            pRestData = NULL;
            p_Package->data = NULL;
        } else {
            rlen = p_Package->size;
            inbuf = p_Package->data;
            p_Package->data = NULL;
        }
        free(p_Package);
        p_Package = NULL;

        nCurrentReadCount = rlen;
        inlen = rlen;
        declen  = 0;
        if (nCurrentReadCount > 0) {
            while (declen < rlen && !audec->exit_decode_thread) {
                outlen = AVCODEC_MAX_AUDIO_FRAME_SIZE;
                if (nAudioFormat == ACODEC_FMT_COOK || nAudioFormat == ACODEC_FMT_RAAC || nAudioFormat == ACODEC_FMT_AMR) {
                    if (needdata > 0) {
                        pRestData = malloc(inlen);
                        if (pRestData) {
                            memcpy(pRestData, (uint8_t *)(inbuf + declen), inlen);
                        }
                        needdata = 0;
                        break;
                    }
                }

                dlen = adec_ops->decode(audec->adec_ops, outbuf, &outlen, inbuf + declen, inlen);
                if (outlen > 0) {
                    audec->decode_sum_size += outlen;
                    check_audio_info_changed(audec);
                }
                if (outlen > AVCODEC_MAX_AUDIO_FRAME_SIZE) {
                    adec_print("!!!!!fatal error,out buffer overwriten,out len %d,actual %d", outlen, AVCODEC_MAX_AUDIO_FRAME_SIZE);
                }
                if (dlen <= 0) {

                    if (nAudioFormat == ACODEC_FMT_APE) {
                        inlen = 0;
                    } else if (inlen > 0) {
                        pRestData = malloc(inlen);
                        if (pRestData) {
                            memcpy(pRestData, (uint8_t *)(inbuf + declen), inlen);
                        }
                    }
                    audec->nDecodeErrCount++;//decode failed, add err_count
                    needdata = 0;
                    break;
                }
                audec->nDecodeErrCount = 0; //decode success reset to 0
                declen += dlen;
                inlen -= dlen;

                /* decoder input buffer not enough,need new data burst */
                if (nAudioFormat == ACODEC_FMT_COOK || nAudioFormat == ACODEC_FMT_RAAC || nAudioFormat == ACODEC_FMT_AMR) {
                    if (inlen <= declen) {
                        needdata ++;
                    }
                }
                // for aac decoder, if decoder cost es data but no pcm output,may need more data ,which is needed by frame resync
                else if (nAudioFormat == ACODEC_FMT_AAC_LATM || nAudioFormat == ACODEC_FMT_AAC) {
                    if (outlen == 0 && inlen) {
                        pRestData = malloc(inlen);
                        if (pRestData) {
                            memcpy(pRestData, (uint8_t *)(inbuf + declen), inlen);
                        }
                        audec->decode_offset += dlen; //update es offset for apts look up
                        break;
                    }
                }
                //write to the pcm buffer
                if (nAudioFormat == ACODEC_FMT_RAAC || nAudioFormat == ACODEC_FMT_COOK) {
                    audec->decode_offset = audec->adec_ops->pts;
                } else {
                    audec->decode_offset += dlen;
                }
                if (((AUDIO_SPDIF_PASSTHROUGH == dgraw) || (AUDIO_HDMI_PASSTHROUGH == dgraw)) &&
                     ((ACODEC_FMT_AC3 == nAudioFormat) || (ACODEC_FMT_EAC3 == nAudioFormat) || (ACODEC_FMT_DTS == nAudioFormat))) {
                    int bytesread=0;
                    while (outlen) {
                        //sub the pcm header(4bytes) and the pcm data(0x1800bytes)
                        unsigned char *output_pcm_buf, *output_raw_buf;
                        output_pcm_buf = outbuf + HEADER_LENGTH_AFTER_IEC61937 + bytesread;
                        int output_pcm_len = *(int *)outbuf;
                        audec->pcm_cache_size=output_pcm_len;
                        outlen = outlen - HEADER_LENGTH_AFTER_IEC61937 - output_pcm_len;

                        //sub the raw header(4bytes) and the raw data(AC3:0x1800bytes/EAC3:0x6000bytes)
                        output_raw_buf = output_pcm_buf + output_pcm_len + HEADER_LENGTH_AFTER_IEC61937;
                        int output_raw_len = *(int *)(outbuf + HEADER_LENGTH_AFTER_IEC61937 + output_pcm_len);
                        outlen = outlen - HEADER_LENGTH_AFTER_IEC61937 - output_raw_len;

                        bytesread += 2*HEADER_LENGTH_AFTER_IEC61937+output_pcm_len+output_raw_len;
                        //use alsa-out.c output pcm data
                        if (g_bst) {
                            int wlen=0;

                            while (output_pcm_len && (!audec->exit_decode_thread)) {
                                if (g_bst->buf_length-g_bst->buf_level<output_pcm_len) {
                                usleep(100000);
                                continue;
                                }
                                wlen=write_pcm_buffer(output_pcm_buf, g_bst, output_pcm_len);
                                output_pcm_len -= wlen;
                                audec->pcm_cache_size-=wlen;
                            }
                        }

                        //use alsa-out-raw.c output raw data
                        if ( (g_bst_raw) && ((AUDIO_SPDIF_PASSTHROUGH == dgraw) || (AUDIO_HDMI_PASSTHROUGH == dgraw)) )
                        {
                            int wlen=0;

                            while (output_raw_len && !audec->exit_decode_thread) {
                                if (g_bst_raw->buf_length-g_bst_raw->buf_level<output_raw_len) {
                                    usleep(100000);
                                    continue;
                                }
                                wlen=write_pcm_buffer(output_raw_buf, g_bst_raw,output_raw_len);
                                output_raw_len -= wlen;
                            }
                        }
                    }
                } else {
                audec->pcm_cache_size = outlen;
                if (g_bst) {
                    int wlen = 0;
                    while (outlen && !audec->exit_decode_thread) {
                        if (g_bst->buf_length - g_bst->buf_level < outlen) {
                            amthreadpool_thread_usleep(100000);
                            continue;
                        }
                        wlen = write_pcm_buffer(outbuf, g_bst, outlen);
                        outlen -= wlen;
                        audec->pcm_cache_size -= wlen;
                        }
                      }
                    }
                  }
        } else {
            amthreadpool_thread_usleep(1000);
            continue;
        }
    }

    adec_print("[%s]exit adec_armdec_loop Thread finished!", __FUNCTION__);
    pthread_exit(NULL);
error:
    pthread_exit(NULL);
    return NULL;
}

void *adec_armdec_loop(void *args)
{
    int ret;
    aml_audio_dec_t *audec;
    audio_out_operations_t *aout_ops;
    adec_cmd_t *msg = NULL;

    audec = (aml_audio_dec_t *)args;
    aout_ops = &audec->aout_ops;

    // codec init
    audec->state = INITING;
    while (1) {
        if (audec->need_stop) {
            goto MSG_LOOP;
        }

        ret = audio_codec_init(audec);
        if (ret == 0) {
            break;
        }
        usleep(10000);
    }
    audec->state = INITTED;

    //start decode thread
    while (1) {
        if (audec->need_stop) {
            //no need to call release, will do it in stop_adec
            goto MSG_LOOP;
        }
        if (audec->StageFrightCodecEnableType) {
#ifdef ANDROID			
            start_decode_thread_omx(audec);
#endif
            if (audec->OmxFirstFrameDecoded != 1) { // just one case, need_stop == 1
                //usleep(10000); // no need
                continue;
            }
        } else {
            start_decode_thread(audec);
        }
        //ok
        break;
    }

    //aout init
    while (1) {
        if (audec->need_stop) {
            //no need to call release, will do it in stop_adec
            goto MSG_LOOP;
        }
        //wait the audio sr/ch ready to set audio track.
        adec_print("wait audio sr/channel begin \n");
        while (((!audec->channels) || (!audec->samplerate)) && !audec->need_stop) {
            amthreadpool_thread_usleep(2000);
        }
        adec_print("wait audio sr/channel done \n");
        ret = aout_ops->init(audec);
        if (ret) {
            adec_print("[%s %d]Audio out device init failed!", __FUNCTION__, __LINE__);
            amthreadpool_thread_usleep(10000);
            continue;
        }
        //ok
        break;
    }

    //wait first pts decoded
    start_adec(audec);

MSG_LOOP:
    do {

        adec_reset_track(audec);
        adec_flag_check(audec);
        if (audec->state == ACTIVE && audec->has_video)
            adec_refresh_pts(audec);
        msg = adec_get_message(audec);
        if (!msg) {
            amthreadpool_thread_usleep(10 * 1000); //if not wait,need changed to amthread usleep
            continue;
        }

        switch (msg->ctrl_cmd) {
        case CMD_START:

            adec_print("Receive START Command!\n");
            //------------------------
            if (!audec->StageFrightCodecEnableType) {
                start_decode_thread(audec);
            }
            //------------------------
            start_adec(audec);
            break;

        case CMD_PAUSE:

            adec_print("Receive PAUSE Command!");
            pause_adec(audec);
            break;

        case CMD_RESUME:

            adec_print("Receive RESUME Command!");
            resume_adec(audec);
            break;

        case CMD_STOP:

            adec_print("Receive STOP Command!");
            stop_adec(audec);
            break;

        case CMD_MUTE:

            adec_print("Receive Mute Command!");
            if (msg->has_arg) {
                mute_adec(audec, msg->value.en);
            }
            break;

        case CMD_SET_VOL:

            adec_print("Receive Set Vol Command!");
            if (msg->has_arg) {
                adec_set_volume(audec, msg->value.volume);
            }
            break;
        case CMD_SET_LRVOL:

            adec_print("Receive Set LRVol Command!");
            if (msg->has_arg) {
                adec_set_lrvolume(audec, msg->value.volume, msg->value_ext.volume);
            }
            break;

        case CMD_CHANL_SWAP:

            adec_print("Receive Channels Swap Command!");
            audio_hardware_ctrl(HW_CHANNELS_SWAP);
            break;

        case CMD_LEFT_MONO:

            adec_print("Receive Left Mono Command!");
            audio_hardware_ctrl(HW_LEFT_CHANNEL_MONO);
            break;

        case CMD_RIGHT_MONO:

            adec_print("Receive Right Mono Command!");
            audio_hardware_ctrl(HW_RIGHT_CHANNEL_MONO);
            break;

        case CMD_STEREO:

            adec_print("Receive Stereo Command!");
            audio_hardware_ctrl(HW_STEREO_MODE);
            break;

        case CMD_RELEASE:

            adec_print("Receive RELEASE Command!");
            release_adec(audec);
            break;

        default:
            adec_print("Unknow Command!");
            break;

        }

        if (msg) {
            adec_message_free(msg);
            msg = NULL;
        }
    } while (audec->state != TERMINATED);

    adec_print("Exit Message Loop Thread!");
    pthread_exit(NULL);
    return NULL;
}

