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

   Filename: pvmp3_seek_synch.cpp

   Functions:
        pvmp3_header_sync


     Date: 9/21/2007

------------------------------------------------------------------------------
 REVISION HISTORY


 Description:

   microMP3: pvmp3_frame_synch() was removed from this file; it was dead code
   (its only caller, the deleted pvmp3_decoder.cpp, is gone). See CHANGES.md.
   This file now contains only pvmp3_header_sync().

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/

#include "pvmp3_seek_synch.h"
#include "pvmp3_getbits.h"


/*----------------------------------------------------------------------------
; MACROS
; Define module specific macros here
----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
; DEFINES
; Include all pre-processor statements here. Include conditional
; compile variables also.
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; LOCAL FUNCTION DEFINITIONS
; Function Prototype declaration
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; LOCAL STORE/BUFFER/POINTER DEFINITIONS
; Variable declaration - defined here and used outside this module
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; EXTERNAL FUNCTION REFERENCES
; Declare functions defined elsewhere and referenced in this module
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; EXTERNAL GLOBAL STORE/BUFFER/POINTER REFERENCES
; Declare variables used in this module but defined elsewhere
----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/



/*
------------------------------------------------------------------------------
 REVISION HISTORY


 Description:

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

pvmp3_header_sync

Input
    tmp3Bits *inputStream,     structure holding the input stream parameters

------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

    search mp3 sync word

------------------------------------------------------------------------------
 REQUIREMENTS


------------------------------------------------------------------------------
 REFERENCES

------------------------------------------------------------------------------
 PSEUDO-CODE

------------------------------------------------------------------------------
*/

/*----------------------------------------------------------------------------
; FUNCTION CODE
----------------------------------------------------------------------------*/


ERROR_CODE pvmp3_header_sync(tmp3Bits  *inputStream)
{
    uint16 val;
    uint32 availableBits = (inputStream->inputBufferCurrentLength << 3); // in bits

    // microMP3 FIX: byte-align usedBits by rounding up to the next multiple
    // of 8. The upstream "& 8" keeps only bit 3, resetting usedBits to 0 or 8
    // instead of aligning (a typo for "& ~7"). The effect is a logic error
    // (the read cursor rewinds toward the buffer start), not an out-of-bounds
    // read. The scan is also unreachable in the wrapper, which pre-validates
    // sync at offset 0 before pvmp3 sees the buffer. Corrected to "& ~7u".
    inputStream->usedBits = (inputStream->usedBits + 7) & ~7u;

    val = (uint16)getUpTo17bits(inputStream, SYNC_WORD_LNGTH);

    // microMP3 FIX: getUpTo9bits() unconditionally reads 2 bytes
    // (pBuffer[offset] and pBuffer[offset+1]), so we must have at least 2
    // bytes remaining before calling it. The upstream condition only checked
    // usedBits < availableBits, letting the read walk one byte past the
    // buffer end when usedBits was in the final byte. Found by ASan fuzzing.
    while (((val&SYNC_WORD) != SYNC_WORD) && (inputStream->usedBits + 16 <= availableBits))
    {
        val <<= 8;
        val |= getUpTo9bits(inputStream, 8);
    }

    if ((val&SYNC_WORD) == SYNC_WORD && (inputStream->usedBits < availableBits))
    {
        return(NO_DECODING_ERROR);
    }
    else
    {
        return(SYNCH_LOST_ERROR);
    }

}

