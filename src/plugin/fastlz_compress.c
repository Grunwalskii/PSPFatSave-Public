/*  
  FastLZ - lightning-fast lossless compression library

  Copyright (C) 2007 Ariya Hidayat (ariya@kde.org)
  Copyright (C) 2006 Ariya Hidayat (ariya@kde.org)
  Copyright (C) 2005 Ariya Hidayat (ariya@kde.org)

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#if !defined(FASTLZ__COMPRESSOR) && !defined(FASTLZ_DECOMPRESSOR)

/*
 * Always check for bound when decompressing.
 * Generally it is best to leave it defined.
 */
#define FASTLZ_SAFE

/*
 * Give hints to the compiler for branch prediction optimization.
 */
#if defined(__GNUC__) && (__GNUC__ > 2)
#define FASTLZ_EXPECT_CONDITIONAL(c)    (__builtin_expect((c), 1))
#define FASTLZ_UNEXPECT_CONDITIONAL(c)  (__builtin_expect((c), 0))
#else
#define FASTLZ_EXPECT_CONDITIONAL(c)    (c)
#define FASTLZ_UNEXPECT_CONDITIONAL(c)  (c)
#endif

/*
 * Use inlined functions for supported systems.
 */
#if defined(__GNUC__) || defined(__DMC__) || defined(__POCC__) || defined(__WATCOMC__) || defined(__SUNPRO_C)
#define FASTLZ_INLINE inline
#elif defined(__BORLANDC__) || defined(_MSC_VER) || defined(__LCC__)
#define FASTLZ_INLINE __inline
#else 
#define FASTLZ_INLINE
#endif

/*
 * Prevent accessing more than 8-bit at once, except on x86 architectures.
 */
#if !defined(FASTLZ_STRICT_ALIGN)
#define FASTLZ_STRICT_ALIGN
#if defined(__i386__) || defined(__386)  /* GNU C, Sun Studio */
#undef FASTLZ_STRICT_ALIGN
#elif defined(__i486__) || defined(__i586__) || defined(__i686__) /* GNU C */
#undef FASTLZ_STRICT_ALIGN
#elif defined(_M_IX86) /* Intel, MSVC */
#undef FASTLZ_STRICT_ALIGN
#elif defined(__386)
#undef FASTLZ_STRICT_ALIGN
#elif defined(_X86_) /* MinGW */
#undef FASTLZ_STRICT_ALIGN
#elif defined(__I86__) /* Digital Mars */
#undef FASTLZ_STRICT_ALIGN
#endif
#endif

/*
 * FIXME: use preprocessor magic to set this on different platforms!
 */
typedef unsigned char  flzuint8;
typedef unsigned short flzuint16;
typedef unsigned int   flzuint32;

/* prototypes */
int fastlz_compress(const void* input, int length, void* output);
int fastlz_compress_level(int level, const void* input, int length, void* output);
int fastlz_decompress(const void* input, int length, void* output, int maxout);

#define MAX_COPY       32
#define MAX_LEN       264  /* 256 + 8 */
#define MAX_DISTANCE 8192

#if !defined(FASTLZ_STRICT_ALIGN)
#define FASTLZ_READU16(p) *((const flzuint16*)(p)) 
#else
#define FASTLZ_READU16(p) ((p)[0] | (p)[1]<<8)
#endif

// 11 (not the FastLZ-default 13): the hash table is htab[HASH_SIZE] pointers =
// 2048*4 = 8KB, which fits in the PSP's 16KB L1 D-cache with room (13 = 32KB, 2x
// the cache, thrashes it). Fewer buckets -> a few % worse ratio, but far fewer
// cache-miss stalls per lookup, so compress is faster. Decompression doesn't use
// the hash table (self-describing stream), so old saves still load unchanged.
#define HASH_LOG  11
#define HASH_SIZE (1<< HASH_LOG)
#define HASH_MASK  (HASH_SIZE-1)
#define HASH_FUNCTION(v,p) { v = FASTLZ_READU16(p); v ^= FASTLZ_READU16(p+1)^(v>>(16-HASH_LOG));v &= HASH_MASK; }

// The 8KB hash table lives in .bss (static), NOT on the caller's stack, so the
// menu thread's stack doesn't have to carry it (lets the menu stack shrink from
// 40KB to 16KB — see main.c). ONE table shared by both level-1/level-2 compressors:
// this file is #included twice, but only one compressor ever runs at a time (a save
// picks level 1 OR 2, never concurrently), so a single static buffer is safe.
static const flzuint8* g_fastlz_htab[HASH_SIZE];

#undef FASTLZ_LEVEL
#define FASTLZ_LEVEL 1

#undef FASTLZ_COMPRESSOR
#undef FASTLZ_DECOMPRESSOR
#define FASTLZ_COMPRESSOR fastlz1_compress
#define FASTLZ_DECOMPRESSOR fastlz1_decompress
static FASTLZ_INLINE int FASTLZ_COMPRESSOR(const void* input, int length, void* output);
static FASTLZ_INLINE int FASTLZ_DECOMPRESSOR(const void* input, int length, void* output, int maxout);
#include "fastlz_compress.c"

/* Level 2 (fastlz2_compress) is intentionally NOT built: the plugin only ever calls
 * fastlz_compress_level(1, ...) — every save is level 1 — so the level-2 compressor was
 * dead code the linker still kept (no --gc-sections). Dropping its self-#include saves
 * ~1KB. (The matching level-2 DECOMPRESSOR is likewise dropped in fastlz_decompress.c;
 * safe because no save file is ever level 2.) */
int fastlz_compress_level(int level, const void* input, int length, void* output)
{
  (void)level;   /* only level 1 is built */
  return fastlz1_compress(input, length, output);
}

#else /* !defined(FASTLZ_COMPRESSOR) && !defined(FASTLZ_DECOMPRESSOR) */

static FASTLZ_INLINE int FASTLZ_COMPRESSOR(const void* input, int length, void* output)
{
  const flzuint8* ip = (const flzuint8*) input;
  const flzuint8* ip_bound = ip + length - 2;
  const flzuint8* ip_limit = ip + length - 12;
  flzuint8* op = (flzuint8*) output;

  const flzuint8** htab = g_fastlz_htab;   // shared static table (.bss), not a stack local
  const flzuint8** hslot;
  flzuint32 hval;

  flzuint32 copy;

  /* sanity check */
  if(FASTLZ_UNEXPECT_CONDITIONAL(length < 4))
  {
    if(length)
    {
      /* create literal copy only */
      *op++ = length-1;
      ip_bound++;
      while(ip <= ip_bound)
        *op++ = *ip++;
      return length+1;
    }
    else
      return 0;
  }

  /* initializes hash table */
  for (hslot = htab; hslot < htab + HASH_SIZE; hslot++)
    *hslot = ip;

  /* we start with literal copy */
  copy = 2;
  *op++ = MAX_COPY-1;
  *op++ = *ip++;
  *op++ = *ip++;

  /* main loop */
  while(FASTLZ_EXPECT_CONDITIONAL(ip < ip_limit))
  {
    const flzuint8* ref;
    flzuint32 distance;

    /* minimum match length */
    flzuint32 len = 3;

    /* comparison starting-point */
    const flzuint8* anchor = ip;

    /* check for a run */
#if FASTLZ_LEVEL==2
    if(ip[0] == ip[-1] && FASTLZ_READU16(ip-1)==FASTLZ_READU16(ip+1))
    {
      distance = 1;
      ip += 3;
      ref = anchor - 1 + 3;
      goto match;
    }
#endif

    /* find potential match */
    HASH_FUNCTION(hval,ip);
    hslot = htab + hval;
    ref = htab[hval];

    /* calculate distance to the match */
    distance = anchor - ref;

    /* update hash table */
    *hslot = anchor;

    /* is this a match? check the first 3 bytes */
    if(distance==0 || 
#if FASTLZ_LEVEL==1
    (distance >= MAX_DISTANCE) ||
#else
    (distance >= MAX_FARDISTANCE) ||
#endif
    *ref++ != *ip++ || *ref++!=*ip++ || *ref++!=*ip++)
      goto literal;

#if FASTLZ_LEVEL==2
    /* far, needs at least 5-byte match */
    if(distance >= MAX_DISTANCE)
    {
      if(*ip++ != *ref++ || *ip++!= *ref++) 
        goto literal;
      len += 2;
    }
    
    match:
#endif

    /* last matched byte */
    ip = anchor + len;

    /* distance is biased */
    distance--;

    if(!distance)
    {
      /* zero distance means a run */
      flzuint8 x = ip[-1];
      while(ip < ip_bound)
        if(*ref++ != x) break; else ip++;
    }
    else
    for(;;)
    {
      /* safe because the outer check against ip limit */
      if(*ref++ != *ip++) break;
      if(*ref++ != *ip++) break;
      if(*ref++ != *ip++) break;
      if(*ref++ != *ip++) break;
      if(*ref++ != *ip++) break;
      if(*ref++ != *ip++) break;
      if(*ref++ != *ip++) break;
      if(*ref++ != *ip++) break;
      while(ip < ip_bound)
        if(*ref++ != *ip++) break;
      break;
    }

    /* if we have copied something, adjust the copy count */
    if(copy)
      /* copy is biased, '0' means 1 byte copy */
      *(op-copy-1) = copy-1;
    else
      /* back, to overwrite the copy count */
      op--;

    /* reset literal counter */
    copy = 0;

    /* length is biased, '1' means a match of 3 bytes */
    ip -= 3;
    len = ip - anchor;

    /* encode the match */
#if FASTLZ_LEVEL==2
    if(distance < MAX_DISTANCE)
    {
      if(len < 7)
      {
        *op++ = (len << 5) + (distance >> 8);
        *op++ = (distance & 255);
      }
      else
      {
        *op++ = (7 << 5) + (distance >> 8);
        for(len-=7; len >= 255; len-= 255)
          *op++ = 255;
        *op++ = len;
        *op++ = (distance & 255);
      }
    }
    else
    {
      /* far away, but not yet in the another galaxy... */
      if(len < 7)
      {
        distance -= MAX_DISTANCE;
        *op++ = (len << 5) + 31;
        *op++ = 255;
        *op++ = distance >> 8;
        *op++ = distance & 255;
      }
      else
      {
        distance -= MAX_DISTANCE;
        *op++ = (7 << 5) + 31;
        for(len-=7; len >= 255; len-= 255)
          *op++ = 255;
        *op++ = len;
        *op++ = 255;
        *op++ = distance >> 8;
        *op++ = distance & 255;
      }
    }
#else

    if(FASTLZ_UNEXPECT_CONDITIONAL(len > MAX_LEN-2))
      while(len > MAX_LEN-2)
      {
        *op++ = (7 << 5) + (distance >> 8);
        *op++ = MAX_LEN - 2 - 7 -2; 
        *op++ = (distance & 255);
        len -= MAX_LEN-2;
      }

    if(len < 7)
    {
      *op++ = (len << 5) + (distance >> 8);
      *op++ = (distance & 255);
    }
    else
    {
      *op++ = (7 << 5) + (distance >> 8);
      *op++ = len - 7;
      *op++ = (distance & 255);
    }
#endif

    /* update the hash at match boundary */
    HASH_FUNCTION(hval,ip);
    htab[hval] = ip++;
    HASH_FUNCTION(hval,ip);
    htab[hval] = ip++;

    /* assuming literal copy */
    *op++ = MAX_COPY-1;

    continue;

    literal:
      *op++ = *anchor++;
      ip = anchor;
      copy++;
      if(FASTLZ_UNEXPECT_CONDITIONAL(copy == MAX_COPY))
      {
        copy = 0;
        *op++ = MAX_COPY-1;
      }
  }

  /* left-over as literal copy */
  ip_bound++;
  while(ip <= ip_bound)
  {
    *op++ = *ip++;
    copy++;
    if(copy == MAX_COPY)
    {
      copy = 0;
      *op++ = MAX_COPY-1;
    }
  }

  /* if we have copied something, adjust the copy length */
  if(copy)
    *(op-copy-1) = copy-1;
  else
    op--;

#if FASTLZ_LEVEL==2
  /* marker for fastlz2 */
  *(flzuint8*)output |= (1 << 5);
#endif

  return op - (flzuint8*)output;
}

#endif /* !defined(FASTLZ_COMPRESSOR) && !defined(FASTLZ_DECOMPRESSOR) */

