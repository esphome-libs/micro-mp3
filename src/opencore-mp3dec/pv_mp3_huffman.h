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

   Filename: pv_mp3_huffman.h

   Date: 09/21/2007

------------------------------------------------------------------------------
 REVISION HISTORY

 Description:
------------------------------------------------------------------------------
 INCLUDE DESCRIPTION


------------------------------------------------------------------------------
 REFERENCES

 [1] ISO MPEG Audio Subgroup Software Simulation Group (1996)
     ISO 13818-3 MPEG-2 Audio Decoder - Lower Sampling Frequency Extension

------------------------------------------------------------------------------
*/
/*----------------------------------------------------------------------------
; CONTINUE ONLY IF NOT ALREADY DEFINED
----------------------------------------------------------------------------*/

#ifndef PV_MP3_HUFFMAN_H
#define PV_MP3_HUFFMAN_H


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/

#include "pvmp3_audio_type_defs.h"
#include "s_mp3bits.h"
#include "s_tmp3dec_file.h"
#include "pvmp3_getbits.h"

/*----------------------------------------------------------------------------
; MACROS
; Define module specific macros here
----------------------------------------------------------------------------*/

#if defined(__GNUC__) || defined(__clang__)
#define MP3_HUFFMAN_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define MP3_HUFFMAN_INLINE static __forceinline
#else
#define MP3_HUFFMAN_INLINE static inline
#endif

/*----------------------------------------------------------------------------
; EXTERNAL VARIABLES REFERENCES
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; DEFINES AND SIMPLE TYPEDEF'S
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; GLOBAL FUNCTION DEFINITIONS
; Function Prototype declaration
----------------------------------------------------------------------------*/

#ifdef __cplusplus
extern "C"
{
#endif

    int32 pvmp3_huffman_parsing(int32 is[SUBBANDS_NUMBER*FILTERBANK_BANDS],
    granuleInfo *grInfo,
    tmp3dec_file   *pVars,
    int32 part2_start,
    mp3Header *info);

#ifdef __cplusplus
}
#endif

/*
 * Per-codeword decoders, defined inline so the big-values loop in
 * pvmp3_huffman_parsing() calls them directly instead of through a per-region
 * function pointer, folding them into the loop. Only the per-table
 * h->pdec_huff_tab lookup stays indirect. The get1bit / getUpTo17bits readers
 * they use are inline in pvmp3_getbits.h.
 */
MP3_HUFFMAN_INLINE void pvmp3_huffman_quad_decoding(struct huffcodetab *h,
                                                    int32 *is,
                                                    tmp3Bits *pMainData)
{
    int32 x;
    int32 y;
    int32 v;
    int32 w;

    y = (*h->pdec_huff_tab)(pMainData);

    if (y)
    {
        v = (y >> 3);
        if (v)
        {
            if (get1bit(pMainData))
            {
                v = -v;
            }
        }
        w = (y >> 2) & 1;
        if (w)
        {
            if (get1bit(pMainData))
            {
                w = -w;
            }
        }
        x = (y >> 1) & 1;
        if (x)
        {
            if (get1bit(pMainData))
            {
                x = -x;
            }
        }
        y =  y & 1;
        if (y)
        {
            if (get1bit(pMainData))
            {
                y = -y;
            }
        }
    }
    else
    {
        v = 0;
        w = 0;
        x = 0;
    }

    *is     = v;
    *(is + 1) = w;
    *(is + 2) = x;
    *(is + 3) = y;
}

MP3_HUFFMAN_INLINE void pvmp3_huffman_pair_decoding(struct huffcodetab *h,
                                                    int32 *is,
                                                    tmp3Bits *pMainData)
{
    int32 x;
    int32 y;

    uint16 cw = (*h->pdec_huff_tab)(pMainData);

    if (cw)
    {
        x = cw >> 4;
        if (x)
        {
            if (get1bit(pMainData))
            {
                x = -x;
            }
            y = cw & 0xf;
            if (y && get1bit(pMainData))
            {
                y = -y;
            }
        }
        else
        {
            y = cw & 0xf;
            if (get1bit(pMainData))
            {
                y = -y;
            }
        }

        *is     = x;
        *(is + 1) = y;
    }
    else
    {
        *is     = 0;
        *(is + 1) = 0;
    }
}

MP3_HUFFMAN_INLINE void pvmp3_huffman_pair_decoding_linbits(struct huffcodetab *h,
                                                            int32 *is,
                                                            tmp3Bits *pMainData)
{
    int32 x;
    int32 y;

    uint16 cw = (*h->pdec_huff_tab)(pMainData);
    x = cw >> 4;

    if (15 == (uint32)x)
    {
        int32 tmp = getUpTo17bits(pMainData, (h->linbits + 1));
        x += tmp >> 1;
        if (tmp&1)
        {
            x = -x;
        }
    }
    else if (x)
    {
        if (get1bit(pMainData))
        {
            x = -x;
        }
    }

    y = cw & 0xf;
    if (15 == (uint32)y)
    {
        int32 tmp = getUpTo17bits(pMainData, (h->linbits + 1));
        y += tmp >> 1;
        if (tmp&1)
        {
            y = -y;
        }
    }
    else if (y)
    {
        if (get1bit(pMainData))
        {
            y = -y;
        }
    }

    *is     = x;
    *(is + 1) = y;
}

/*----------------------------------------------------------------------------
; END
----------------------------------------------------------------------------*/

#endif



