/*----------------------------------------------------------------------------
  File    : fcmat1.c
  Contents: data type for functional connectivity matrix (on-demand)
  Author  : Kristian Loewe, Christian Borgelt
----------------------------------------------------------------------------*/
#ifndef _WIN32                  /* if Linux/Unix system */
#define _POSIX_C_SOURCE 200809L /* needed for clock_gettime() */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>
#include <math.h>

#ifdef __SSE2__
#include <emmintrin.h>
#endif
#ifdef __AVX__
#include <immintrin.h>
#endif

#include "binarize.h"
#include "fcmat1.h"
// #include "mex.h"

/*----------------------------------------------------------------------------
  Data Type Definition / Recursion Handling
----------------------------------------------------------------------------*/
#ifdef REAL                     /* if REAL is defined, */
#  undef  _FCM_PASS             /* ensure _FCM_PASS is undefined */
#  define _FCM_PASS 0           /* define macro for single pass */
#  ifndef SUFFIX                /* function names get no suffix */
#  define SUFFIX                /* (only single set of functions) */
#  endif
#elif !defined _FCM_PASS        /* if in first pass of two */
#  undef  _FCM_PASS             /* ensure _FCM_PASS is undefined */
#  define _FCM_PASS 1           /* define macro for first pass */
#  define REAL      float       /* first pass: single precision */
#  define SUFFIX    _flt        /* function name suffix is '_flt' */
#else                           /* if in second pass of two */
#  undef  _FCM_PASS             /* ensure _FCM_PASS is undefined */
#  define _FCM_PASS 2           /* define macro for second pass */
#  define REAL      double      /* second pass: double precision */
#  define SUFFIX    _dbl        /* function name suffix is '_dbl' */
#endif

/*--------------------------------------------------------------------------*/
#define float  1                /* to check the definition of REAL */
#define double 2

#if   REAL == float             /* if single precision data */
#undef  REAL_IS_DOUBLE
#define REAL_IS_DOUBLE  0       /* clear indicator for double */
#elif REAL == double            /* if double precision data */
#undef  REAL_IS_DOUBLE
#define REAL_IS_DOUBLE  1       /* set   indicator for double */
#else
#error "REAL must be either 'float' or 'double'"
#endif

#undef float                    /* delete definitions */
#undef double                   /* used for type checking */
/*--------------------------------------------------------------------------*/
#define int         1           /* to check definitions */
#define long        2           /* for certain types */
#define ptrdiff_t   3

#if   DIM == int
#ifndef DIM_FMT
#define DIM_FMT     "d"         /* printf format code for int */
#endif
#ifndef strtodim
#define strtodim(s,p)   (int)strtol(s,p,0)
#endif

#elif DIM == long
#ifndef DIM_FMT
#define DIM_FMT     "ld"        /* printf format code for long */
#endif
#ifndef strtodim
#define strtodim(s,p)   strtol(s,p,0)
#endif

#elif DIM == ptrdiff_t
#ifndef DIM_FMT
#  ifdef _MSC_VER
#  define DIM_FMT   "Id"        /* printf format code for ptrdiff_t */
#  else
#  define DIM_FMT   "td"        /* printf format code for ptrdiff_t */
#  endif                        /* MSC still does not support C99 */
#endif
#ifndef strtodim
#define strtodim(s,p)   (ptrdiff_t)strtoll(s,p,0)
#endif

#else
#error "DIM must be either 'int', 'long', or 'ptrdiff_t'"
#endif

#undef int                      /* remove preprocessor definitions */
#undef long                     /* needed for the type checking */
#undef ptrdiff_t
/*--------------------------------------------------------------------------*/

#ifndef SFXNAME                 /* macros to generate function names */
#define SFXNAME(n)      SFXNAME_1(n,SUFFIX)
#define SFXNAME_1(n,s)  SFXNAME_2(n,s)
#define SFXNAME_2(n,s)  n##s    /* the two step recursion is needed */
#endif                          /* to ensure proper expansion */

/*----------------------------------------------------------------------------
  Preprocessor Definitions
----------------------------------------------------------------------------*/
#if   defined __AVX__           /* if AVX instructions available */
#  define INIT_PCC SFXNAME(init_avx)
#  define PAIR_PCC SFXNAME(pair_avx)
#elif defined __SSE2__          /* if SSE2 instructions available */
#  define INIT_PCC SFXNAME(init_sse2)
#  define PAIR_PCC SFXNAME(pair_sse2)
#else                           /* if neither extension available */
#  define INIT_PCC SFXNAME(init_naive)
#  define PAIR_PCC SFXNAME(pair_naive)
#endif                          /* fall back to naive computations */

#if defined __POPCNT__ && defined __SSE4_1__
#  define PCAND_TCC pcand_m128i /* use m128i implementation */
#  define BPI       128         /* 128 bits per integer */
#else
#  define PCAND_TCC pcand_lut16 /* use lut16 implemenation */
#  define BPI       32          /* 32 bits per integer */
#endif

#define INDEX(i,j,N)    ((size_t)(i)*((size_t)(N)+(size_t)(N) \
                        -(size_t)(i)-3)/2-1+(size_t)(j))

/*----------------------------------------------------------------------------
  Function Prototypes
----------------------------------------------------------------------------*/
extern REAL SFXNAME(fcm_pccotf) (FCMAT *fcm, DIM row, DIM col);
extern REAL SFXNAME(fcm_pccr2z) (FCMAT *fcm, DIM row, DIM col);
extern REAL SFXNAME(fcm_tccotf) (FCMAT *fcm, DIM row, DIM col);
extern REAL SFXNAME(fcm_tccr2z) (FCMAT *fcm, DIM row, DIM col);

/*----------------------------------------------------------------------------
  Functions
----------------------------------------------------------------------------*/

SFXNAME(FCMAT)* SFXNAME(fcm_create) (REAL *data, DIM V, DIM T, int mode, ...)
{                               /* --- create a func. connect. matrix */
  SFXNAME(FCMAT) *fcm;          /* func. con. matrix to be created */

  assert(data && (V > 1) && (T > 1)); /* check the function arguments */
  fcm = (SFXNAME(FCMAT)*)malloc(sizeof(SFXNAME(FCMAT)));
  if (!fcm) return NULL;        /* allocate the base structure */
  fcm->V       = V;             /* note the number of voxels */
  fcm->T       = T;             /* and  the number of scans */
  fcm->X       = T;             /* default: data blocks like scans */
  fcm->mode    = mode;          /* note the processing mode */
  fcm->mem     = NULL;          /* clear memory block, */
  fcm->cmap    = NULL;          /* cosine map and cache */
  fcm->diag    = (REAL)((mode & FCM_R2Z) ? R2Z_MAX : 1.0);
  fcm->value   = fcm->diag;     /* set the value and coordinates */
  fcm->row     = fcm->col = 0;  /* of the current element */
  fcm->ra      = 0; fcm->rb = V;
  fcm->ca      = 0; fcm->cb = V;/* init. the coordinate ranges */
  fcm->err     = 0;             /* clear the error status */

  mode &= FCM_CORR;             /* get the correlation type */
  if      (mode == FCM_PCC) {   /* if Pearson's correlation coeff. */
    #ifdef __AVX__              /* use AVX if possible */
    #if REAL_IS_DOUBLE          /* if to use double precision values */
    fcm->X = (((int)T+3) & ~3); /* process blocks with 4 numbers */
    #else                       /* if to use single precision values */
    fcm->X = (((int)T+7) & ~7); /* process blocks with 8 numbers */
    #endif
    #else                       /* otherwise fall back to SSE2 */
    #if REAL_IS_DOUBLE          /* if to use double precision values */
    fcm->X = (((int)T+1) & ~1); /* process blocks with 2 numbers */
    #else                       /* if to use single precision values */
    fcm->X = (((int)T+3) & ~3); /* process blocks with 4 numbers */
    #endif                      /* get the data block size */
    #endif                      /* allocate memory for norm.ed data */
    fcm->mem = malloc((size_t)V*(size_t)fcm->X *sizeof(REAL) +31);
    if (!fcm->mem) { SFXNAME(fcm_delete)(fcm); return NULL; }
    fcm->data = (REAL*)(((uintptr_t)fcm->mem +31) & ~(uintptr_t)31);
    INIT_PCC(data, (int)V, (int)T, fcm->data, (int)fcm->X); }
  else if (mode == FCM_TCC) {   /* if tetrachoric correlation coeff. */
    #if defined __POPCNT__ \
    &&  defined __SSE4_1__      /* use 128 bit and popcnt if possible */
    fcm->X = 4 *(((int)T+127) >> 7);
    #else                       /* otherwise fall back to LUT16 */
    fcm->X = ((int)T+31) >> 5;  /* get block size of binarized data */
    #endif
    fcm->mem  =                 /* allocate memory for binarized data */
    fcm->data = SFXNAME(binarize)(data, (int)V, (int)T, BIN_MEDIAN,BPI);
    if (!fcm->data) { SFXNAME(fcm_delete)(fcm); return NULL; };
    fcm->cmap = SFXNAME(make_cmap)((int)T); /* create cosine map */
    if (!fcm->cmap) { SFXNAME(fcm_delete)(fcm); return NULL; };
    init_popcnt(); }            /* initialize bit count table */
  else {                        /* if unknown correlation variant */
    fprintf(stderr, "fcm_create: unknown correlation variant\n");
    free(fcm); return NULL;     /* print an error message */
  }                             /* and abort the function */

  if (!(fcm->mode & FCM_R2Z))   /* if pure correlation coefficients */
    fcm->get = (mode == FCM_PCC)
             ? SFXNAME(fcm_pccotf) : SFXNAME(fcm_tccotf);
  else {                        /* if to apply Fisher's r to z trans. */
    fcm->get = (mode == FCM_PCC)
             ? SFXNAME(fcm_pccr2z) : SFXNAME(fcm_tccr2z);
  }                             /* get the element retrieval function */
  fcm->cget = fcm->get;         /* get the element retrieval function */

  return fcm;                   /* return created FC matrix */
}  /* fcm_create() */

/*--------------------------------------------------------------------------*/

void SFXNAME(fcm_delete) (SFXNAME(FCMAT) *fcm)
{                               /* --- delete a func. connect. matrix */
  assert(fcm);                  /* check the function argument */
  if (fcm->cmap)  free(fcm->cmap);
  if (fcm->mem)   free(fcm->mem);/* delete cache, cosine map, */
  free(fcm);                    /* data, and the base structure */
}  /* fcm_delete() */

/*--------------------------------------------------------------------------*/

int SFXNAME(fcm_first) (SFXNAME(FCMAT) *fcm)
{                               /* --- get first matrix element */
  assert(fcm);                  /* check the function argument */
  fcm->err = 0;                 /* clear the error status */
  fcm->row = 0; fcm->col = 1;   /* start with first off-diag. element */
  fcm->value = fcm->cget(fcm, 0, 1);
  return (fcm->err) ? -1 : +1;  /* store the value and return status */
}  /* fcm_first() */

/*--------------------------------------------------------------------------*/

int SFXNAME(fcm_next) (SFXNAME(FCMAT) *fcm)
{                               /* --- get next matrix element */
  assert(fcm);                  /* check the function argument */
  if (fcm->col >= fcm->V)       /* if traversal is already finished, */
    return 0;                   /* abort the function with failure */
  fcm->col += 1;                /* go to the next column */
  if (fcm->col >= fcm->cb) {    /* if at the end of a cache row, */
    fcm->col  = fcm->ca;        /* return to the start of a row */
    fcm->row += 1;              /* and go to the next row */
    if (fcm->col <= fcm->row)   /* if in the lower triangle, */
      fcm->col = fcm->row+1;    /* go to the upper triangle */
    if (fcm->col >= fcm->cb) {  /* if at the end of a strip */
      fcm->row = 0;             /* start a new strip */
      fcm->col = fcm->cb;       /* (row 0, first column after strip) */
      if (fcm->col >= fcm->V) return 0;
    }                           /* if whole matrix is traversed, */
  }                             /* abort the function with failure */
  fcm->value = fcm->cget(fcm, fcm->row, fcm->col);
  return (fcm->err) ? -1 : +1;  /* store the value and return status */
}  /* fcm_next() */

/*--------------------------------------------------------------------------*/

void SFXNAME(fcm_show) (SFXNAME(FCMAT) *fcm)
{                               /* --- show funct. connect. matrix */
  assert(fcm);                  /* check the function argument */
  printf("     ");              /* start the matrix output */
  for (DIM col = 0; col < fcm->V; col++)
    printf(" %-8"DIM_FMT, col); /* print the header line */
  printf("\n");                 /* (column indices) */
  for (DIM row = 0; row < fcm->V; row++) {
    printf("%-5"DIM_FMT, row);  /* print the row index */
    for (DIM col = 0; col < fcm->V; col++)
      printf(" %-8.3g", SFXNAME(fcm_get)(fcm, row, col));
    printf("\n");               /* traverse the matrix rows */
  }                             /* print the matrix elements */
}  /* fcm_show() */

/*----------------------------------------------------------------------------
  Recursion Handling
----------------------------------------------------------------------------*/
#if _FCM_PASS == 1              /* if in first of two passes */
#undef REAL
#undef SUFFIX
#include "fcmat1.c"             /* process file recursively */
#endif