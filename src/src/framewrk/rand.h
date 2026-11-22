/*
@Copyright Looking Glass Studios, Inc.
1996,1997,1998,1999,2000 Unpublished Work.
*/

///////////////////////////////////////////////////////////////////////////////
// $Source: r:/t2repos/thief2/src/framewrk/rand.h,v $
// $Author: adurant $
// $Date: 2000/01/31 09:47:05 $
// $Revision: 1.3 $
//
#pragma once

#ifndef __RAND_H
#define __RAND_H

EXTERN int Rand();
EXTERN int RandRange(int low, int high);
EXTERN int UnrecordedRand();

// The top of Rand() and UnrecordedRand(): 15 bits, as Visual C's rand()
// gave. The game's sums divide by the C library's RAND_MAX expecting that;
// the C libraries here have 31 bits, and every such random draw came out
// at (nearly) its least -- random lights stuck at their dimmest, the AI's
// hearing and sight always at the low end of their variation, the first
// of a set of motions always chosen. Divide by this instead.
#define DARK_RAND_MAX 0x7fff

#endif /* !__RAND_H */
