/**
 * Copyright (C) 2010 Regis Montoya (aka r3gis - www.r3gis.fr)
 * This file is part of pjsip_android.
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

#include <pjmedia/codec.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/errno.h>
#include <pjmedia/port.h>
#include <pjmedia-codec/types.h>
#include <pj/pool.h>
#include <pj/string.h>
#include <pj/assert.h>
#include <pj/log.h>


#if defined(PJMEDIA_HAS_SILK_CODEC) && (PJMEDIA_HAS_SILK_CODEC!=0)
#include <silk.h>
#include "SKP_Silk_SDK_API.h"

/* Define codec specific settings */
#define MAX_BYTES_PER_FRAME     250 // Equals peak bitrate of 100 kbps
#define MAX_INPUT_FRAMES        5
#define MAX_LBRR_DELAY          2
#define MAX_FRAME_LENGTH        480
#define FRAME_LENGTH_MS         20
#define MAX_API_FS_KHZ          48

#define THIS_FILE       "silk.c"


/* Prototypes for SILK factory */
static pj_status_t silk_test_alloc( pjmedia_codec_factory *factory,
				    const pjmedia_codec_info *id );
static pj_status_t silk_default_attr( pjmedia_codec_factory *factory,
				      const pjmedia_codec_info *id,
				      pjmedia_codec_param *attr );
static pj_status_t silk_enum_codecs (pjmedia_codec_factory *factory,
				     unsigned *count,
				     pjmedia_codec_info codecs[]);
static pj_status_t silk_alloc_codec( pjmedia_codec_factory *factory,
				     const pjmedia_codec_info *id,
				     pjmedia_codec **p_codec);
static pj_status_t silk_dealloc_codec( pjmedia_codec_factory *factory,
				       pjmedia_codec *codec );

/* Prototypes for SILK implementation. */
static pj_status_t  silk_codec_init( pjmedia_codec *codec,
			       pj_pool_t *pool );
static pj_status_t  silk_codec_open( pjmedia_codec *codec,
			       pjmedia_codec_param *attr );
static pj_status_t  silk_codec_close( pjmedia_codec *codec );
static pj_status_t  silk_codec_modify(pjmedia_codec *codec,
			        const pjmedia_codec_param *attr );
static pj_status_t  silk_codec_parse(pjmedia_codec *codec,
			       void *pkt,
			       pj_size_t pkt_size,
			       const pj_timestamp *timestamp,
			       unsigned *frame_cnt,
			       pjmedia_frame frames[]);
static pj_status_t  silk_codec_encode( pjmedia_codec *codec,
				 const struct pjmedia_frame *input,
				 unsigned output_buf_len,
				 struct pjmedia_frame *output);
static pj_status_t  silk_codec_decode( pjmedia_codec *codec,
				 const struct pjmedia_frame *input,
				 unsigned output_buf_len,
				 struct pjmedia_frame *output);
#if !PLC_DISABLED
static pj_status_t  silk_codec_recover( pjmedia_codec *codec,
				  unsigned output_buf_len,
				  struct pjmedia_frame *output);
#endif


enum
{
    PARAM_NB,	/* Index for narrowband parameter.	*/
    PARAM_WB,	/* Index for wideband parameter.	*/
    PARAM_UWB,	/* Index for ultra-wideband parameter	*/
};

/* Silk default parameter */
struct silk_param
{
    int		     enabled;		/* Is this mode enabled?	    */
    int		     pt;		/* Payload type.		    */
    unsigned	 clock_rate;	/* Default sampling rate to be used.*/
    int		     packet_size_ms;	/* packet size ms.		    */
    int		     bitrate;		/* Bit rate for current mode.	    */
    int		     max_bitrate;	/* Max bit rate for current mode.   */
    int 		complexity;
};

/* Definition for SILK codec operations. */
static pjmedia_codec_op silk_op =
{
    &silk_codec_init,
    &silk_codec_open,
    &silk_codec_close,
    &silk_codec_modify,
    &silk_codec_parse,
    &silk_codec_encode,
    &silk_codec_decode,
    NULL
   // &silk_codec_recover
};

/* Definition for SILK codec factory operations. */
static pjmedia_codec_factory_op silk_factory_op =
{
    &silk_test_alloc,
    &silk_default_attr,
    &silk_enum_codecs,
    &silk_alloc_codec,
    &silk_dealloc_codec,
    &pjmedia_codec_silk_deinit
};

/* SILK factory private data */
static struct silk_factory
{
    pjmedia_codec_factory	base;
    pjmedia_endpt	       *endpt;
    pj_pool_t		       *pool;
    pj_mutex_t		       *mutex;
    pjmedia_codec	     codec_list;
    struct silk_param	    silk_param[3];
} silk_factory;


/* SILK codec private data. */
struct silk_private
{
    int			 param_id;	    /**< Index to speex param.	*/

   // char		 obj_name[PJ_MAX_OBJ_NAME];

    pj_bool_t		 enc_ready;
    SKP_SILK_SDK_EncControlStruct	 enc;
    void* psEnc;

    pj_bool_t		 dec_ready;
    SKP_SILK_SDK_DecControlStruct	 dec;
    void* psDec;
};




PJ_DEF(pj_status_t) pjmedia_codec_silk_init(pjmedia_endpt *endpt)
{
    pjmedia_codec_mgr *codec_mgr;
    pj_status_t status;

    if (silk_factory.endpt != NULL) {
	/* Already initialized. */
	return PJ_SUCCESS;
    }

    /* Init factory */
    silk_factory.base.op = &silk_factory_op;
    silk_factory.base.factory_data = NULL;
    silk_factory.endpt = endpt;

    /* Create pool */
    silk_factory.pool = pjmedia_endpt_create_pool(endpt, "silk codecs", 4000, 4000);
    if (!silk_factory.pool)
	return PJ_ENOMEM;

    /* Init list */
    pj_list_init(&silk_factory.codec_list);

    /* Create mutex. */
    status = pj_mutex_create_simple(silk_factory.pool, "silk codecs",
				    &silk_factory.mutex);
    if (status != PJ_SUCCESS)
	goto on_error;
    PJ_LOG(5, (THIS_FILE, "Init silk"));

    /* Initialize default Speex parameter. */
    silk_factory.silk_param[PARAM_NB].enabled = 1; //((options & PJMEDIA_SILK_NO_NB) == 0);
    silk_factory.silk_param[PARAM_NB].pt = PJMEDIA_RTP_PT_SILK_NB;
	silk_factory.silk_param[PARAM_NB].clock_rate = 8000;
	silk_factory.silk_param[PARAM_NB].packet_size_ms = FRAME_LENGTH_MS;
	silk_factory.silk_param[PARAM_NB].complexity = 2;
	silk_factory.silk_param[PARAM_NB].bitrate = 20000;

	silk_factory.silk_param[PARAM_WB].enabled = 1; //((options & PJMEDIA_SILK_NO_WB) == 0);
	silk_factory.silk_param[PARAM_WB].pt = PJMEDIA_RTP_PT_SILK_WB;
	silk_factory.silk_param[PARAM_WB].clock_rate = 16000;
	silk_factory.silk_param[PARAM_WB].packet_size_ms = FRAME_LENGTH_MS;
	silk_factory.silk_param[PARAM_WB].complexity = 2;
	silk_factory.silk_param[PARAM_WB].bitrate = 30000;

	silk_factory.silk_param[PARAM_UWB].enabled = 1; //((options & PJMEDIA_SILK_NO_UWB) == 0);
	silk_factory.silk_param[PARAM_UWB].pt = PJMEDIA_RTP_PT_SILK_UWB;
	silk_factory.silk_param[PARAM_UWB].clock_rate = 24000;
	silk_factory.silk_param[PARAM_UWB].packet_size_ms = FRAME_LENGTH_MS;
	silk_factory.silk_param[PARAM_UWB].complexity = 2;
	silk_factory.silk_param[PARAM_UWB].bitrate = 40000;


    /* Get the codec manager. */
    codec_mgr = pjmedia_endpt_get_codec_mgr(endpt);
    if (!codec_mgr) {
	return PJ_EINVALIDOP;
    }

    PJ_LOG(5, (THIS_FILE, "Init silk > DONE"));

    /* Register codec factory to endpoint. */
    status = pjmedia_codec_mgr_register_factory(codec_mgr,
						&silk_factory.base);
    if (status != PJ_SUCCESS)
	return status;

    return PJ_SUCCESS;

on_error:
    if (silk_factory.mutex) {
	pj_mutex_destroy(silk_factory.mutex);
	silk_factory.mutex = NULL;
    }
    if (silk_factory.pool) {
	pj_pool_release(silk_factory.pool);
	silk_factory.pool = NULL;
    }

    return status;
}

/*
 * Unregister SILK codec factory from pjmedia endpoint and deinitialize
 * the SILK codec library.
 */
PJ_DEF(pj_status_t) pjmedia_codec_silk_deinit(void)
{
    pjmedia_codec_mgr *codec_mgr;
    pj_status_t status;

    if (silk_factory.endpt == NULL) {
	/* Not registered. */
	return PJ_SUCCESS;
    }

    /* Lock mutex. */
    pj_mutex_lock(silk_factory.mutex);

    /* Get the codec manager. */
    codec_mgr = pjmedia_endpt_get_codec_mgr(silk_factory.endpt);
    if (!codec_mgr) {
	silk_factory.endpt = NULL;
	pj_mutex_unlock(silk_factory.mutex);
	return PJ_EINVALIDOP;
    }

    /* Unregister silk codec factory. */
    status = pjmedia_codec_mgr_unregister_factory(codec_mgr,
						  &silk_factory.base);
    silk_factory.endpt = NULL;

    /* Destroy mutex. */
    pj_mutex_destroy(silk_factory.mutex);
    silk_factory.mutex = NULL;


    /* Release pool. */
    pj_pool_release(silk_factory.pool);
    silk_factory.pool = NULL;

    return status;
}

/*
 * Check if factory can allocate the specified codec.
 */
static pj_status_t silk_test_alloc(pjmedia_codec_factory *factory,
				   const pjmedia_codec_info *info )
{
    const pj_str_t silk_tag = { "SILK", 4};
    unsigned i;

    PJ_UNUSED_ARG(factory);
    PJ_ASSERT_RETURN(factory==&silk_factory.base, PJ_EINVAL);


    /* Type MUST be audio. */
    if (info->type != PJMEDIA_TYPE_AUDIO)
	return PJMEDIA_CODEC_EUNSUP;

    /* Check encoding name. */
    if (pj_stricmp(&info->encoding_name, &silk_tag) != 0)
	return PJMEDIA_CODEC_EUNSUP;

    /* Channel count must be one */
    if (info->channel_cnt != 1)
	return PJMEDIA_CODEC_EUNSUP;

    /* Check clock-rate */
    for (i=0; i<PJ_ARRAY_SIZE(silk_factory.silk_param); ++i) {
	if (info->clock_rate == silk_factory.silk_param[i].clock_rate) {
	    /* Okay, let's Speex! */
	    return PJ_SUCCESS;
	}
    }
    /* Clock rate not supported */
    return PJMEDIA_CODEC_EUNSUP;
}

/*
 * Generate default attribute.
 */
static pj_status_t silk_default_attr( pjmedia_codec_factory *factory,
				      const pjmedia_codec_info *id,
				      pjmedia_codec_param *attr )
{
    PJ_UNUSED_ARG(factory);
    PJ_LOG(5, (THIS_FILE, "silk default attr"));
    pj_bzero(attr, sizeof(pjmedia_codec_param));
    attr->info.channel_cnt = 1;

    //TODO : add good values here
    if (id->clock_rate <= 8000) {
		attr->info.clock_rate = silk_factory.silk_param[PARAM_NB].clock_rate;
		attr->info.avg_bps = silk_factory.silk_param[PARAM_NB].bitrate;
		attr->info.max_bps = 64000;//silk_factory.silk_param[PARAM_NB].max_bitrate;
		attr->info.frm_ptime = silk_factory.silk_param[PARAM_NB].packet_size_ms;

	} else if (id->clock_rate <= 16000) {
		attr->info.clock_rate =  silk_factory.silk_param[PARAM_WB].clock_rate;
		attr->info.avg_bps = silk_factory.silk_param[PARAM_WB].bitrate;
		attr->info.max_bps = 64000;//silk_factory.silk_param[PARAM_WB].max_bitrate;
		attr->info.frm_ptime = silk_factory.silk_param[PARAM_WB].packet_size_ms;
	} else {
		attr->info.clock_rate = silk_factory.silk_param[PARAM_UWB].clock_rate;
		attr->info.avg_bps = silk_factory.silk_param[PARAM_UWB].bitrate;
		attr->info.max_bps = 64000;//silk_factory.silk_param[PARAM_UWB].max_bitrate;
		attr->info.frm_ptime = silk_factory.silk_param[PARAM_UWB].packet_size_ms;
	}

	attr->info.pcm_bits_per_sample = 16;


	attr->info.pt = (pj_uint8_t) id->pt;

    attr->setting.frm_per_pkt = 1;
    attr->setting.vad = 0;
    attr->setting.plc = 0;

    /* Default all other flag bits disabled. */

    return PJ_SUCCESS;
}

/*
 * Enum codecs supported by this factory.
 */
static pj_status_t silk_enum_codecs(pjmedia_codec_factory *factory,
				    unsigned *count,
				    pjmedia_codec_info codecs[])
{
    unsigned max;
    int i;  /* Must be signed */
    PJ_LOG(5, (THIS_FILE, "silk enum codecs"));

    PJ_UNUSED_ARG(factory);
    PJ_ASSERT_RETURN(codecs && *count > 0, PJ_EINVAL);

    max = *count;
    *count = 0;

    for (i=PJ_ARRAY_SIZE(silk_factory.silk_param)-1; i>=0 && *count<max; --i) {

        PJ_LOG(5, (THIS_FILE, "silk add codecs"));
    	if (!silk_factory.silk_param[i].enabled){
    	    continue;
    	}
    	PJ_LOG(5, (THIS_FILE, "Add codec %d", silk_factory.silk_param[i].clock_rate));

    	pj_bzero(&codecs[*count], sizeof(pjmedia_codec_info));
    	codecs[*count].encoding_name = pj_str("SILK");
    	codecs[*count].pt = silk_factory.silk_param[i].pt;
    	codecs[*count].type = PJMEDIA_TYPE_AUDIO;
    	codecs[*count].clock_rate = silk_factory.silk_param[i].clock_rate;
    	codecs[*count].channel_cnt = 1;

    	++*count;
	}

	return PJ_SUCCESS;

}


/*
 * Allocate a new SILK codec instance.
 */
static pj_status_t silk_alloc_codec(pjmedia_codec_factory *factory,
				    const pjmedia_codec_info *id,
				    pjmedia_codec **p_codec)
{
    pjmedia_codec *codec;
    struct silk_private *silk;

    PJ_ASSERT_RETURN(factory && id && p_codec, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &silk_factory.base, PJ_EINVAL);


    pj_mutex_lock(silk_factory.mutex);

    /* Get free nodes, if any. */
    if (!pj_list_empty(&silk_factory.codec_list)) {
		codec = silk_factory.codec_list.next;
		pj_list_erase(codec);
    } else {
		codec = PJ_POOL_ZALLOC_T(silk_factory.pool, pjmedia_codec);
		PJ_ASSERT_RETURN(codec != NULL, PJ_ENOMEM);
		codec->op = &silk_op;
		codec->factory = factory;
		codec->codec_data = pj_pool_alloc(silk_factory.pool,
						  sizeof(struct silk_private));
    }

    pj_mutex_unlock(silk_factory.mutex);

    silk = (struct silk_private*) codec->codec_data;
    silk->enc_ready = PJ_FALSE;
    silk->dec_ready = PJ_FALSE;

    if (id->clock_rate <= 8000){
    	silk->param_id = PARAM_NB;
    }else if (id->clock_rate <= 16000){
    	silk->param_id = PARAM_WB;
    }else{
    	silk->param_id = PARAM_UWB;
    }

    *p_codec = codec;
    return PJ_SUCCESS;
}



/*
 * Free codec.
 */
static pj_status_t silk_dealloc_codec( pjmedia_codec_factory *factory,
				      pjmedia_codec *codec )
{
    struct silk_private *silk;

    PJ_ASSERT_RETURN(factory && codec, PJ_EINVAL);
    PJ_UNUSED_ARG(factory);
    PJ_ASSERT_RETURN(factory == &silk_factory.base, PJ_EINVAL);

    silk = (struct silk_private*) codec->codec_data;

    /* Close codec, if it's not closed. */
    if (silk->enc_ready == PJ_TRUE || silk->dec_ready == PJ_TRUE) {
    	silk_codec_close(codec);
    }


    /* Put in the free list. */
    pj_mutex_lock(silk_factory.mutex);
    pj_list_push_front(&silk_factory.codec_list, codec);
    pj_mutex_unlock(silk_factory.mutex);


    return PJ_SUCCESS;
}

/*
 * Init codec.
 */
static pj_status_t silk_codec_init(pjmedia_codec *codec,
				   pj_pool_t *pool )
{
    PJ_UNUSED_ARG(codec);
    PJ_UNUSED_ARG(pool);
    return PJ_SUCCESS;
}


/*
 * Open codec.
 */
static pj_status_t silk_codec_open(pjmedia_codec *codec,
				   pjmedia_codec_param *attr )
{

    pj_status_t status;
    struct silk_private *silk;
    int id, ret = 0;
    struct silk_param params;

    silk = (struct silk_private*) codec->codec_data;
    id = silk->param_id;



    pj_assert(silk != NULL);
    pj_assert(silk->enc_ready == PJ_FALSE &&
   	      silk->dec_ready == PJ_FALSE);


    SKP_int32 encSizeBytes, decSizeBytes;

    params = silk_factory.silk_param[id];

    PJ_LOG(5, (THIS_FILE, "Open silk codec @ %d", params.clock_rate));
    /* default settings */
	SKP_int32 API_fs_Hz = params.clock_rate;
	SKP_int32 max_internal_fs_Hz = params.clock_rate;
	if(max_internal_fs_Hz > 24000){
		//TODO : refine ranges
		max_internal_fs_Hz = 24000;
	}
	SKP_int32 targetRate_bps = params.bitrate;
	SKP_int32 packetSize_ms = params.packet_size_ms;
	SKP_int32 packetLoss_perc = 0, complexity_mode = params.complexity, smplsSinceLastPacket;
	SKP_int32 INBandFEC_enabled = 0, DTX_enabled = 0, quiet = 0;

    /* Create Encoder */

    ret = SKP_Silk_SDK_Get_Encoder_Size( &encSizeBytes );
    if(ret){
        PJ_LOG(1, (THIS_FILE, "Unable to get encoder size : %d", ret));
        return PJ_EINVAL;
    }
    silk->psEnc = malloc( encSizeBytes );


    /* Reset Encoder */
    ret = SKP_Silk_SDK_InitEncoder( silk->psEnc, &silk->enc );
    if(ret){
		PJ_LOG(1, (THIS_FILE, "Unable to init encoder : %d", ret));
		//TODO free malloc
		return PJ_EINVAL;
	}



    /* Set Encoder parameters */
    silk->enc.API_sampleRate        = API_fs_Hz;
    silk->enc.maxInternalSampleRate = max_internal_fs_Hz;
    silk->enc.packetSize            = ( packetSize_ms * API_fs_Hz ) / 1000;
    silk->enc.packetLossPercentage  = packetLoss_perc;
    silk->enc.useInBandFEC          = INBandFEC_enabled;
    silk->enc.useDTX                = DTX_enabled;
    silk->enc.complexity            = complexity_mode;
    silk->enc.bitRate               = ( targetRate_bps > 0 ? targetRate_bps : 0 );

    silk->enc_ready = PJ_TRUE;


    //Decoder

    /* Set Decoder parameters */
    silk->dec.API_sampleRate		  = API_fs_Hz;
    /* Create decoder */
	ret = SKP_Silk_SDK_Get_Decoder_Size( &decSizeBytes );
	if(ret){
		PJ_LOG(1, (THIS_FILE, "Unable to get dencoder size : %d", ret));
		//TODO free malloc
		return PJ_EINVAL;
	}
	silk->psDec = malloc( decSizeBytes );
	/* Reset decoder */
	ret = SKP_Silk_SDK_InitDecoder( silk->psDec );
	if(ret){
		PJ_LOG(1, (THIS_FILE, "Unable to init dencoder : %d", ret));
		//TODO free malloc
		return PJ_EINVAL;
	}
    silk->dec_ready = PJ_TRUE;

    PJ_LOG(4, (THIS_FILE, "Open silk codec > DONE !!"));
    return PJ_SUCCESS;
}

/*
 * Close codec.
 */
static pj_status_t silk_codec_close( pjmedia_codec *codec )
{
    struct silk_private *silk;
    silk = (struct silk_private*) codec->codec_data;

    //TODO : see if we can destroy encoder/decoder here
    silk->enc_ready = PJ_FALSE;
    silk->dec_ready = PJ_FALSE;

    PJ_LOG(4, (THIS_FILE, "SILK codec closed"));
    return PJ_SUCCESS;
}

/*
 * Modify codec settings.
 */
static pj_status_t  silk_codec_modify(pjmedia_codec *codec,
				      const pjmedia_codec_param *attr )
{

    return PJ_SUCCESS;
}


/*
 * Encode frame.
 */
static pj_status_t silk_codec_encode(pjmedia_codec *codec,
				     const struct pjmedia_frame *input,
				     unsigned output_buf_len,
				     struct pjmedia_frame *output)
{
	struct silk_private *silk;
	silk = (struct silk_private*) codec->codec_data;
    SKP_int32 ret;
    SKP_int16 nBytes;
    pj_assert(silk && input && output);

    /* Encode */
    //TODO silence detection
    output->size = 0;
    //PJ_LOG(4, (THIS_FILE, "Input size : %d - Encoder packet size  %d", input->size, silk->enc.packetSize));
    nBytes = output_buf_len;

    //That's fine with pjmedia cause input size is always already the good size
	ret = SKP_Silk_SDK_Encode( silk->psEnc, &silk->enc,
			(pj_int16_t*)input->buf, ( input->size >> 1 ),
			(SKP_uint8 *)output->buf , &nBytes );
	if( ret ) {
		//TODO : better conversion of silk errors to pjmedia errors
		PJ_LOG(1, (THIS_FILE, "Impossible to encode packet %d", ret));
		if(ret == SKP_SILK_ENC_PAYLOAD_BUF_TOO_SHORT){
			return PJMEDIA_CODEC_EFRMTOOSHORT;
		}
		return PJMEDIA_CODEC_EPCMFRMINLEN;
	}


	output->size = nBytes;
    output->type = PJMEDIA_FRAME_TYPE_AUDIO;
    output->timestamp = input->timestamp;

    return PJ_SUCCESS;
}


/*
 * Get frames in the packet.
 */

static pj_status_t  silk_codec_parse( pjmedia_codec *codec,
				     void *pkt,
				     pj_size_t pkt_size,
				     const pj_timestamp *ts,
				     unsigned *frame_cnt,
				     pjmedia_frame frames[])
{
	    unsigned count;
		struct silk_private *silk;
		silk = (struct silk_private*) codec->codec_data;

	    PJ_ASSERT_RETURN(frame_cnt, PJ_EINVAL);

	//    PJ_LOG(4, (THIS_FILE, "We have an input of %d bytes ", pkt_size));

	    count = 0;
	    int dec_frame_size = pkt_size;
	    int samples_per_frame = silk->enc.API_sampleRate * 20 / 1000;

	    while (pkt_size >= dec_frame_size && count < *frame_cnt) {
			frames[count].type = PJMEDIA_FRAME_TYPE_AUDIO;
			frames[count].buf = pkt;
			frames[count].size = dec_frame_size;
			frames[count].timestamp.u64 = ts->u64 + count * samples_per_frame; // fHz * ptime / 1000

			pkt = ((char*)pkt) + dec_frame_size;
			pkt_size -= dec_frame_size;

			++count;
	    }
	    *frame_cnt = count;
	    return PJ_SUCCESS;

}



static pj_status_t silk_codec_decode(pjmedia_codec *codec,
				     const struct pjmedia_frame *input,
				     unsigned output_buf_len,
				     struct pjmedia_frame *output)
{
    SKP_int16 nBytes;
    int ret = 0, id;

    PJ_ASSERT_RETURN(input && output, PJ_EINVAL);

	struct silk_private *silk;
	unsigned count;
    SKP_int16 len;
    struct silk_param params;

    silk = (struct silk_private*) codec->codec_data;
    id = silk->param_id;


    params = silk_factory.silk_param[id];

    //For silk parsing need to decode...
    len = 320;

    ret = SKP_Silk_SDK_Decode( silk->psDec, &silk->dec,
    		0, //not loss frames
    		input->buf, //Packet
    		input->size, //Packet size
    		output->buf,
    		&len );
	if(ret){
		 PJ_LOG(1, (THIS_FILE, "Failed to decode silk frame : %d", ret));
	}
//	PJ_LOG(4, (THIS_FILE, "Decode silk frame len : %d, %d, %d", len, silk->dec.frameSize, silk->dec.framesPerPacket));
	if(silk->dec.framesPerPacket == len){
		output->size = len;
		output->type = PJMEDIA_FRAME_TYPE_AUDIO;
		output->timestamp = input->timestamp;
	}else{
		output->size = 0;
		output->type = PJMEDIA_FRAME_TYPE_NONE;
		output->buf = NULL;
	}


    return PJ_SUCCESS;
}

/*
 * Recover lost frame.
 */
static pj_status_t  silk_codec_recover(pjmedia_codec *codec,
				      unsigned output_buf_len,
				      struct pjmedia_frame *output)
{
	//TODO : reactivate this
	struct silk_private *silk;
	silk = (struct silk_private*) codec->codec_data;

	SKP_int16 nBytes;
    int ret;
    PJ_ASSERT_RETURN(output, PJ_EINVAL);

    PJ_LOG(5, (THIS_FILE, "Recover silk frame"));

    /* Decode */
	ret = SKP_Silk_SDK_Decode( silk->psDec, &silk->dec, 1, NULL, 0, output->buf, &nBytes );
	if(ret){
		PJ_LOG(1, (THIS_FILE, "Failed to decode silk frame"));
	}

	output->size = nBytes;
    output->type = PJMEDIA_FRAME_TYPE_AUDIO;

    return PJ_SUCCESS;
}



#endif
