/*----------------------------------------------------------------------
  File    : fcmat.c
  Contents: data type for functional connectivity matrix (all in one)
  Authors : Kristian Loewe, Christian Borgelt
----------------------------------------------------------------------*/
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

#include "cpuinfo.h"
#include "fcmat1.h"
#include "fcmat2.h"
#include "fcmat3.h"

#ifndef NDEBUG
#line __LINE__ "fcmat.c"
#endif

/*----------------------------------------------------------------------
  Data Type Definition / Recursion Handling
----------------------------------------------------------------------*/
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

/*--------------------------------------------------------------------*/
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
/*--------------------------------------------------------------------*/

#ifndef SFXNAME                 /* macros to generate function names */
#define SFXNAME(n)      SFXNAME_1(n,SUFFIX)
#define SFXNAME_1(n,s)  SFXNAME_2(n,s)
#define SFXNAME_2(n,s)  n##s    /* the two step recursion is needed */
#endif                          /* to ensure proper expansion */

/*----------------------------------------------------------------------
  Function Prototypes (on-demand functions defined in fcmat1.h)
----------------------------------------------------------------------*/
extern REAL fcm_pccotf (FCMAT *fcm, DIM row, DIM col);
extern REAL fcm_pccr2z (FCMAT *fcm, DIM row, DIM col);
extern REAL fcm_tccotf (FCMAT *fcm, DIM row, DIM col);
extern REAL fcm_tccr2z (FCMAT *fcm, DIM row, DIM col);

/*----------------------------------------------------------------------
  Function Prototypes (cache-based functions defined in fcmat2.h)
----------------------------------------------------------------------*/
extern REAL SFXNAME(pcc_pure)  (SFXNAME(FCMAT) *fcm, DIM row, DIM col);
extern REAL SFXNAME(pcc_r2z)   (SFXNAME(FCMAT) *fcm, DIM row, DIM col);
extern REAL SFXNAME(tcc_pure)  (SFXNAME(FCMAT) *fcm, DIM row, DIM col);
extern REAL SFXNAME(tcc_r2z)   (SFXNAME(FCMAT) *fcm, DIM row, DIM col);
extern void SFXNAME(rec_rct)   (SFXNAME(WORK) *w,
                               DIM ra, DIM rb, DIM ca, DIM cb);
extern void SFXNAME(rec_trg)   (SFXNAME(WORK) *w, DIM a, DIM b);
extern WORKERDEF(fill, p);
extern int  SFXNAME(fcm_fill)  (SFXNAME(FCMAT) *fcm, DIM row, DIM col);
extern REAL SFXNAME(fcm_cache) (SFXNAME(FCMAT) *fcm, DIM row, DIM col);

/*----------------------------------------------------------------------
  Function Prototypes (half-stored functions defined in fcmat3.h)
----------------------------------------------------------------------*/
extern REAL SFXNAME(fcm_full)     (SFXNAME(FCMAT) *fcm, DIM row, DIM col);
extern REAL SFXNAME(fcm_full_r2z) (SFXNAME(FCMAT) *fcm, DIM row, DIM col);

/*----------------------------------------------------------------------
  Functions
----------------------------------------------------------------------*/

SFXNAME(FCMAT)* SFXNAME(fcm_create) (REAL *data, DIM V, DIM T, int mode, ...)
{                               /* --- create a func. connect. matrix */
  SFXNAME(FCMAT)    *fcm;       /* func. con. matrix to be created */
  SFXNAME(FCMGETFN) *get;       /* element computation function */
  SFXNAME(WORK)     *w;         /* to initialize the worker data */
  WORKER  *worker;              /* worker for parallel execution */
  int     i, n;                 /* loop variable for threads */
  va_list args;                 /* list of variable arguments */
  size_t  z;                    /* cache size */
  #ifdef RECTGRID               /* if to split rectangle into grid */
  DIM     g;                    /* loop variable for grid size */
  #endif

  assert(data && (V > 1) && (T > 1)); /* check the function arguments */
  fcm = (SFXNAME(FCMAT)*)malloc(sizeof(SFXNAME(FCMAT)));
  if (!fcm) return NULL;        /* allocate the base structure */
  fcm->V       = V;             /* note the number of voxels */
  fcm->T       = T;             /* and  the number of scans */
  fcm->X       = T;             /* default: data blocks like scans */
  fcm->tile    = -1;            /* default: auto-determine */
  fcm->maxmem  = -1;            /* default: auto-determine */
  fcm->nthd    = -1;            /* default: auto-determine */
  fcm->mode    = mode;          /* note the processing mode */
  fcm->mem     = NULL;          /* clear memory block, */
  fcm->cmap    = NULL;          /* cosine map and cache */
  fcm->cache   = NULL;          /* for easier cleanup */
  fcm->diag    = (REAL)((mode & FCM_R2Z) ? R2Z_MAX : 1.0);
  fcm->value   = fcm->diag;     /* set the value and coordinates */
  fcm->row     = fcm->col = 0;  /* of the current element */
  fcm->ra      = 0; fcm->rb = V;
  fcm->ca      = 0; fcm->cb = V;/* init. the coordinate ranges */
  fcm->err     = 0;             /* clear the error status */
  fcm->threads = NULL;          /* clear thread handles and */
  fcm->work    = NULL;          /* worker data for easier cleanup */
  #ifndef _WIN32                /* not yet available for Windows */
  fcm->join    = 1;             /* set the thread join flag */
  #endif                        /* (default, to be changed later) */
  #ifdef FCM_BENCH              /* if to do some benchmarking */
  fcm->sum     = fcm->beg = fcm->end = 0;
  fcm->cnt     = 0;             /* initialize benchmark variables */
  #endif                        /* (for time loss computation) */
  DBGMSG("T: %d  N: %d  P: %4d  C: %d  maxmem [GiB]: %f\n",
          fcm->T, fcm->V, fcm->nthd, fcm->tile, fcm->maxmem);

  /* variable arguments */
  va_start(args, mode);

  if (mode & FCM_THREAD)        /* number of threads */
    fcm->nthd = va_arg(args, int);
  assert((fcm->nthd == -1) || ((fcm->nthd >= 1) && (fcm->nthd <= 1024)));
  if (fcm->nthd == 0)           /* nthd = 0 is not allowed */
    fcm->nthd = 1;

  if (mode & FCM_CACHE)         /* cache parameter (tile/cache size) */
    fcm->tile = va_arg(args, int);
  assert((fcm->tile == -1) || ((fcm->tile >= 0) && (fcm->tile <= V)));

  if (mode & FCM_MAXMEM)        /* max. amount of memory */
    fcm->maxmem = va_arg(args, double);

  va_end(args);
  DBGMSG("T: %d  N: %d  P: %4d  C: %d  maxmem [GiB]: %f\n",
          fcm->T, fcm->V, fcm->nthd, fcm->tile, fcm->maxmem);

  /* number of threads */
  if (fcm->nthd == -1)          /* if auto-determine */
    fcm->nthd = proccnt();      /* use number of logical processors */

  /* max memory */
  if (fcm->maxmem < 0)          /* if auto-determine */
    fcm->maxmem = 2.0;          /* use 2 GiB */
  fcm->maxmem = fcm->maxmem * 1024*1024*1024;
  DBGMSG("maxmem [B]: %f\n", fcm->maxmem);

  /* cache size */
  size_t E = (size_t)V * (size_t)(V-1)/2;
  if (fcm->tile == fcm->V) {    /* if half-stored */
    if ((E * sizeof(REAL)) > fcm->maxmem) {
      if (fcm->nthd <= 6)
        fcm->tile = 1024;
      else
        fcm->tile = 0;
      WARNING("fcm->tile has been set to %d to enforce the specified"
              "memory limit.\n", fcm->tile);
    }
  }
  if ((fcm->tile < V)           /* if cache-based */
      && (fcm->tile > 0)) {
    if (((size_t)fcm->tile *(size_t)fcm->tile *sizeof(REAL)) > fcm->maxmem) {
      fcm->tile = 0;
      WARNING("fcm->tile has been set to %d to enforce the specified"
              "memory limit.\n", fcm->tile);
    }
  }
  if (fcm->tile == -1) {        /* if auto-determine */
    if ((E * sizeof(REAL)) <= fcm->maxmem)
      fcm->tile = fcm->V;       /* use half-stored */
    else if ((1024*1024 * sizeof(REAL) <= fcm->maxmem) && (fcm->nthd <= 6))
      fcm->tile = 1024;         /* use cache-based (tile size: 1024) */
    else
      fcm->tile = 0;            /* use on-demand */
  }
  DBGMSG("T: %d  N: %d  P: %4d  C: %d  maxmem [B]: %f\n",
          fcm->T, fcm->V, fcm->nthd, fcm->tile, fcm->maxmem);
  assert((fcm->nthd >= 1) && (fcm->nthd <= 1024));
  assert((fcm->tile >= 0) && (fcm->tile <= fcm->V));
  assert((fcm->maxmem >= 0));

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

  if      (fcm->tile <= 0)      /* if computation on the fly */
    fcm->cget = fcm->get;       /* get the element retrieval function */
  else if (fcm->tile <  V) {    /* if to cache smaller areas */
    z = (size_t)fcm->tile *(size_t)fcm->tile;
    fcm->cache    = (REAL*)  malloc(z *sizeof(REAL));
    fcm->threads  = (THREAD*)malloc((size_t)fcm->nthd *sizeof(THREAD));
    fcm->work = w = malloc((size_t)fcm->nthd *sizeof(SFXNAME(WORK)));
    if (!fcm->cache || !fcm->threads || !fcm->work)  {
      SFXNAME(fcm_delete)(fcm); return NULL; }
    if (!(fcm->mode & FCM_R2Z)) /* if to compute pure corr. coeffs. */
      get = (mode == FCM_PCC) ? SFXNAME(pcc_pure) : SFXNAME(tcc_pure);
    else                        /* if to apply Fisher's r to z trans. */
      get = (mode == FCM_PCC) ? SFXNAME(pcc_r2z)  : SFXNAME(tcc_r2z);
    for (i = 0; i < fcm->nthd; i++) {
      w[i].work = 0;            /* clear assigned work flag */
      w[i].fcm  = fcm;          /* store func. con. matrix object */
      w[i].get  = get;          /* and the function that */
    }                           /* computes a matrix element */
    #ifndef _WIN32              /* not yet available for Windows */
    fcm->join = ((fcm->nthd <= 1) || (fcm->mode & FCM_JOIN));
    if (!fcm->join) {           /* if to block and signal threads */
      pthread_mutex_init(&fcm->mutex,     NULL);
      pthread_cond_init (&fcm->cond_idle, NULL);
      pthread_cond_init (&fcm->cond_work, NULL);
      fcm->idle = 0;            /* init. thread synchronization */
      worker = SFXNAME(fill);   /* get the worker function */
      for (n = 0; n < fcm->nthd; n++)
        if (pthread_create(fcm->threads+n, NULL, worker, w+n) != 0) {
          fcm->err = -1; break;}/* start the worker threads */
      pthread_mutex_lock(&fcm->mutex);
      while (fcm->idle < n)     /* wait for all threads to start */
        pthread_cond_wait(&fcm->cond_idle, &fcm->mutex);
      pthread_mutex_unlock(&fcm->mutex);
      if (fcm->err) {           /* if starting the threads failed */
        fcm->nthd = n; SFXNAME(fcm_delete)(fcm); return NULL; }
    }                           /* delete the matrix and abort */
    #endif
    #ifdef RECTGRID             /* if to split rectangle into grid */
    for (g = (DIM)floor(sqrt((REAL)fcm->nthd)); g > 1; g--)
      if (g *(fcm->nthd/g) == fcm->nthd)
        break;                  /* compute factors as close as */
    fcm->gc = g;                /* possible to the square root */
    fcm->gr = fcm->nthd/g;      /* for the rectangle grid */
    #endif
    fcm->cget = SFXNAME(fcm_cache);
    fcm->ra = fcm->rb = -1;     /* get element retrieval function and */
    fcm->ca = fcm->cb = -1; }   /* invalidate row and column range */
  else {                        /* if to cache the whole matrix */
    z = (size_t)V *(size_t)(V-1)/2; /* cache for upper triangle */
    fcm->cache = (REAL*)malloc(z *sizeof(REAL));
    if (!fcm->cache) { SFXNAME(fcm_delete)(fcm); return NULL; }
    if (mode == FCM_PCC)        /* if Pearson correlation cofficient */
      SFXNAME(pccx)    (data, fcm->cache, (int)V, (int)T,
                        PCC_AUTO|PCC_THREAD, fcm->nthd);
    else                        /* if tetrachoric correlation coeff. */
      SFXNAME(tetraccx)(data, fcm->cache, (int)V, (int)T,
                        TCC_AUTO|TCC_THREAD, fcm->nthd);
    if (!(fcm->mode & FCM_R2Z)) {/* if to compute pure corr. coeffs. */
      fcm->cget = SFXNAME(fcm_full);
      fcm->get = SFXNAME(fcm_full); }
    else {                       /* if to apply Fisher's r to z trans. */
      fcm->cget = SFXNAME(fcm_full_r2z);
      fcm->get = SFXNAME(fcm_full_r2z);
    }                            /* set the element retrieval function */
  }
  return fcm;                   /* return created FC matrix */
} /* fcm_create() */

/*--------------------------------------------------------------------*/

void SFXNAME(fcm_delete) (SFXNAME(FCMAT) *fcm)
{                               /* --- delete a func. connect. matrix */
  assert(fcm);                  /* check the function argument */
  #ifdef FCM_BENCH              /* if to do some benchmarking */
  double loss = fcm->beg +fcm->end;
  if (fcm->cnt <= 0) fcm->cnt = 1;
  fprintf(stderr, "fcm_delete()\n");
  fprintf(stderr, "%d tiles\n", fcm->cnt);
  fprintf(stderr, "time: %10.6f (%10.6f)\n",
          fcm->sum, fcm->sum/(double)fcm->cnt);
  fprintf(stderr, "loss: %10.6f (%10.6f/%10.6f)\n",
          loss,     loss    /(double)fcm->nthd,
          loss    /(double)fcm->cnt);
  fprintf(stderr, "beg : %10.6f (%10.6f/%10.6f)\n",
          fcm->beg, fcm->beg/(double)fcm->nthd,
          fcm->beg/(double)fcm->cnt);
  fprintf(stderr, "end : %10.6f (%10.6f/%10.6f)\n",
          fcm->end, fcm->end/(double)fcm->nthd,
          fcm->end/(double)fcm->cnt);
  #endif                        /* print benchmarking information */
  #ifndef _WIN32                /* not yet available for Windows */
  if (!fcm->join) {             /* if to block and signal threads */
    int i;                      /* loop variable for threads */
    SFXNAME(WORK) *w = fcm->work;     /* get the worker data */
    pthread_mutex_lock(&fcm->mutex);
    for (i = 0; i < fcm->nthd; i++)
      w[i].work = -1;           /* set the thread stop indicators */
    fcm->idle = 0;              /* signal that work was assigned */
    pthread_cond_broadcast(&fcm->cond_work);
    pthread_mutex_unlock(&fcm->mutex);
    for (i = 0; i < fcm->nthd; i++) /* wait for all threads to finish */
      pthread_join(fcm->threads[i], NULL);
    pthread_cond_destroy (&fcm->cond_work);
    pthread_cond_destroy (&fcm->cond_idle);
    pthread_mutex_destroy(&fcm->mutex);
  }                             /* clean up thread synchronization */
  #endif
  if (fcm->work)    free(fcm->work);
  if (fcm->threads) free(fcm->threads);
  if (fcm->cache)   free(fcm->cache);
  if (fcm->cmap)    free(fcm->cmap);
  if (fcm->mem)     free(fcm->mem);/* delete cache, cosine map, */
  free(fcm);                    /* data, and the base structure */
} /* fcm_delete() */

/*----------------------------------------------------------------------
  Recursion Handling
----------------------------------------------------------------------*/
#if _FCM_PASS == 1              /* if in first of two passes */
#undef REAL
#undef SUFFIX
#include "fcmat.c"              /* process file recursively */
#endif
