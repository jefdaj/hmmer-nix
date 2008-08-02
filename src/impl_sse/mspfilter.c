/* The MSP filter implementation; SSE version.
 * 
 * A "filter" is a one-row, O(M), DP implementation that calculates
 * an approximated nat score (i.e. in limited precision - uchar, for 
 * example) and may have limited numeric range. It will return 
 * <eslERANGE> if its numeric range is exceeded, in which case the caller
 * will have to obtain the score by another (probably slower) method.
 * 
 * Contents:
 *   1. p7_MSPFilter() implementation.
 *   2. Benchmark driver.
 *   3. Unit tests
 *   4. Test driver
 *   5. Example
 *   6. Copyright and license information
 * 
 * SRE, Sun Nov 25 11:26:48 2007 [Casa de Gatos]
 * SVN $Id: impl_sse.c 2509 2008-07-30 14:45:52Z eddys $
 */
#include "p7_config.h"

#include <stdio.h>
#include <math.h>

#include <xmmintrin.h>		/* SSE  */
#include <emmintrin.h>		/* SSE2 */

#include "easel.h"
#include "esl_sse.h"

#include "hmmer.h"
#include "impl_sse.h"

/*****************************************************************
 * 1. The p7_MSPFilter() DP implementation.
 *****************************************************************/

/* Function:  p7_MSPFilter()
 * Synopsis:  Calculates MSP score, vewy vewy fast, in limited precision.
 * Incept:    SRE, Wed Dec 26 15:12:25 2007 [Janelia]
 *
 * Purpose:   Calculates an approximation of the MSP score for sequence
 *            <dsq> of length <L> residues, using optimized profile <om>,
 *            and a preallocated one-row DP matrix <ox>. Return the 
 *            estimated MSP score (in nats) in <ret_sc>.
 *            
 *            Score may overflow (and will, on high-scoring
 *            sequences), but will not underflow. 
 *            
 *            The model may be in any mode, because only its match
 *            emission scores will be used. The MSP filter inherently
 *            assumes a multihit local mode, and uses its own special
 *            state transition scores, not the scores in the profile.
 *
 * Args:      dsq     - digital target sequence, 1..L
 *            L       - length of dsq in residues          
 *            om      - optimized profile
 *            ox      - DP matrix
 *            ret_sc  - RETURN: MSP score (in nats)          
 *                      
 * Note:      We misuse the matrix <ox> here, using only a third of the
 *            first dp row, accessing it as <dp[0..Q-1]> rather than
 *            in triplets via <{MDI}MX(q)> macros, since we only need
 *            to store M state values. We know that if <ox> was big
 *            enough for normal DP calculations, it must be big enough
 *            to hold the MSPFilter calculation.
 *
 * Returns:   <eslOK> on success.
 *            <eslERANGE> if the score overflows the limited range; in
 *            this case, this is a high-scoring hit.
 *
 * Throws:    <eslEINVAL> if <ox> allocation is too small.
 */
int
p7_MSPFilter(const ESL_DSQ *dsq, int L, const P7_OPROFILE *om, P7_OMX *ox, float *ret_sc)
{
  register __m128i mpv;            /* previous row values                                       */
  register __m128i xEv;		   /* E state: keeps max for Mk->E as we go                     */
  register __m128i xBv;		   /* B state: splatted vector of B[i-1] for B->Mk calculations */
  register __m128i sv;		   /* temp storage of 1 curr row value in progress              */
  register __m128i biasv;	   /* emission bias in a vector                                 */
  uint8_t  xE, xB, xC;             /* special states' scores                                    */
  int i;			   /* counter over sequence positions 1..L                      */
  int q;			   /* counter over vectors 0..nq-1                              */
  int Q        = p7O_NQU(om->M);   /* segment length: # of vectors                              */
  __m128i *dp  = ox->dpu[0];	   /* we're going to use dp[0][0..q..Q-1], not {MDI}MX(q) macros*/
  __m128i *rsc;			   /* will point at om->ru[x] for residue x[i]                  */

  /* Check that the DP matrix is ok for us. */
  if (Q > ox->allocQ16)  ESL_EXCEPTION(eslEINVAL, "DP matrix allocated too small");
  ox->M   = om->M;

  /* Initialization. In offset unsigned arithmetic, -infinity is 0, and 0 is om->base.
   */
  biasv = _mm_set1_epi8((int8_t) om->bias); /* yes, you can set1() an unsigned char vector this way */
  for (q = 0; q < Q; q++)
    dp[q] = _mm_setzero_si128();
  xB   = om->base - om->tjb;                /* remember, all values are costs to be subtracted. */
  xC   = 0;

#if p7_DEBUGGING
  if (ox->debugging) p7_omx_DumpMSPRow(ox, 0, 0, 0, xC, xB, xC);
#endif

  for (i = 1; i <= L; i++)
    {
      rsc = om->rm[dsq[i]];
      xEv = _mm_setzero_si128();      
      xBv = _mm_set1_epi8((int8_t) (xB - om->tbm));

      /* Right shifts by 1 byte. 4,8,12,x becomes x,4,8,12. 
       * Because ia32 is littlendian, this means a left bit shift.
       * Zeros shift on automatically, which is our -infinity.
       */
      mpv = _mm_slli_si128(dp[Q-1], 1);   
      for (q = 0; q < Q; q++)
	{
	  /* Calculate new MMX(i,q); don't store it yet, hold it in sv. */
	  sv   = _mm_max_epu8(mpv, xBv);
	  sv   = _mm_adds_epu8(sv, biasv);     
	  sv   = _mm_subs_epu8(sv, *rsc);   rsc++;
	  xEv  = _mm_max_epu8(xEv, sv);	

	  mpv   = dp[q];   	  /* Load {MDI}(i-1,q) into mpv */
	  dp[q] = sv;       	  /* Do delayed store of M(i,q) now that memory is usable */
	}	  

      /* Now the "special" states, which start from Mk->E (->C, ->J->B) */
      xE = esl_sse_hmax_epu8(xEv);
      if (xE >= 255 - om->bias) { *ret_sc = eslINFINITY; return eslERANGE; }	/* immediately detect overflow */

      xC = ESL_MAX(xC,        xE  - om->tec);
      xB = ESL_MAX(om->base,  xC) - om->tjb;
	  
#if p7_DEBUGGING
      if (ox->debugging) p7_omx_DumpMSPRow(ox, i, xE, 0, xC, xB, xC);   
#endif
    } /* end loop over sequence residues 1..L */

  /* finally C->T, and add our missing precision on the NN,CC,JJ back */
  *ret_sc = ((float) (xC - om->tjb) - (float) om->base);
  *ret_sc /= om->scale;
  *ret_sc -= 3.0; /* that's ~ L \log \frac{L}{L+3}, for our NN,CC,JJ */

  return eslOK;
}
/*------------------ end, p7_MSPFilter() ------------------------*/




/*****************************************************************
 * 2. Benchmark driver.
 *****************************************************************/
/* The benchmark driver has some additional non-benchmarking options
 * to facilitate small-scale (by-eye) comparison of MSP scores against
 * other implementations, for debugging purposes.
 * 
 * The -c option compares against p7_GMSP() scores. This allows
 * measuring the error inherent in the SSE implementation's reduced
 * precision (p7_MSPFilter() runs in uint8_t; p7_GMSP() uses floats).
 * 
 * The -x option compares against an emulation that should give
 * exactly the same scores. The emulation is achieved by jiggering the
 * fp scores in a generic profile to disallow gaps, have the same
 * rounding and precision as the uint8_t's MSPFilter() is using, and
 * to make the same post-hoc corrections for the NN, CC, JJ
 * contributions to the final nat score; under these contrived
 * circumstances, p7_GViterbi() gives the same scores as
 * p7_MSPFilter().
 * 
 * For using either -c or -x, you probably also want to limit the
 * number of generated target sequences, using -N10 or -N100 for
 * example.
 */
#ifdef p7MSPFILTER_BENCHMARK
/* 
   gcc -o benchmark-mspfilter -std=gnu99 -g -Wall -msse2 -I.. -L.. -I../../easel -L../../easel -Dp7MSPFILTER_BENCHMARK mspfilter.c -lhmmer -leasel -lm 
   icc -o benchmark-mspfilter -O3 -static -I.. -L.. -I../../easel -L../../easel -Dp7MSPFILTER_BENCHMARK mspfilter.c -lhmmer -leasel -lm 

   ./benchmark-mspfilter <hmmfile>            runs benchmark 
   ./benchmark-mspfilter -b <hmmfile>         gets baseline time to subtract: just random seq generation
   ./benchmark-mspfilter -N100 -c <hmmfile>   compare scores of SSE to generic impl    - differences reflect lack of MSP precision 
   ./benchmark-mspfilter -N100 -x <hmmfile>   compare scores of SSE to exact emulation - should exactly match
 */
#include "p7_config.h"

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_getopts.h"
#include "esl_random.h"
#include "esl_randomseq.h"
#include "esl_stopwatch.h"

#include "hmmer.h"
#include "impl_sse.h"

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range toggles reqs incomp  help                                       docgroup*/
  { "-h",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "show brief help on version and usage",             0 },
  { "-b",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "baseline timing: don't run DP at all",             0 },
  { "-c",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, "-x", "compare scores of generic vs. SSE version (debug)",0 }, 
  { "-r",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "set random number seed randomly",                  0 },
  { "-s",        eslARG_INT,     "42", NULL, NULL,  NULL,  NULL, NULL, "set random number seed to <n>",                    0 },
  { "-x",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, "-c", "test against exact emulation (debug)",             0 },
  { "-L",        eslARG_INT,    "400", NULL, "n>0", NULL,  NULL, NULL, "length of random target seqs",                     0 },
  { "-N",        eslARG_INT,  "50000", NULL, "n>0", NULL,  NULL, NULL, "number of random target seqs",                     0 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options] <hmmfile>";
static char banner[] = "benchmark driver for MSPFilter() implementation";

int 
main(int argc, char **argv)
{
  ESL_GETOPTS    *go      = esl_getopts_CreateDefaultApp(options, 1, argc, argv, banner, usage);
  char           *hmmfile = esl_opt_GetArg(go, 1);
  ESL_STOPWATCH  *w       = esl_stopwatch_Create();
  ESL_RANDOMNESS *r       = NULL;
  ESL_ALPHABET   *abc     = NULL;
  P7_HMMFILE     *hfp     = NULL;
  P7_HMM         *hmm     = NULL;
  P7_BG          *bg      = NULL;
  P7_PROFILE     *gm      = NULL;
  P7_OPROFILE    *om      = NULL;
  P7_OMX         *ox      = NULL;
  P7_GMX         *gx      = NULL;
  int             L       = esl_opt_GetInteger(go, "-L");
  int             N       = esl_opt_GetInteger(go, "-N");
  ESL_DSQ        *dsq     = malloc(sizeof(ESL_DSQ) * (L+2));
  int             i;
  float           sc1, sc2;

  if (esl_opt_GetBoolean(go, "-r"))  r = esl_randomness_CreateTimeseeded();
  else                               r = esl_randomness_Create(esl_opt_GetInteger(go, "-s"));

  if (p7_hmmfile_Open(hmmfile, NULL, &hfp) != eslOK) p7_Fail("Failed to open HMM file %s", hmmfile);
  if (p7_hmmfile_Read(hfp, &abc, &hmm)     != eslOK) p7_Fail("Failed to read HMM");

  bg = p7_bg_Create(abc);
  p7_bg_SetLength(bg, L);
  gm = p7_profile_Create(hmm->M, abc);
  p7_ProfileConfig(hmm, bg, gm, L, p7_LOCAL);
  om = p7_oprofile_Create(gm->M, abc);
  p7_oprofile_Convert(gm, om);
  p7_oprofile_ReconfigLength(om, L);
  if (esl_opt_GetBoolean(go, "-x")) p7_oprofile_SameMSP(om, gm);
  ox = p7_omx_Create(gm->M, 0, 0);
  gx = p7_gmx_Create(gm->M, L);

  esl_stopwatch_Start(w);
  for (i = 0; i < N; i++)
    {
      esl_rsq_xfIID(r, bg->f, abc->K, L, dsq);

      if (! esl_opt_GetBoolean(go, "-b")) 
	{
	  p7_MSPFilter    (dsq, L, om, ox, &sc1);   

	  /* -c option: compare generic to fast score */
	  if (esl_opt_GetBoolean(go, "-c")) 
	    {
	      p7_GMSP    (dsq, L, gm, gx, &sc2); 
	      printf("%.4f %.4f\n", sc1, sc2);  
	    }

	/* -x option: compare generic to fast score in a way that should give exactly the same result */
	  if (esl_opt_GetBoolean(go, "-x"))
	    {
	      p7_GViterbi(dsq, L, gm, gx, &sc2); 
	      sc2 /= om->scale;
	      if (om->mode == p7_UNILOCAL)   sc2 -= 2.0; /* that's ~ L \log \frac{L}{L+2}, for our NN,CC,JJ */
	      else if (om->mode == p7_LOCAL) sc2 -= 3.0; /* that's ~ L \log \frac{L}{L+3}, for our NN,CC,JJ */
	      printf("%.4f %.4f\n", sc1, sc2);  
	    }
	}
    }
  esl_stopwatch_Stop(w);
  esl_stopwatch_Display(stdout, w, "# CPU time: ");
  printf("# M    = %d\n",   gm->M);

  free(dsq);
  p7_omx_Destroy(ox);
  p7_gmx_Destroy(gx);
  p7_oprofile_Destroy(om);
  p7_profile_Destroy(gm);
  p7_bg_Destroy(bg);
  p7_hmm_Destroy(hmm);
  p7_hmmfile_Close(hfp);
  esl_alphabet_Destroy(abc);
  esl_stopwatch_Destroy(w);
  esl_randomness_Destroy(r);
  esl_getopts_Destroy(go);
  return 0;
}
#endif /*p7MSPFILTER_BENCHMARK*/
/*------------------ end, benchmark driver ----------------------*/




/*****************************************************************
 * 3. Unit tests
 *****************************************************************/
#ifdef p7MSPFILTER_TESTDRIVE
#include "esl_random.h"
#include "esl_randomseq.h"

/* 
 * We can check that scores are identical (within machine error) to
 * scores of generic DP with scores rounded the same way.  Do this for
 * a random model of length <M>, for <N> test sequences of length <L>.
 * 
 * We assume that we don't accidentally generate a high-scoring random
 * sequence that overflows MSPFilter()'s limited range.
 * 
 */
static void
utest_msp_filter(ESL_RANDOMNESS *r, ESL_ALPHABET *abc, P7_BG *bg, int M, int L, int N)
{
  P7_HMM      *hmm = NULL;
  P7_PROFILE  *gm  = NULL;
  P7_OPROFILE *om  = NULL;
  ESL_DSQ     *dsq = malloc(sizeof(ESL_DSQ) * (L+2));
  P7_OMX      *ox  = p7_omx_Create(M, 0, 0);
  P7_GMX      *gx  = p7_gmx_Create(M, L);
  float sc1, sc2;

  p7_oprofile_Sample(r, abc, bg, M, L, &hmm, &gm, &om);
  p7_oprofile_SameMSP(om, gm);
#if 0
  p7_oprofile_Dump(stdout, om);              //dumps the optimized profile
  p7_omx_SetDumpMode(stdout, ox, TRUE);      //makes the fast DP algorithms dump their matrices
#endif

  while (N--)
    {
      esl_rsq_xfIID(r, bg->f, abc->K, L, dsq);
      p7_MSPFilter(dsq, L, om, ox, &sc1);
      p7_GViterbi (dsq, L, gm, gx, &sc2);
#if 0
      p7_gmx_Dump(stdout, gx);           //dumps a generic DP matrix
#endif

      sc2 = sc2 / om->scale - 3.0f;
      if (fabs(sc1-sc2) > 0.001) esl_fatal("msp filter unit test failed: scores differ (%.2f, %.2f)", sc1, sc2);
    }

  free(dsq);
  p7_hmm_Destroy(hmm);
  p7_omx_Destroy(ox);
  p7_gmx_Destroy(gx);
  p7_profile_Destroy(gm);
  p7_oprofile_Destroy(om);
}
#endif /*p7MSPFILTER_TESTDRIVE*/
/*-------------------- end, unit tests --------------------------*/




/*****************************************************************
 * 4. Test driver
 *****************************************************************/
#ifdef p7MSPFILTER_TESTDRIVE
/* 
   gcc -g -Wall -msse2 -std=gnu99 -I.. -L.. -I../../easel -L../../easel -o mspfilter_utest -Dp7MSPFILTER_TESTDRIVE mspfilter.c -lhmmer -leasel -lm
   ./mspfilter_utest
 */
#include "p7_config.h"

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_getopts.h"

#include "hmmer.h"
#include "impl_sse.h"

static ESL_OPTIONS options[] = {
  /* name           type      default  env  range toggles reqs incomp  help                                       docgroup*/
  { "-h",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "show brief help on version and usage",           0 },
  { "-r",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "set random number seed randomly",                0 },
  { "-s",        eslARG_INT,     "42", NULL, NULL,  NULL,  NULL, NULL, "set random number seed to <n>",                  0 },
  { "-v",        eslARG_NONE,   FALSE, NULL, NULL,  NULL,  NULL, NULL, "be verbose",                                     0 },
  { "-L",        eslARG_INT,    "200", NULL, NULL,  NULL,  NULL, NULL, "size of random sequences to sample",             0 },
  { "-M",        eslARG_INT,    "145", NULL, NULL,  NULL,  NULL, NULL, "size of random models to sample",                0 },
  { "-N",        eslARG_INT,    "100", NULL, NULL,  NULL,  NULL, NULL, "number of random sequences to sample",           0 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};
static char usage[]  = "[-options]";
static char banner[] = "test driver for the SSE MSPFilter() implementation";

int
main(int argc, char **argv)
{
  ESL_GETOPTS    *go   = esl_getopts_CreateDefaultApp(options, 0, argc, argv, banner, usage);
  ESL_RANDOMNESS *r    = NULL;
  ESL_ALPHABET   *abc  = NULL;
  P7_BG          *bg   = NULL;
  int             M    = esl_opt_GetInteger(go, "-M");
  int             L    = esl_opt_GetInteger(go, "-L");
  int             N    = esl_opt_GetInteger(go, "-N");

  if (esl_opt_GetBoolean(go, "-r"))  r = esl_randomness_CreateTimeseeded();
  else                               r = esl_randomness_Create(esl_opt_GetInteger(go, "-s"));

  /* First round of tests for DNA alphabets.  */
  if ((abc = esl_alphabet_Create(eslDNA)) == NULL)  esl_fatal("failed to create alphabet");
  if ((bg = p7_bg_Create(abc))            == NULL)  esl_fatal("failed to create null model");

  if (esl_opt_GetBoolean(go, "-v")) printf("MSPFilter() tests, DNA\n");
  utest_msp_filter(r, abc, bg, M, L, N);   /* normal sized models */
  utest_msp_filter(r, abc, bg, 1, L, 10);  /* size 1 models       */
  utest_msp_filter(r, abc, bg, M, 1, 10);  /* size 1 sequences    */

  esl_alphabet_Destroy(abc);
  p7_bg_Destroy(bg);

  if ((abc = esl_alphabet_Create(eslAMINO)) == NULL)  esl_fatal("failed to create alphabet");
  if ((bg = p7_bg_Create(abc))              == NULL)  esl_fatal("failed to create null model");

  if (esl_opt_GetBoolean(go, "-v")) printf("MSPFilter() tests, protein\n");
  utest_msp_filter(r, abc, bg, M, L, N);   
  utest_msp_filter(r, abc, bg, 1, L, 10);  
  utest_msp_filter(r, abc, bg, M, 1, 10);  

  esl_alphabet_Destroy(abc);
  p7_bg_Destroy(bg);

  esl_getopts_Destroy(go);
  esl_randomness_Destroy(r);
  return eslOK;
}
#endif /*VITFILTER_TESTDRIVE*/



/*****************************************************************
 * 5. Example
 *****************************************************************/

#ifdef p7MSPFILTER_EXAMPLE
/* A minimal example.
   Also useful for debugging on small HMMs and sequences.

   gcc -g -Wall -msse2 -std=gnu99 -I.. -L.. -I../../easel -L../../easel -o example -Dp7MSPFILTER_EXAMPLE mspfilter.c -lhmmer -leasel -lm
   ./example <hmmfile> <seqfile>
 */ 
#include "p7_config.h"

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_sq.h"
#include "esl_sqio.h"

#include "hmmer.h"
#include "impl_sse.h"

int 
main(int argc, char **argv)
{
  char           *hmmfile = argv[1];
  char           *seqfile = argv[2];
  ESL_ALPHABET   *abc     = NULL;
  P7_HMMFILE     *hfp     = NULL;
  P7_HMM         *hmm     = NULL;
  P7_BG          *bg      = NULL;
  P7_PROFILE     *gm      = NULL;
  P7_OPROFILE    *om      = NULL;
  P7_OMX         *ox      = NULL;
  P7_GMX         *gx      = NULL;
  ESL_SQ         *sq      = NULL;
  ESL_SQFILE     *sqfp    = NULL;
  int             format  = eslSQFILE_UNKNOWN;
  float           sc;
  int             status;

  /* Read in one HMM */
  if (p7_hmmfile_Open(hmmfile, NULL, &hfp) != eslOK) p7_Fail("Failed to open HMM file %s", hmmfile);
  if (p7_hmmfile_Read(hfp, &abc, &hmm)     != eslOK) p7_Fail("Failed to read HMM");

  /* Read in one sequence */
  sq     = esl_sq_CreateDigital(abc);
  status = esl_sqfile_Open(seqfile, format, NULL, &sqfp);
  if      (status == eslENOTFOUND) p7_Fail("No such file.");
  else if (status == eslEFORMAT)   p7_Fail("Format unrecognized.");
  else if (status == eslEINVAL)    p7_Fail("Can't autodetect stdin or .gz.");
  else if (status != eslOK)        p7_Fail("Open failed, code %d.", status);
  if  (esl_sqio_Read(sqfp, sq) != eslOK) p7_Fail("Failed to read sequence");

  /* create default null model, then create and optimize profile */
  bg = p7_bg_Create(abc);
  p7_bg_SetLength(bg, sq->n);
  gm = p7_profile_Create(hmm->M, abc);
  p7_ProfileConfig(hmm, bg, gm, sq->n, p7_LOCAL);
  om = p7_oprofile_Create(gm->M, abc);
  p7_oprofile_Convert(gm, om);

  /* allocate DP matrices, both a generic and an optimized one */
  ox = p7_omx_Create(gm->M, 0, 0); /* one row version */
  gx = p7_gmx_Create(gm->M, sq->n);

  /* Useful to place and compile in for debugging: 
     p7_oprofile_Dump(stdout, om);      dumps the optimized profile
     p7_omx_SetDumpMode(ox, TRUE);      makes the fast DP algorithms dump their matrices
     p7_gmx_Dump(stdout, gx);           dumps a generic DP matrix
     p7_oprofile_SameMSP(om, gm);
  */

  p7_MSPFilter      (sq->dsq, sq->n, om, ox, &sc);  
  printf("msp filter score:     %.2f nats\n", sc);

  /* now in a real app, you'd need to convert raw nat scores to final bit
   * scores, by subtracting the null model score and rescaling.
   */

  /* cleanup */
  esl_sq_Destroy(sq);
  esl_sqfile_Close(sqfp);
  p7_omx_Destroy(ox);
  p7_gmx_Destroy(gx);
  p7_oprofile_Destroy(om);
  p7_profile_Destroy(gm);
  p7_bg_Destroy(bg);
  p7_hmm_Destroy(hmm);
  p7_hmmfile_Close(hfp);
  esl_alphabet_Destroy(abc);
  return 0;
}
#endif /*p7MSPFILTER_EXAMPLE*/
/*---------------------- end, example ---------------------------*/




/*****************************************************************
 * @LICENSE@
 *****************************************************************/
