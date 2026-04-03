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

   Filename: pvmp3_normalize.cpp

     Date: 10/02/2007

------------------------------------------------------------------------------
 REVISION HISTORY


 Description:

------------------------------------------------------------------------------
 INPUT AND OUTPUT DEFINITIONS

Input
    Int32 x             32-bit integer non-zero input
Returns
    Int32 i             number of leading zeros on x


------------------------------------------------------------------------------
 FUNCTION DESCRIPTION

    Returns number of leading zeros on the non-zero input

------------------------------------------------------------------------------
 REQUIREMENTS


------------------------------------------------------------------------------
 REFERENCES

------------------------------------------------------------------------------
 PSEUDO-CODE

------------------------------------------------------------------------------
*/


/*----------------------------------------------------------------------------
; INCLUDES
----------------------------------------------------------------------------*/

#include "pvmp3_audio_type_defs.h"
#include "pvmp3_normalize.h"

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

#if (defined(PV_ARM_V5)||defined(PV_ARM_V4))
#elif (defined(PV_ARM_GCC_V5)||defined(PV_ARM_GCC_V4))


/* function is inlined in header file */


#else

int32 pvmp3_normalize(int32 x)
{
    return __builtin_clz(x) - 1;
}

#endif
