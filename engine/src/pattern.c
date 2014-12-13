#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <glib.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include "spew.h"
#include "data.h"
#include "cluster.h"
#include "pattern.h"


static sem_t synSem;
static CONFIG *p_Conf;
static MELT_POT *p_Pot;


/*======================================================================*
 *                 Declaration for Internal Functions                   *
 *======================================================================*/
/**
 * This function extracts a set of commonly shared byte sequences for a group.
 *
 * @param vp_Param      The THREAD_CRAFT type parameter.
 *
 * @return (currently deprecated)
 */
void*
_PtnMapCraft(void *vp_Param);


/**
 * This function extracts the commonly shared byte sequences for the samples
 * belonged to the same partition slot.
 * 
 * @param p_Param       The pointer to the THREAD_SLOT parameter.
 * 
 * @return status code
 */
int8_t
_PtnMapSlot(THREAD_SLOT *p_Param);


/**
 * This function iteratively merges the results extracted from each group.
 *
 * @param p_Param       The pointer to the updated THREAD_CRAFT parameter.
 * 
 * @return status code
 */
int8_t
_PtnReduceCraft(THREAD_CRAFT *p_Param);


/**
 * This function iteratively merges the results extracted from each slot.
 * 
 * @param a_Param       The array of the updated THREAD_SLOT parameter.
 * @param ulSize        The array size
 * 
 * @return status code
 */
int8_t
_PtnReduceSlot(THREAD_SLOT *a_Param, uint64_t ulSize);


/**
 * This function initializes the THREAD_CRAFT structure.
 * 
 * @param p_Param       The pointer to the to be initialized structure.
 * @param p_Grp         The pointer to the GROUP structure.
 * 
 * @return status code (currently unused)
 */
int8_t
_PtnSetParamThrdCrt(THREAD_CRAFT *p_Param, GROUP *p_Grp);


/**
 * This function initializes the THREAD_SLOT structure.
 * 
 * @param p_Param       The pointer to the to be initialized structure.
 * @param p_Grp         The pointer to the GROUP structure.
 * @param ulIdxBgn      The beginning index to the assigned range.
 * @param ulIdxEnd      The ending index to the assigned range.
 * 
 * @return status code
 */
int8_t
_PtnSetParamThrdSlt(THREAD_SLOT *p_Param, GROUP *p_Grp, uint64_t ulIdxBgn, uint64_t ulIdxEnd);


/**
 * This function initializes the array of THREAD_CRAFT structures.
 * 
 * @param p_aParam      The pointer to the array of THREAD_CRAFT structures.
 * @param ulSize        The array size.
 * 
 * @return status code
 */
int8_t
_PtnInitArrayThrdCrt(THREAD_CRAFT **p_aParam, uint64_t ulSize);


/**
 * This function initializes the array of THREAD_SLOT structures.
 * 
 * @param p_aParam      The pointer to the array of THREAD_SLOT structures.
 * @param ulSize        The array size.
 * 
 * @return status code
 */
int8_t
_PtnInitArrayThrdSlt(THREAD_SLOT **p_aParam, uint64_t ulSize);


/**
 * This function deinitializes the array of THREAD_CRAFT structures.
 * 
 * @param a_Param       The array of THREAD_CRAFT structures.
 * @param ulSize        The array size.
 * 
 * @return (currently unused)
 */
int8_t
_PtnDeinitArrayThrdCrt(THREAD_CRAFT *a_Param, uint64_t ulSize);


/**
 * This function deinitializes the array of THREAD_SLOT structures.
 * 
 * @param a_Param       The array of THREAD_SLOT structures.
 * @param ulSize        The array size.
 * 
 * @return (currently unused)
 */
int8_t
_PtnDeinitArrayThrdSlt(THREAD_SLOT *a_Param, uint64_t ulSize);


/*======================================================================*
 *                Implementation for Exported Functions                 *
 *======================================================================*/
int8_t
PtnSetContext(CONTEXT *p_Ctx)
{
    p_Conf = p_Ctx->p_Conf;
    p_Pot = p_Ctx->p_Pot;
    
    return CLS_SUCCESS;
}


int8_t
PtnCraftPattern()
{
    int8_t cRtnCode = CLS_SUCCESS;

    /*-----------------------------------------------------------------------*
     * Spawn multipe threads each of which:                                  *
     * 1. Extract a set of commonly shared byte blocks with user specified   *
     *    quality for the given group.                                       *
     *-----------------------------------------------------------------------*/
    THREAD_CRAFT *a_Param;
    uint64_t ulCntGrp = g_hash_table_size(p_Pot->h_Grp);
    int8_t cStat = _PtnInitArrayThrdCrt(&a_Param, ulCntGrp);
    if (cStat != CLS_SUCCESS)
        EXITQ(cStat, EXIT);

    sem_init(&synSem, 0, p_Conf->ucCntThrd);
    GHashTableIter iterHash;
    gpointer gpKey, gpValue;
    uint64_t ulIdx = 0;
    g_hash_table_iter_init(&iterHash, p_Pot->h_Grp);
    while (g_hash_table_iter_next(&iterHash, &gpKey, &gpValue)) {
        sem_wait(&synSem);
        GROUP *p_Grp = (GROUP*)gpValue;
        _PtnSetParamThrdCrt(&(a_Param[ulIdx]), p_Grp);
        pthread_create(&(a_Param[ulIdx].tId), NULL, _PtnMapCraft, (void*)&(a_Param[ulIdx]));    
        ulIdx++;
    }

    /*-----------------------------------------------------------------------*
     * Join the spawned threads:                                             *
     * 1. Merge the byte block sets collected from each group.               * 
     *-----------------------------------------------------------------------*/
    uint64_t ulCntFork = ulIdx;
    for (ulIdx = 0 ; ulIdx < ulCntFork ; ulIdx++) {
        pthread_join(a_Param[ulIdx].tId, NULL);
        _PtnReduceCraft(&(a_Param[ulIdx]));
        if (a_Param[ulIdx].cRtnCode != CLS_SUCCESS)
            cRtnCode = CLS_FAIL_PROCESS;
    }
    sem_destroy(&synSem);

FREEPARAM:    
    _PtnDeinitArrayThrdCrt(a_Param, ulCntGrp);  
EXIT:
    return cRtnCode;
}


int8_t
PtnOutputYara()
{
    int8_t cRtnCode = CLS_SUCCESS;
    
    return cRtnCode;
}


/*======================================================================*
 *                Implementation for Internal Functions                 *
 *======================================================================*/
void*
_PtnMapCraft(void *vp_Param)
{
    int8_t cRtnCode = CLS_SUCCESS;
    THREAD_CRAFT *p_Param = (THREAD_CRAFT*)vp_Param;
    GROUP *p_Grp = p_Param->p_Grp;
    GArray *a_Mbr = p_Grp->a_Mbr;

    /*-----------------------------------------------------------------------*
     * To avoid opening too many files simultaneously, we split the grouped  *
     * slices into sevaral slots. And we first extract the commonly shared   *
     * byte blocks from each slot and then merge them together.              *
     *-----------------------------------------------------------------------*/
    uint64_t ulCntMbr = a_Mbr->len;
    uint64_t ulCntSlot = (uint64_t)ceil((double)ulCntMbr / p_Conf->ucIoBand);
    THREAD_SLOT *a_Param;
    int8_t cStat = _PtnInitArrayThrdSlt(&a_Param, ulCntSlot);    
    if (cStat != CLS_SUCCESS)
        EXITQ(cStat, EXIT);

    uint64_t ulIdxSlot, ulIdxBgn, ulIdxEnd = 0;
    for (ulIdxSlot = 0 ; ulIdxSlot < ulCntSlot ; ulIdxSlot++) {
        ulIdxBgn = ulIdxEnd;
        ulIdxEnd = MIN(ulIdxBgn + p_Conf->ucIoBand, ulCntMbr);
        cStat = _PtnSetParamThrdSlt(&(a_Param[ulIdxSlot]), p_Grp, ulIdxBgn, ulIdxEnd);
        if (cStat != CLS_SUCCESS)
            break;
        cStat = _PtnMapSlot(&(a_Param[ulIdxSlot]));
        if (cStat != CLS_SUCCESS)
            break;
    }

    _PtnReduceSlot(a_Param, ulCntSlot);

FREEPARAM:
    _PtnDeinitArrayThrdSlt(a_Param, ulCntSlot);
EXIT:
    p_Param->cRtnCode = cRtnCode;
    sem_post(&synSem);
    return;
}


int8_t
_PtnMapSlot(THREAD_SLOT *p_Param)
{
    int8_t cRtnCode = CLS_SUCCESS;

    uint8_t ucSizeBlk = p_Conf->ucSizeBlk;
    char **a_szBin = p_Param->a_szBin;
    GArray *a_Mbr = p_Param->a_Mbr;
    uint64_t ulIdxBgn = p_Param->ulIdxBgn;
    uint64_t ulIdxEnd = p_Param->ulIdxEnd;
    uint16_t usFront = 0;
    uint16_t usRear = ucSizeBlk;
    uint64_t ulRange = ulIdxEnd - ulIdxBgn;

    while (usRear <= p_Param->usSizeMinSlc) {
        BLOCK_CAND *p_BlkCand;
        uint8_t cStat = DsNewBlockCand(&p_BlkCand, ucSizeBlk);
        if (cStat != CLS_SUCCESS)
            EXITQ(cStat, EXIT);

        /* Let the first slice be the comparison base. */
        uint16_t *a_usCtn = p_BlkCand->a_usCtn;
        GArray *a_CtnAddr = p_BlkCand->a_CtnAddr;
        uint16_t usSrc, usTge;
        for (usSrc = usFront, usTge = 0 ; usSrc < usRear ; usSrc++, usTge++)
            a_usCtn[usTge] = a_szBin[0][usSrc];

        uint64_t ulIdSlc = g_array_index(a_Mbr, uint64_t, ulIdxBgn);
        SLICE *p_Slc = g_ptr_array_index(p_Pot->a_Slc, ulIdSlc);
        CONTENT_ADDR addr;
        addr.iIdSec = p_Slc->iIdSec;
        addr.ulOfstRel = p_Slc->ulOfstRel + usFront;
        g_array_append_val(a_CtnAddr, addr);

        /* Iteratively compare the rest slices with the base. */
        uint64_t ulOfst, ulIdx;
        for (ulOfst = 1, ulIdx = ulIdxBgn + 1; ulOfst < ulRange ; ulOfst++, ulIdx++) {
            for (usSrc = usFront, usTge = 0 ; usSrc < usRear ; usSrc++, usTge++) {
                if (a_usCtn[usTge] != a_szBin[ulOfst][usSrc])
                    a_usCtn[usTge] = WILD_CARD_MARK;
            }

            ulIdSlc = g_array_index(a_Mbr, uint64_t, ulIdx);
            p_Slc = g_ptr_array_index(p_Pot->a_Slc, ulIdSlc);
            addr.iIdSec = p_Slc->iIdSec;
            addr.ulOfstRel = p_Slc->ulOfstRel + usFront;
            g_array_append_val(a_CtnAddr, addr);
        }

        g_ptr_array_add(p_Param->a_BlkCand, p_BlkCand);
        usFront += ROLL_SHFT_COUNT;
        usRear += ROLL_SHFT_COUNT;
    }

EXIT:
    return cRtnCode;
}


int8_t
_PtnReduceCraft(THREAD_CRAFT *p_Param)
{
    return CLS_SUCCESS;
}


int8_t
_PtnReduceSlot(THREAD_SLOT *a_Param, uint64_t ulSize)
{
    int8_t cRtnCode = CLS_SUCCESS;

    uint16_t usCntMin = USHRT_MAX;
    uint64_t ulIdx;
    for (ulIdx = 0 ; ulIdx < ulSize ; ulIdx++) {
        if (a_Param[ulIdx].a_BlkCand->len < usCntMin)
            usCntMin = a_Param[ulIdx].a_BlkCand->len;
    }

    /* Let the common block list extracted from the first slot be the comparison base. */
    THREAD_SLOT *p_Param = a_Param;
    GPtrArray *a_BlkCandB = p_Param->a_BlkCand;

    /* Iteratively compare the rest block lists with the base. */
    uint16_t usIdx;
    for (usIdx = 0 ; usIdx < usCntMin ; usIdx++) {
        BLOCK_CAND *p_BlkCandB = g_ptr_array_index(a_BlkCandB, usIdx);
        uint16_t *a_usCtnB = p_BlkCandB->a_usCtn;
        GArray *a_CtnAddrB = p_BlkCandB->a_CtnAddr;
        for (ulIdx = 1 ; ulIdx < ulSize ; ulIdx++) {
            p_Param = a_Param + ulIdx;
            GPtrArray *a_BlkCandC = p_Param->a_BlkCand;
            BLOCK_CAND *p_BlkCandC = g_ptr_array_index(a_BlkCandC, usIdx);
            uint16_t *a_usCtnC = p_BlkCandC->a_usCtn;
            GArray *a_CtnAddrC = p_BlkCandC->a_CtnAddr;

            uint8_t ucIdx;
            for (ucIdx = 0 ; ucIdx < p_Conf->ucSizeBlk ; ucIdx++) {
                if (a_usCtnB[ucIdx] != a_usCtnC[ucIdx])
                    a_usCtnB[ucIdx] = WILD_CARD_MARK;
            }

            uint32_t uiLen = a_CtnAddrC->len;
            uint32_t uiIdx;
            for (uiIdx = 0 ; uiIdx < uiLen ; uiIdx++) {
                CONTENT_ADDR addr = g_array_index(a_CtnAddrC, CONTENT_ADDR, uiIdx);
                g_array_append_val(a_CtnAddrB, addr);
            }
        }
    }

EXIT:
    return cRtnCode;
}


int8_t
_PtnSetParamThrdCrt(THREAD_CRAFT *p_Param, GROUP *p_Grp)
{
    p_Param->p_Grp = p_Grp;
    return CLS_SUCCESS;
}


int8_t
_PtnSetParamThrdSlt(THREAD_SLOT *p_Param, GROUP *p_Grp, uint64_t ulIdxBgn, uint64_t ulIdxEnd)
{
    int8_t cRtnCode = CLS_SUCCESS;

    p_Param->ulIdxBgn = ulIdxBgn;
    p_Param->ulIdxEnd = ulIdxEnd;
    p_Param->a_Mbr = p_Grp->a_Mbr;
    
    /* The first slot should be assigned with the block list of this group. */
    if (ulIdxBgn == 0)
        p_Param->a_BlkCand = p_Grp->a_BlkCand;
    else {
        p_Param->a_BlkCand = g_ptr_array_new_with_free_func(DsDeleteBlkCand);
        if (!p_Param->a_BlkCand)
            EXIT1(CLS_FAIL_MEM_ALLOC, EXIT, "Error: %s.", strerror(errno));
    }

    uint64_t ulRange = ulIdxEnd - ulIdxBgn;
    p_Param->a_szBin = (char**)malloc(sizeof(char*) * ulRange);
    if (!p_Param->a_szBin)
        EXIT1(CLS_FAIL_MEM_ALLOC, EXIT, "Error: %s.", strerror(errno));

    uint64_t ulOfst;
    for (ulOfst = 0 ; ulOfst < ulRange ; ulOfst++)
        p_Param->a_szBin[ulOfst] = NULL;

    uint64_t ulIdx;
    for (ulIdx = ulIdxBgn, ulOfst = 0 ; ulIdx < ulIdxEnd ; ulIdx++, ulOfst++) {
        uint64_t ulIdSlc = g_array_index(p_Grp->a_Mbr, uint64_t, ulIdx);
        SLICE *p_Slc = g_ptr_array_index(p_Pot->a_Slc, ulIdSlc);

        p_Param->a_szBin[ulOfst] = (char*)malloc(sizeof(char) * p_Slc->usSize);
        if (!(p_Param->a_szBin[ulOfst]))
            EXIT1(CLS_FAIL_MEM_ALLOC, EXIT, "Error: %s.", strerror(errno));

        FILE *fp = fopen(p_Slc->szPathFile, "rb");
        if (!fp)
            EXIT1(CLS_FAIL_FILE_IO, EXIT, "Error: %s.", strerror(errno));

        int8_t cStat = fseek(fp, p_Slc->ulOfstAbs, SEEK_SET);
        if (cStat != 0) {
            EXIT1(CLS_FAIL_FILE_IO, CLOSE, "Error: %s.", strerror(errno));
        }

        size_t nReadExpt = p_Slc->usSize;
        size_t nReadReal = fread(p_Param->a_szBin[ulOfst], sizeof(char), nReadExpt, fp);
        if (nReadExpt != nReadReal)
            EXIT1(CLS_FAIL_FILE_IO, CLOSE, "Error: %s.", strerror(errno));
        
        if (p_Slc->usSize < p_Param->usSizeMinSlc)
            p_Param->usSizeMinSlc = p_Slc->usSize;

    CLOSE:
        if (fp)
            fclose(fp);
        if (cRtnCode != CLS_SUCCESS)    
            break;
    }

EXIT:
    return cRtnCode;
}


int8_t
_PtnInitArrayThrdCrt(THREAD_CRAFT **p_aParam, uint64_t ulSize)
{
    int8_t cRtnCode = CLS_SUCCESS;

    *p_aParam = (THREAD_CRAFT*)malloc(sizeof(THREAD_CRAFT) * ulSize);
    if (!(*p_aParam))
        EXIT1(CLS_FAIL_MEM_ALLOC, EXIT, "Error: %s.", strerror(errno));

    THREAD_CRAFT *a_Param = *p_aParam;
    uint32_t ulIdx;
    for (ulIdx = 0 ; ulIdx < ulSize ; ulIdx++) {
        a_Param[ulIdx].p_Grp = NULL;
    }

EXIT:
    return cRtnCode;
}


int8_t
_PtnInitArrayThrdSlt(THREAD_SLOT **p_aParam, uint64_t ulSize)
{
    int8_t cRtnCode = CLS_SUCCESS;

    *p_aParam = (THREAD_SLOT*)malloc(sizeof(THREAD_SLOT) * ulSize);
    if (!(*p_aParam))
        EXIT1(CLS_FAIL_MEM_ALLOC, EXIT, "Error: %s.", strerror(errno));

    THREAD_SLOT *a_Param = *p_aParam;
    uint64_t ulIdx;
    for (ulIdx = 0 ; ulIdx < ulSize ; ulIdx++) {
        a_Param[ulIdx].usSizeMinSlc = USHRT_MAX;
        a_Param[ulIdx].a_szBin = NULL;
        a_Param[ulIdx].a_Mbr = NULL;
        a_Param[ulIdx].a_BlkCand = NULL;
    }

EXIT:
    return cRtnCode;
}


int8_t
_PtnDeinitArrayThrdCrt(THREAD_CRAFT *a_Param, uint64_t ulSize)
{
    if (a_Param)
        free(a_Param);
    
    return CLS_SUCCESS;
}


int8_t
_PtnDeinitArrayThrdSlt(THREAD_SLOT *a_Param, uint64_t ulSize)
{
    if (!a_Param)
        return CLS_SUCCESS;

    uint64_t ulIdx;
    for (ulIdx = 0 ; ulIdx < ulSize ; ulIdx++) {
        THREAD_SLOT *p_Param = a_Param + ulIdx;
        if (p_Param->a_BlkCand) {
            /* We now do not free the first array which represents as the
               common features shared by this group. */
            if (ulIdx > 0)
                g_ptr_array_free(p_Param->a_BlkCand, true);
        }
        if (p_Param->a_szBin) {
            uint64_t ulRange = p_Param->ulIdxEnd - p_Param->ulIdxBgn;
            uint64_t ulOfst;
            for (ulOfst = 0 ; ulOfst < ulRange ; ulOfst++) {
                if (p_Param->a_szBin[ulOfst])
                    free(p_Param->a_szBin[ulOfst]);
            }
            free(p_Param->a_szBin);
        }
    }

    free(a_Param);
    return CLS_SUCCESS;
}
