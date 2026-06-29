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

   Filename: pvmp3_getbits.h

     Date: 09/21/2007

------------------------------------------------------------------------------
 REVISION HISTORY

 Description:

------------------------------------------------------------------------------
 INCLUDE DESCRIPTION


------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; CONTINUE ONLY IF NOT ALREADY DEFINED
----------------------------------------------------------------------------*/
#ifndef PVMP3_GETBITS_H
#define PVMP3_GETBITS_H

/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/
#include "pvmp3_dec_defs.h"
#include "s_mp3bits.h"
#include "pvmp3_audio_type_defs.h"

/*----------------------------------------------------------------------------
; MACROS
; Define module specific macros here
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; DEFINES
; Include all pre-processor statements here.
----------------------------------------------------------------------------*/
#define INBUF_ARRAY_INDEX_SHIFT  (3)
#define INBUF_BIT_WIDTH         (1<<(INBUF_ARRAY_INDEX_SHIFT))
#define INBUF_BIT_MODULO_MASK   ((INBUF_BIT_WIDTH)-1)


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

/*----------------------------------------------------------------------------
; STRUCTURES TYPEDEF'S
----------------------------------------------------------------------------*/
/*
 * Bit-buffer readers, defined inline so the call and the per-call load/store of
 * pMainData->usedBits fold away at each call site. They are leaf functions hit
 * once per Huffman codeword and per sign bit.
 */
#if defined(__GNUC__) || defined(__clang__)
#define MP3_GETBITS_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define MP3_GETBITS_INLINE static __forceinline
#else
#define MP3_GETBITS_INLINE static inline
#endif

MP3_GETBITS_INLINE uint32 getNbits(tmp3Bits *ptBitStream,
                                   int32 neededBits) /* up to 25 bits */
{
    uint32    offset;
    uint32    bitIndex;
    uint8     Elem;
    uint8     Elem1;
    uint8     Elem2;
    uint8     Elem3;
    uint32   returnValue = 0;

    if (!neededBits)
    {
        return (returnValue);
    }

    offset = (ptBitStream->usedBits) >> INBUF_ARRAY_INDEX_SHIFT;

    Elem  = *(ptBitStream->pBuffer + module(offset  , BUFSIZE));
    Elem1 = *(ptBitStream->pBuffer + module(offset + 1, BUFSIZE));
    Elem2 = *(ptBitStream->pBuffer + module(offset + 2, BUFSIZE));
    Elem3 = *(ptBitStream->pBuffer + module(offset + 3, BUFSIZE));

    returnValue = (((uint32)(Elem)) << 24) |
                  (((uint32)(Elem1)) << 16) |
                  (((uint32)(Elem2)) << 8) |
                  ((uint32)(Elem3));

    bitIndex = module(ptBitStream->usedBits, INBUF_BIT_WIDTH);
    returnValue <<= bitIndex;
    returnValue >>= (32 - neededBits);

    ptBitStream->usedBits += neededBits;

    return (returnValue);
}

MP3_GETBITS_INLINE uint16 getUpTo9bits(tmp3Bits *ptBitStream,
                                       int32 neededBits) /* 2 to 9 bits */
{
    uint32    offset;
    uint32    bitIndex;
    uint8    Elem;
    uint8    Elem1;
    uint16   returnValue;

    offset = (ptBitStream->usedBits) >> INBUF_ARRAY_INDEX_SHIFT;

    Elem  = *(ptBitStream->pBuffer + module(offset  , BUFSIZE));
    Elem1 = *(ptBitStream->pBuffer + module(offset + 1, BUFSIZE));

    returnValue = (((uint16)(Elem)) << 8) |
                  ((uint16)(Elem1));

    bitIndex = module(ptBitStream->usedBits, INBUF_BIT_WIDTH);

    ptBitStream->usedBits += neededBits;
    returnValue = (returnValue << (bitIndex));

    return (uint16)(returnValue >> (16 - neededBits));
}

MP3_GETBITS_INLINE uint32 getUpTo17bits(tmp3Bits *ptBitStream,
                                        int32 neededBits) /* 2 to 17 bits */
{
    uint32    offset;
    uint32    bitIndex;
    uint8     Elem;
    uint8     Elem1;
    uint8     Elem2;
    uint32   returnValue;

    offset = (ptBitStream->usedBits) >> INBUF_ARRAY_INDEX_SHIFT;

    Elem  = *(ptBitStream->pBuffer + module(offset  , BUFSIZE));
    Elem1 = *(ptBitStream->pBuffer + module(offset + 1, BUFSIZE));
    Elem2 = *(ptBitStream->pBuffer + module(offset + 2, BUFSIZE));

    returnValue = (((uint32)(Elem)) << 16) |
                  (((uint32)(Elem1)) << 8) |
                  ((uint32)(Elem2));

    bitIndex = module(ptBitStream->usedBits, INBUF_BIT_WIDTH);

    ptBitStream->usedBits += neededBits;
    returnValue = 0xFFFFFF & (returnValue << (bitIndex));

    return (uint32)(returnValue >> (24 - neededBits));
}

MP3_GETBITS_INLINE uint8 get1bit(tmp3Bits *ptBitStream)
{
    uint32    offset;
    uint32    bitIndex;
    uint8   returnValue;

    offset = (ptBitStream->usedBits) >> INBUF_ARRAY_INDEX_SHIFT;

    returnValue  = *(ptBitStream->pBuffer + module(offset  , BUFSIZE));

    bitIndex = module(ptBitStream->usedBits, INBUF_BIT_WIDTH);
    ptBitStream->usedBits++;

    returnValue = (returnValue << (bitIndex));

    return (uint8)(returnValue >> 7);
}

/*----------------------------------------------------------------------------
; GLOBAL FUNCTION DEFINITIONS
; Function Prototype declaration
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; END
----------------------------------------------------------------------------*/

#endif

