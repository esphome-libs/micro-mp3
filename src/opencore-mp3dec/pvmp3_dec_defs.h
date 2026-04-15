/* ------------------------------------------------------------------
 * Copyright (C) 1998-2009 PacketVideo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * -------------------------------------------------------------------
 */
/*
------------------------------------------------------------------------------
   PacketVideo Corp.
   MP3 Decoder Library

   Filename: pvmp3_dec_defs.h

     Date: 09/21/2007

------------------------------------------------------------------------------
 REVISION HISTORY

 Description:

------------------------------------------------------------------------------
 INCLUDE DESCRIPTION

 This include file has the mp3 decoder common defines.

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; CONTINUE ONLY IF NOT ALREADY DEFINED
----------------------------------------------------------------------------*/
#ifndef PVMP3_DEC_DEFS_H
#define PVMP3_DEC_DEFS_H

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "pvmp3_audio_type_defs.h"
#include "pvmp3decoder_api.h"

/*----------------------------------------------------------------------------
; MACROS
; Define module specific macros here
----------------------------------------------------------------------------*/
#define module(x, POW2)   ((x)&(POW2-1))

/*----------------------------------------------------------------------------
; DEFINES
; Include all pre-processor statements here.
----------------------------------------------------------------------------*/
// microMP3 NOTE: the original comment here read
//   "big enough to hold 4608 bytes == biggest mp3 frame"
// which is wrong on two counts:
//   1. 4608 is the biggest decoded PCM *output* size (1152 samples * 2ch * 2B),
//      not a compressed MP3 frame. The biggest compressed frame is 1441 bytes
//      (MPEG1 Layer III, 320 kbps, 32 kHz, padded).
//   2. BUFSIZE isn't sized for one frame in the first place. It's the size of
//      the bit reservoir ring buffer (mainDataBuffer[BUFSIZE]), which has to
//      hold the current frame's main data PLUS up to 511 bytes of prior
//      frames' main data referenced via the main_data_begin back-pointer.
//      8192 is "comfortably more than 1441 + 511, rounded to a power of two."
//
// pvmp3 also reuses BUFSIZE as the wrap modulus for reads of the *input*
// buffer (in getNbits / getUpTo9bits / getUpTo17bits / fillMainDataBuf), which
// implicitly assumes the caller supplies an 8192-byte circular buffer. The
// micro-mp3 wrapper does not -- it hands pvmp3 a slice sized for one frame
// (<= 1536 bytes). The wrapper is responsible for keeping usedBits within
// inputBufferCurrentLength so the modulo arithmetic is harmless (offset &
// 8191 == offset whenever offset < 1536); the input-side bound check added
// to fillMainDataBuf in this fork closes the one path where pvmp3 itself
// would have over-read.
#define BUFSIZE   8192

#define CHAN           2
#define GRAN           2


#define SUBBANDS_NUMBER        32
#define FILTERBANK_BANDS       18
#define HAN_SIZE              512


/* MPEG Header Definitions - ID Bit Values */

#define MPEG_1              0
#define MPEG_2              1
#define MPEG_2_5            2
#define INVALID_VERSION     -1

/* MPEG Header Definitions - Mode Values */

#define MPG_MD_STEREO           0
#define MPG_MD_JOINT_STEREO     1
#define MPG_MD_DUAL_CHANNEL     2
#define MPG_MD_MONO             3



#define LEFT        0
#define RIGHT       1


#define SYNC_WORD         (int32)0x7ff
#define SYNC_WORD_LNGTH   11

/*----------------------------------------------------------------------------
; EXTERNAL VARIABLES REFERENCES
; Declare variables used in this module but defined elsewhere
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; SIMPLE TYPEDEF'S
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; ENUMERATED TYPEDEF'S
----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum ERROR_CODE
    {
        NO_DECODING_ERROR         = 0,
        UNSUPPORTED_LAYER         = 1,
        UNSUPPORTED_FREE_BITRATE  = 2,
        FILE_OPEN_ERROR           = 3,          /* error opening file */
        CHANNEL_CONFIG_ERROR      = 4,     /* error in channel configuration */
        SYNTHESIS_WINDOW_ERROR    = 5,   /* error in synthesis window table */
        READ_FILE_ERROR           = 6,          /* error reading input file */
        SIDE_INFO_ERROR           = 7,          /* error in side info */
        HUFFMAN_TABLE_ERROR       = 8,      /* error in Huffman table */
        COMMAND_LINE_ERROR        = 9,       /* error in command line */
        MEMORY_ALLOCATION_ERROR   = 10,   /* error allocating memory */
        NO_ENOUGH_MAIN_DATA_ERROR = 11,
        SYNCH_LOST_ERROR          = 12,
        OUTPUT_BUFFER_TOO_SMALL   = 13     /* output buffer can't hold output */
    } ERROR_CODE;

    /*----------------------------------------------------------------------------
    ; STRUCTURES TYPEDEF'S
    ----------------------------------------------------------------------------*/

    /* Header Information Structure */

    typedef struct
    {
        int32 version_x;
        int32 layer_description;
        int32 error_protection;
        int32 bitrate_index;
        int32 sampling_frequency;
        int32 padding;
        int32 extension;
        int32 mode;
        int32 mode_ext;
        int32 copyright;
        int32 original;
        int32 emphasis;
    } mp3Header;


    /* Layer III side information. */

    typedef  struct
    {
        uint32 part2_3_length;
        uint32 big_values;
        int32 global_gain;
        uint32 scalefac_compress;
        uint32 window_switching_flag;
        uint32 block_type;
        uint32 mixed_block_flag;
        uint32 table_select[3];
        uint32 subblock_gain[3];
        uint32 region0_count;
        uint32 region1_count;
        uint32 preflag;
        uint32 scalefac_scale;
        uint32 count1table_select;

    } granuleInfo;

    typedef  struct
    {
        uint32      scfsi[4];
        granuleInfo gran[2];

    } channelInfo;

    /* Layer III side info. */

    typedef struct
    {
        uint32      main_data_begin;
        uint32      private_bits;
        channelInfo ch[2];

    } mp3SideInfo;

    /* Layer III scale factors. */
    typedef struct
    {
        int32 l[23];            /* [cb] */
        int32 s[3][13];         /* [window][cb] */

    } mp3ScaleFactors;


#ifdef __cplusplus
}
#endif

/*----------------------------------------------------------------------------
; GLOBAL FUNCTION DEFINITIONS
; Function Prototype declaration
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; END
----------------------------------------------------------------------------*/

#endif



