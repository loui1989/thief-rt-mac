////////////////////////////////////////////////////////////////////////
// $Source: x:/prj/tech/libsrc/sndsrc/RCS/sndseg.cpp $
// $Author: PATMAC $
// $Date: 1997/10/01 16:39:25 $
// $Revision: 1.5 $
//
// (c) 1997 Looking Glass Technologies Inc.
// Pat McElhatton
//
// Module name: sound segment
// File name: sndseg.cpp
//
// Description: implementation of sound segment objects
//
////////////////////////////////////////////////////////////////////////

#include <lg.h>
#include <sndseg.h>
#include <string.h>

// TBD: stereo ADPCM
// TBD: stereo with frequency doubling

// !!! must decide what sample positions mean when frequency
// !!! doubling is in effect


// Stereo IMA ADPCM (a WAV's format 0x11 with two channels: A Night in
// Rockburg's sounds; LGT's own were all mono, and its decoder left stereo
// "TBD" -- each such sound was decoded as mono, a loud garble). A block: for
// each channel a header (its first sample, 16 bits, its step index, a byte,
// a byte spare), then the channels in turn, 4 bytes (8 samples, low nybble
// first) of each. Decodes a whole block, interleaved; returns its frames.
static uint32
DecodeIMAStereoBlock( const unsigned char *pIn, uint32 nBytes, short *pOut, uint32 maxFrames )
{
   static const int kIndex[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
   static const int kStep[89] = {
      7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
      50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
      253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
      1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
      3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
      12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
   if ( nBytes < 8 || maxFrames == 0 )
      return 0;
   int pred[2], index[2];
   for ( int c = 0; c < 2; c++ ) {
      pred[c] = (short) ( pIn[c * 4] | ( pIn[c * 4 + 1] << 8 ) );
      index[c] = pIn[c * 4 + 2];
      if ( index[c] > 88 ) index[c] = 88;
      pOut[c] = (short) pred[c];
   }
   uint32 frames = 1;
   for ( uint32 at = 8; at + 8 <= nBytes && frames + 8 <= maxFrames; at += 8 ) {
      for ( int c = 0; c < 2; c++ ) {
         const unsigned char *p = pIn + at + c * 4;
         for ( int k = 0; k < 8; k++ ) {
            const int code = ( k & 1 ) ? ( p[k >> 1] >> 4 ) : ( p[k >> 1] & 0xF );
            const int step = kStep[index[c]];
            int diff = step >> 3;
            if ( code & 4 ) diff += step;
            if ( code & 2 ) diff += step >> 1;
            if ( code & 1 ) diff += step >> 2;
            pred[c] += ( code & 8 ) ? -diff : diff;
            if ( pred[c] > 32767 ) pred[c] = 32767;
            else if ( pred[c] < -32768 ) pred[c] = -32768;
            index[c] += kIndex[code];
            if ( index[c] < 0 ) index[c] = 0;
            else if ( index[c] > 88 ) index[c] = 88;
            pOut[( frames + k ) * 2 + c] = (short) pred[c];
         }
      }
      frames += 8;
   }
   return frames;
}

uint32 cSndSegment::mSegsMade = 0;
uint32 cSndSegment::mSegsDestroyed = 0;

EXTERN char *
DecompressIMABlockPartial( char           *pInBuff,
                           short          *pOutBuff,
                           long           nSamples,
                           BOOL           getHeader,
                           uint32         *pState );


cSndSegment::cSndSegment( void )
{
   mInited = FALSE;
   mpInBuffer = NULL;
   mpOutBuffer = NULL;
   mpBlockPcm = NULL;
   mBlockFrames = mBlockAt = mBlockSkip = 0;
   mSegsMade++;
}


cSndSegment::~cSndSegment( void )
{
   if ( mpInBuffer != NULL ) {
      delete mpInBuffer;
   }
   if ( mpOutBuffer != NULL ) {
      delete mpOutBuffer;
   }
   delete [] mpBlockPcm;
   mSegsDestroyed++;
}


void
cSndSegment::InitBase( uint32       offset,
                       sSndAttribs  *pAttribs,
                       BOOL         doDouble )
{
   mStartOffset = offset;
   mSrcOffset = offset;
   mSrcPos = 0;
   mNeedsSeek = TRUE;
   mNewPos = 0;
   mInited = TRUE;
   mAttribs = *pAttribs;
   mDoDoubling = doDouble;
 
   switch( mAttribs.dataType ) {
      case kSndDataPCM:
         mBytesPerSample = (mAttribs.bitsPerSample * mAttribs.nChannels)/ 8;
         break;
      case kSndDataIMAADPCM:
         mBytesPerSample = 2 * mAttribs.nChannels;
         break;
   }

   mBytesLeft = mAttribs.numSamples * mBytesPerSample;

   //
   // create temp input buffer for compressed datatypes
   //
   if ( mAttribs.dataType == kSndDataIMAADPCM ) {
      mpInBuffer = new char[mAttribs.bytesPerBlock];
      if ( mAttribs.nChannels == 2 ) {
         delete [] mpBlockPcm;
         mpBlockPcm = new short[2 * (mAttribs.samplesPerBlock + 8)];
         mBlockFrames = mBlockAt = mBlockSkip = 0;
      }
   }

   //
   // create temp output buffer if frequency doubling is needed
   //
   if ( doDouble ) {
      mBytesLeft <<= 1;
      mpOutBuffer = new char[mBytesLeft];
   }
}

//
// reset # of bytes left in segment when a segment is to be used by
//  two consecutive playlist ops
//
void
cSndSegment::ReInit( uint32   nSamples )
{
   mAttribs.numSamples = nSamples;
   mBytesLeft = mAttribs.numSamples * mBytesPerSample;
   if ( mDoDoubling ) {
      mBytesLeft <<= 1;
   }
}

// ARGH! - when frequency doubling, are SetPosition positions in
// terms of the doubled or original sample numbers?
void
cSndSegment::SetPosition( uint32 newPos )
{
   if ( mDoDoubling ) {
      // force positions to even boundaries when doubling samples
      mNewPos = newPos / 2;
   } else {
      mNewPos = newPos;
   }
   mNeedsSeek = TRUE;
   mBytesLeft = (mAttribs.numSamples - newPos) * mBytesPerSample;
}


//
// how many bytes of output are left in this segment
//
uint32
cSndSegment::BytesLeft( void )
{
   return mBytesLeft;
}


//
// position in source
//
uint32
cSndSegment::GetPosition( void )
{
   return mSrcPos;
}



// NOTE: This only works (currently) for mono streams!
//
// This routine creates a frequency-doubled copy of its source samples.
// The doubling is done by inserting a sample in between the original
// samples which is the average of its neighbor samples.
// Returns the last original sample value, which must be passed back in
// the next time this routine is called
//
static int
doubleSamples( void     *pSrc,
               void     *pDst,
               uint32   nSamples,
               int      bytesPerSample,
               int      oldValue ) 
{
   uint32 i;
   int value;
   short *pSrcW, *pDstW;
   unsigned char  *pSrcB, *pDstB;

   if ( bytesPerSample == 2 ) {
      pSrcW = (short *) pSrc;
      pDstW = (short *) pDst;
      for ( i = 0; i < nSamples; i++ ) {
         value = *pSrcW++;
         *pDstW++ = (short) ((value + oldValue) / 2);
         *pDstW++ = (short) value;
         oldValue = value;
      }
   } else {
      pSrcB = (unsigned char *) pSrc;
      pDstB = (unsigned char *) pDst;
      for ( i = 0; i < nSamples; i++ ) {
         value = (*pSrcB++) - 128;
         *pDstB++ = (unsigned char) ( ((value + oldValue) / 2) + 128 );
         *pDstB++ = (unsigned char) (value + 128);
         oldValue = value;
      }
   }

   return oldValue;
}

// 2 broad ways to handle decompression:
// 1. Always decompress an entire block to a temporary buffer, then
//    spool data out of that buffer.  tmp buffer is maintained betweeen
//    GetData calls, but buffer for input block is only needed during
//    block decompression.
// 2. Only decompress part of buffer which is needed at the moment
//    directly into the destination buffer.  Input block is stored
//    in tmp buffer and must be kept between calls to GetData.
//    Decompressor must be restartable in the middle of a block.
//
// Method 2 seems to be the way to go.  One potential problem with
// it occurs during seeking - when discarding the unneeded samples
// between the nearest block boundary and the seek position, you
// still need somewhere to temporarily write those samples to.  The
// obvious answer would be to write them to the actual destination
// buffer, then overwrite them with the samples from the seek point
// on, but what if the number of samples to skip is larger than the
// number of samples available in the destination buffer?
//
// ARGH! Do we have to worry about odd number of samples when
//  frequency doubling is being done?  Level above LoadBufferIndirect
//  could force sample count to be even when doubling is on, but
//  can we be sure that there is never going to be an odd sample
//  count because of DirectSound buffer wraparound?

void *
cSndSegment::GetData( void    *pDst,
                      uint32  nBytes )
{
   void *pActualDst;
   uint32 skipNSamples = 0;
   uint32 nSamples, nSamps, nSamplesOutNeeded;
   BOOL getHeader;
   short *pOut;
   int blockNum, blockPos;

   nSamples = nBytes / mBytesPerSample;
   if ( mDoDoubling ) {
      // must be called with an even # of samples if doubling
      assert( (nSamples & 1) == 0 );
   }

   // argh - handle block-oriented formats
   // call GetRawData to access media source
   // do ADPCM decompress if needed
   // do frequency doubling if needed

   if ( mAttribs.dataType == kSndDataPCM ) {


      ///////////////////////////////////////////////////
      //
      //                   PCM
      //
      ///////////////////////////////////////////////////
      if ( mNeedsSeek ) {
         // set mSrcOffset to correspond to sample position mNewPos
         mSrcOffset = mStartOffset + (mNewPos * mBytesPerSample);
         mOldValue = 0;
      }
      if ( mDoDoubling ) {
         //TBD!
         pActualDst = GetRawData( mpOutBuffer, nBytes >> 1 );
         mOldValue = doubleSamples( pActualDst, pDst, nSamples,
                                    mAttribs.bitsPerSample, mOldValue );
         pActualDst = pDst;
      } else {
         pActualDst = GetRawData( pDst, nBytes );
      }
      mSrcOffset += nBytes;


   } else if ( mAttribs.dataType == kSndDataIMAADPCM ) {


      ///////////////////////////////////////////////////
      //
      //               IMA ADPCM
      //
      ///////////////////////////////////////////////////

    if ( mAttribs.nChannels == 2 && mpBlockPcm != NULL ) {
      // stereo: block by block (DecodeIMAStereoBlock), frames of 4 bytes
      if ( mNeedsSeek ) {
         blockNum = mNewPos / mAttribs.samplesPerBlock;
         mBlockSkip = mNewPos - blockNum * mAttribs.samplesPerBlock;
         mSrcOffset = mStartOffset + (blockNum * mAttribs.bytesPerBlock);
         mSrcPos = mNewPos;
         mBlockFrames = mBlockAt = 0;
      }
      short *pFrames = (short *) pDst;
      uint32 framesNeeded = nBytes / 4;
      while ( framesNeeded > 0 ) {
         if ( mBlockAt >= mBlockFrames ) {
            const unsigned char *pBlock =
               (const unsigned char *) GetRawData( mpInBuffer, mAttribs.bytesPerBlock );
            mSrcOffset += mAttribs.bytesPerBlock;
            mBlockFrames = DecodeIMAStereoBlock( pBlock, mAttribs.bytesPerBlock, mpBlockPcm,
                                                 mAttribs.samplesPerBlock );
            mBlockAt = mBlockSkip < mBlockFrames ? mBlockSkip : mBlockFrames;
            mBlockSkip = 0;
            if ( mBlockFrames == 0 ) {
               memset( pFrames, 0, framesNeeded * 4 );
               break;
            }
         }
         uint32 n = mBlockFrames - mBlockAt;
         if ( n > framesNeeded )
            n = framesNeeded;
         memcpy( pFrames, mpBlockPcm + mBlockAt * 2, n * 4 );
         pFrames += n * 2;
         mBlockAt += n;
         framesNeeded -= n;
      }
      pActualDst = pDst;
    } else {

      // handle seeking
      if ( mNeedsSeek ) {
         // see how many samples to skip after block start to get
         //   to new position
         blockNum = mNewPos / mAttribs.samplesPerBlock;
         blockPos = blockNum * mAttribs.samplesPerBlock;
         skipNSamples = mNewPos - blockPos;
         // convert from sample position to byte position in input stream
         mSrcOffset = mStartOffset + (blockNum * mAttribs.bytesPerBlock);
         mSrcPos = mNewPos;
         mInSamplesLeft = 0;
         mOldValue = 0;
      }     // end if mNeedsSeek

      pActualDst = pDst;
      if ( mDoDoubling ) {
         pOut = (short *) mpOutBuffer;     // decompress to tmp output buffer
      } else {
         pOut = (short *) pDst;         // decompress directly to app output buffer
      }

      nSamplesOutNeeded = nBytes >> 1;
      while ( nSamplesOutNeeded > 0 ) {

         if ( mInSamplesLeft == 0 ) {
            //
            // current block is empty, get new block
            //
            mInSamplesLeft = mAttribs.samplesPerBlock - skipNSamples;
            mpIn = (char *) GetRawData( mpInBuffer, mAttribs.bytesPerBlock );
            if ( skipNSamples != 0 ) {
               mpIn = DecompressIMABlockPartial( mpIn, pOut, skipNSamples, TRUE,
                                                 &mDecompState );
               skipNSamples = 0;
               getHeader = FALSE;
            } else {
               getHeader = TRUE;
            }
            mSrcOffset += mAttribs.bytesPerBlock;
         } else {
            getHeader = FALSE;
         }

         //
         // decompress a block
         //
         if ( mDoDoubling ) {
            nSamps = ((nSamplesOutNeeded>>1) < mInSamplesLeft)
               ? (nSamplesOutNeeded>>1) : mInSamplesLeft;
         } else {
            nSamps = (nSamplesOutNeeded < mInSamplesLeft) ? nSamplesOutNeeded : mInSamplesLeft;
         }
         mpIn = DecompressIMABlockPartial( mpIn, pOut, nSamps, getHeader,
                                          &mDecompState );
         mInSamplesLeft -= nSamps;

         if ( mDoDoubling ) {
            //
            // do frequency doubling
            //
            mOldValue = doubleSamples( pOut, pDst, nSamps,
                                       mBytesPerSample, mOldValue );

            pDst = (void *) ( ((char *) pDst) + (nSamps << 2) );
            nSamplesOutNeeded -= (nSamps << 1);
         } else {
            pOut += nSamps;
            nSamplesOutNeeded -= nSamps;
         }


      } // end while nSamplesOutNeeded > 0
    }

   }
   assert( mBytesLeft >= nBytes );
   mBytesLeft -= nBytes;
   mNeedsSeek = FALSE;

   return pActualDst;
}

