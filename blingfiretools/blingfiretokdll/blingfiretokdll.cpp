#include "FAConfig.h"
#include "FALimits.h"
#include "FAUtf8Utils.h"
#include "FAImageDump.h"
#include "FAWbdConfKeeper.h"
#include "FALDB.h"
#include "FALexTools_t.h"
#include "FADictConfKeeper.h"
#include "FATokenSegmentationTools_1best_t.h"
#include "FATokenSegmentationTools_1best_bpe_t.h"
#include "FATokenSegmentationTools_1best_bpe_with_merges_t.h"
#include "FAHyphConfKeeper_packaged.h"
#include "FAHyphInterpreter_core_t.h"
#include "FAStringArray_pack.h"
#include "FAWREConf_pack.h"
#include "FADigitizer_t.h"
#include "FADigitizer_dct_t.h"
#include "FAAutInterpretTools_t.h"
#include "FAAutInterpretTools2_trbr_t.h"
#include "FAAllocator.h"
#include "FAFsmConst.h"
#include "FABrResultA.h"
#include "FATaggedTextA.h"
#include "FAMorphLDB_t_packaged.h"
#include "FAParserConfKeeper.h"
#include "FAWreLexTools_t.h"

#include "blingfiretokdll.h"

/*
This library provides easy interface to sentence and word-breaking functionality
which can be used in C#, Python, Perl, etc.

Also the goal is to reduce dependecies and provide the same library available
on Windows, Linux, Mac OSX, etc.
*/


// defines g_dumpBlingFireTokLibWbdData, the sources of this data is SearchGold\deploy\builds\data\IndexGenData\ldbsrc\ldb\tp3\wbd
#include "BlingFireTokLibWbdData.cxx"

// defines g_dumpBlingFireTokLibSbdData, the sources of this data is SearchGold\deploy\builds\data\IndexGenData\ldbsrc\ldb\tp3\sbd
#include "BlingFireTokLibSbdData.cxx"
#include <iostream>

using namespace BlingFire;

// version of this binary and the algo logic
#define BLINGFIRETOK_MAJOR_VERSION_NUM 18
#define BLINGFIRETOK_MINOR_VERSION_NUM 0

const int WBD_WORD_TAG = 1;
const int WBD_IGNORE_TAG = 4;

// flag indicating the one-time initialization is done
volatile bool g_fInitialized = false;
std::mutex g_InitializationMutex; // this mutex is used once for default models only

// keep model data together
struct FAModelData
{
    // image of the loaded file
    FAImageDump m_Img;
    FALDB m_Ldb;

    // data and const processor for tokenization
    FAWbdConfKeeper m_Conf;
    FALexTools_t < int > m_Engine;
    bool m_hasWbd;

    // data and const processor for tokenization
    FADictConfKeeper m_DictConf;
    // indicates that a pos-dict data are present in the bin LDB file
    bool m_hasSeg;

    // Unigram LM algorithm
    FATokenSegmentationTools_1best_t < int > m_SegEngine;
    // BPE (with separate merge ranks, ID's are ranks) runtime
    FATokenSegmentationTools_1best_bpe_t < int > m_SegEngineBpe;
    // BPE (with separate merge ranks) runtime
    FATokenSegmentationTools_1best_bpe_with_merges_t < int > m_SegEngineBpeWithMerges;
    // one selected algorithm for this bin file
    const FATokenSegmentationToolsCA_t < int > * m_pAlgo;
    // indicates wether characters are bytes of the UTF-8 rather than the Unicode symbols
    bool m_useRawBytes;

    // Hyphenation / Syllabification data
    bool m_hasHy;
    FAHyphConfKeeper m_HyConf;
    FAHyphInterpreter_core_t < int > m_HyEngine;

    // id2word lexicon data
    bool m_hasI2w;
    FAStringArray_pack m_i2w;
    int m_min_token_id; // min regular token id, needed to separate special tokens
    int m_max_token_id; // max regular token id, needed to separate special tokens

    // TODO(twwhatever): probably need extra LDB for dictionary support in WRE

    FAModelData ():
        m_hasWbd (false),
        m_hasSeg (false),
        m_pAlgo (NULL),
        m_useRawBytes (false),
        m_hasHy (false),
        m_hasI2w (false),
        m_min_token_id (0),
        m_max_token_id (FALimits::MaxArrSize)
    {}
};

#ifndef SIZE_OPTIMIZATION
// keep two built-in models one for default WBD and one for default SBD
FAModelData g_DefaultWbd;
FAModelData g_DefaultSbd;
#endif

//
// returns the current version of the algo
//
extern "C"
const int GetBlingFireTokVersion()
{
    return (BLINGFIRETOK_MAJOR_VERSION_NUM * 1000) + BLINGFIRETOK_MINOR_VERSION_NUM;
}


#ifndef SIZE_OPTIMIZATION
// does initialization
void InitializeWbdSbd()
{
    const int * pValues = NULL;
    int iSize = 0;

    // initialize WBD
    g_DefaultWbd.m_Ldb.SetImage(g_dumpBlingFireTokLibWbdData);
    pValues = NULL;
    iSize = g_DefaultWbd.m_Ldb.GetHeader()->Get(FAFsmConst::FUNC_WBD, &pValues);
    g_DefaultWbd.m_Conf.Initialize(&g_DefaultWbd.m_Ldb, pValues, iSize);
    g_DefaultWbd.m_Engine.SetConf(&g_DefaultWbd.m_Conf);

    // initialize SBD
    g_DefaultSbd.m_Ldb.SetImage(g_dumpBlingFireTokLibSbdData);
    pValues = NULL;
    iSize = g_DefaultSbd.m_Ldb.GetHeader()->Get(FAFsmConst::FUNC_WBD, &pValues);
    g_DefaultSbd.m_Conf.Initialize(&g_DefaultSbd.m_Ldb, pValues, iSize);
    g_DefaultSbd.m_Engine.SetConf(&g_DefaultSbd.m_Conf);
}
#endif


inline int FAGetFirstNonWhiteSpace(int * pStr, const int StrLen)
{
    for (int i = 0; i < StrLen; ++i)
    {
        const int C = pStr[i];
        if (__FAIsWhiteSpace__(C)) {
            continue;
        }
        return i;
    }

    return StrLen;
}


//
// The same as TextToSentences, but it allows to use a custom model and returns offsets
//
// pStartOffsets is an array of integers (first character of each sentence) with upto MaxOutUtf8StrByteCount elements
// pEndOffsets is an array of integers (last character of each sentence) with upto MaxOutUtf8StrByteCount elements
//
// The hModel parameter allows to use a custom model loaded with LoadModel API, if NULL then
//  the built in is used.
//

extern "C"
const int TextToSentencesWithOffsetsWithModel(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, int * pStartOffsets, int * pEndOffsets, const int MaxOutUtf8StrByteCount,
    void * hModel)
{

#ifdef SIZE_OPTIMIZATION
    if (NULL == hModel) {
        return -1;
    }
#else
    // check if the initilization is needed
    if (false == g_fInitialized) {
        // make sure only one thread can get the mutex
        std::lock_guard<std::mutex> guard(g_InitializationMutex);
        // see if the g_fInitialized is still false
        if (false == g_fInitialized) {
            InitializeWbdSbd();
            g_fInitialized = true;
        }
    }

    // use the default model if it was not provided
    if (NULL == hModel) {
        hModel = &g_DefaultSbd;
    }

#endif

    // get the types right, hModel is always defined
    const FAModelData * pModel = (const FAModelData *) hModel;
    // validate the parameters
    if (0 == InUtf8StrByteCount) {
        return 0;
    }
    if (0 > InUtf8StrByteCount || InUtf8StrByteCount > FALimits::MaxArrSize) {
        return -1;
    }
    if (NULL == pInUtf8Str) {
        return -1;
    }

    // allocate buffer for UTF-32, sentence breaking results, word-breaking results
    std::vector< int > utf32input(InUtf8StrByteCount);
    int * pBuff = utf32input.data();
    if (NULL == pBuff) {
        return -1;
    }
    std::vector< int > utf32offsets(InUtf8StrByteCount);
    int * pOffsets = utf32offsets.data();
    if (NULL == pOffsets) {
        return -1;
    }
    // make sure there are no uninitialized offsets
    if (pStartOffsets) {
        memset(pStartOffsets, 0, MaxOutUtf8StrByteCount * sizeof(int));
    }
    if (pEndOffsets) {
        memset(pEndOffsets, 0, MaxOutUtf8StrByteCount * sizeof(int));
    }

    // convert input to UTF-32
    const int MaxBuffSize = ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, pOffsets, InUtf8StrByteCount);
    if (MaxBuffSize <= 0 || MaxBuffSize > InUtf8StrByteCount) {
        return -1;
    }
    // make sure the utf32input does not contain 'U+0000' elements
    std::replace(pBuff, pBuff + MaxBuffSize, 0, 0x20);

    // allocated a buffer for UTF-8 output
    std::vector< char > utf8output(InUtf8StrByteCount + 1);
    char * pTmpUtf8 = utf8output.data();
    if (NULL == pTmpUtf8) {
        return -1;
    }

    // keep sentence boundary information here
    std::vector< int > SbdRes(MaxBuffSize * 3);
    int * pSbdRes = SbdRes.data();
    if (NULL == pSbdRes) {
        return -1;
    }

    // get the sentence breaking results
    const int SbdOutSize = pModel->m_Engine.Process(pBuff, MaxBuffSize, pSbdRes, MaxBuffSize * 3);
    if (SbdOutSize > MaxBuffSize * 3 || 0 != SbdOutSize % 3) {
        return -1;
    }

    // number of sentences
    int SentCount = 0;
    // accumulate the output here
    std::ostringstream Os;
    // keep track if a sentence was already added
    bool fAdded = false;
    // set previous sentence end to -1
    int PrevEnd = -1;

    for (int i = 0; i < SbdOutSize; i += 3) {

        // we don't care about Tag or From for p2s task
        const int From = PrevEnd + 1;
        const int To = pSbdRes[i + 2];
        const int Len = To - From + 1;
        PrevEnd = To;

        // adjust sentence start if needed
        const int Delta = FAGetFirstNonWhiteSpace(pBuff + From, Len);
        if (Delta < Len) {
            // convert buffer to a UTF-8 string, we temporary use pOutUtf8Str, MaxOutUtf8StrByteCount
            const int StrOutSize = ::FAArrayToStrUtf8(pBuff + From + Delta, Len - Delta, pTmpUtf8, InUtf8StrByteCount);
            if (pStartOffsets && SentCount < MaxOutUtf8StrByteCount) {
                pStartOffsets[SentCount] = pOffsets[From + Delta];
            }
            if (pEndOffsets && SentCount < MaxOutUtf8StrByteCount) {
                const int ToCharSize = ::FAUtf8Size(pInUtf8Str + pOffsets[To]);
                pEndOffsets[SentCount] = pOffsets[To] + (0 < ToCharSize ? ToCharSize - 1 : 0);
            }
            SentCount++;

            // check the output size
            if (0 > StrOutSize || StrOutSize > InUtf8StrByteCount) {
                // should never happen, but happened :-(
                return -1;
            }
            else {
                // add a new line separator
                if (fAdded) {
                    Os << '\n';
                }
                // make sure this buffer does not contain '\n' since it is a delimiter
                std::replace(pTmpUtf8, pTmpUtf8 + StrOutSize, '\n', ' ');
                // actually copy the data into the string builder
                pTmpUtf8[StrOutSize] = 0;
                Os << pTmpUtf8;
                fAdded = true;
            }
        }
    }

    // always use the end of paragraph as the end of sentence
    if (PrevEnd + 1 < MaxBuffSize) {

        const int From = PrevEnd + 1;
        const int To = MaxBuffSize - 1;
        const int Len = To - From + 1;

        // adjust sentence start if needed
        const int Delta = FAGetFirstNonWhiteSpace(pBuff + From, Len);
        if (Delta < Len) {
            // convert buffer to a UTF-8 string, we temporary use pOutUtf8Str, MaxOutUtf8StrByteCount
            const int StrOutSize = ::FAArrayToStrUtf8(pBuff + From + Delta, Len - Delta, pTmpUtf8, InUtf8StrByteCount);
            if (pStartOffsets && SentCount < MaxOutUtf8StrByteCount) {
                pStartOffsets[SentCount] = pOffsets[From + Delta];
            }
            if (pEndOffsets && SentCount < MaxOutUtf8StrByteCount) {
                const int ToCharSize = ::FAUtf8Size(pInUtf8Str + pOffsets[To]);
                pEndOffsets[SentCount] = pOffsets[To] + (0 < ToCharSize ? ToCharSize - 1 : 0);
            }
            SentCount++;

            // check the output size
            if (0 > StrOutSize || StrOutSize > InUtf8StrByteCount) {
                // should never happen, but happened :-(
                return -1;
            }
            else {
                // add a new line separator
                if (fAdded) {
                    Os << '\n';
                }
                // make sure this buffer does not contain '\n' since it is a delimiter
                std::replace(pTmpUtf8, pTmpUtf8 + StrOutSize, '\n', ' ');
                // actually copy the data into the string builder
                pTmpUtf8[StrOutSize] = 0;
                Os << pTmpUtf8;
            }
        }
    }

    // we will include the 0 just in case some scriping languages expect 0-terminated buffers and cannot use the size
    Os << char(0);

    // get the actual output buffer as one string
    const std::string & OsStr = Os.str();
    const char * pStr = OsStr.c_str();
    const int StrLen = (int)OsStr.length();

    if (StrLen <= MaxOutUtf8StrByteCount) {
        memcpy(pOutUtf8Str, pStr, StrLen);
    }
    return StrLen;
}


//
// The same as TextToSentences, but this one also returns original offsets from the input buffer for each sentence.
//
// pStartOffsets is an array of integers (first character of each sentence) with upto MaxOutUtf8StrByteCount elements
// pEndOffsets is an array of integers (last character of each sentence) with upto MaxOutUtf8StrByteCount elements
//
extern "C"
const int TextToSentencesWithOffsets(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, int * pStartOffsets, int * pEndOffsets, const int MaxOutUtf8StrByteCount)
{
    return TextToSentencesWithOffsetsWithModel(pInUtf8Str, InUtf8StrByteCount, pOutUtf8Str,  pStartOffsets, pEndOffsets, MaxOutUtf8StrByteCount, NULL);
}


//
// The same as TextToSentences, but allows to load a custom model
//
// The hModel parameter allows to use a custom model loaded with LoadModel API, if NULL then
//  the built in is used.
//
extern "C"
const int TextToSentencesWithModel(const char * pInUtf8Str, int InUtf8StrByteCount, char * pOutUtf8Str, const int MaxOutUtf8StrByteCount, void * hModel)
{
    return TextToSentencesWithOffsetsWithModel(pInUtf8Str, InUtf8StrByteCount, pOutUtf8Str,  NULL, NULL, MaxOutUtf8StrByteCount, hModel);
}


//
// Splits plain-text in UTF-8 encoding into sentences.
//
// Input:  UTF-8 string of one paragraph or document
// Output: Size in bytes of the output string and UTF-8 string of '\n' delimited sentences, if the return size <= MaxOutUtf8StrByteCount
//
//  Notes:
//
//  1. The return value is -1 in case of an error (make sure your input is a valid UTF-8, BOM is not required)
//  2. We do not use "\r\n" to delimit sentences, it is always '\n'
//  3. It is not necessary to remove '\n' from the input paragraph, sentence breaking algorithm will take care of them
//  4. The output sentences will not contain '\n' in them
//
extern "C"
const int TextToSentences(const char * pInUtf8Str, int InUtf8StrByteCount, char * pOutUtf8Str, const int MaxOutUtf8StrByteCount)
{
    return TextToSentencesWithOffsetsWithModel(pInUtf8Str, InUtf8StrByteCount, pOutUtf8Str,  NULL, NULL, MaxOutUtf8StrByteCount, NULL);
}


//
// Same as TextToWords, but also returns original offsets from the input buffer for each word and allows to use a
//  custom model
//
// pStartOffsets is an array of integers (first character of each word) with upto MaxOutUtf8StrByteCount elements
// pEndOffsets is an array of integers (last character of each word) with upto MaxOutUtf8StrByteCount elements
//
// The hModel parameter allows to use a custom model loaded with LoadModel API, if NULL then
//  the built in is used.
//
extern "C"
const int TextToWordsWithOffsetsWithModel(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, int * pStartOffsets, int * pEndOffsets, int * pTags, const int MaxOutUtf8StrByteCount,
    void * hModel)
{
#ifdef SIZE_OPTIMIZATION
    if (NULL == hModel) {
        return -1;
    }
#else
    // check if the initilization is needed
    if (false == g_fInitialized) {
        // make sure only one thread can get the mutex
        std::lock_guard<std::mutex> guard(g_InitializationMutex);
        // see if the g_fInitialized is still false
        if (false == g_fInitialized) {
            InitializeWbdSbd();
            g_fInitialized = true;
        }
    }

    // use a default model if none was provided
    if (NULL == hModel) {
        hModel = &g_DefaultWbd;
    }
#endif

    // pModel is always initialized here
    const FAModelData * pModel = (const FAModelData *) hModel;

    // validate the parameters
    if (0 == InUtf8StrByteCount) {
        return 0;
    }
    if (0 > InUtf8StrByteCount || InUtf8StrByteCount > FALimits::MaxArrSize) {
        return -1;
    }
    if (NULL == pInUtf8Str) {
        return -1;
    }

    // allocate buffer for UTF-32, sentence breaking results, word-breaking results
    std::vector< int > utf32input(InUtf8StrByteCount);
    int * pBuff = utf32input.data();
    if (NULL == pBuff) {
        return -1;
    }

    std::vector< int > utf32offsets(InUtf8StrByteCount);
    int * pOffsets = utf32offsets.data();
    if (NULL == pOffsets) {
        return -1;
    }
    // make sure there are no uninitialized offsets
    if (pStartOffsets) {
        memset(pStartOffsets, 0, MaxOutUtf8StrByteCount * sizeof(int));
    }
    if (pEndOffsets) {
        memset(pEndOffsets, 0, MaxOutUtf8StrByteCount * sizeof(int));
    }
    if (pTags) {
        memset(pTags, 0, MaxOutUtf8StrByteCount * sizeof(int));
    }

    // convert input to UTF-32
    const int MaxBuffSize = ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, pOffsets, InUtf8StrByteCount);
    if (MaxBuffSize <= 0 || MaxBuffSize > InUtf8StrByteCount) {
        return -1;
    }
    // make sure the utf32input does not contain 'U+0000' elements
    std::replace(pBuff, pBuff + MaxBuffSize, 0, 0x20);

    // allocated a buffer for UTF-8 output
    std::vector< char > utf8output(InUtf8StrByteCount + 1);
    char * pTmpUtf8 = utf8output.data();
    if (NULL == pTmpUtf8) {
        return -1;
    }

    // keep sentence boundary information here
    std::vector< int > WbdRes(MaxBuffSize * 3);
    int * pWbdRes = WbdRes.data();
    if (NULL == pWbdRes) {
        return -1;
    }

    // get the sentence breaking results
    const int WbdOutSize = pModel->m_Engine.Process(pBuff, MaxBuffSize, pWbdRes, MaxBuffSize * 3);
    if (WbdOutSize > MaxBuffSize * 3 || 0 != WbdOutSize % 3) {
        return -1;
    }

    // keep track of the word count
    int WordCount = 0;
    // accumulate the output here
    std::ostringstream Os;
    // keep track if a word was already added
    bool fAdded = false;

    for (int i = 0; i < WbdOutSize; i += 3) {

        // ignore tokens with IGNORE tag
        const int Tag = pWbdRes[i];
        if (WBD_IGNORE_TAG == Tag) {
            continue;
        }

        const int From = pWbdRes[i + 1];
        const int To = pWbdRes[i + 2];
        const int Len = To - From + 1;

        // convert buffer to a UTF-8 string, we temporary use pOutUtf8Str, MaxOutUtf8StrByteCount
        const int StrOutSize = ::FAArrayToStrUtf8(pBuff + From, Len, pTmpUtf8, InUtf8StrByteCount);
        if (pStartOffsets && WordCount < MaxOutUtf8StrByteCount) {
            pStartOffsets[WordCount] = pOffsets[From];
        }
        if (pEndOffsets && WordCount < MaxOutUtf8StrByteCount) {
            // offset of last UTF-32 character plus its length in bytes in the original string - 1
            const int ToCharSize = ::FAUtf8Size(pInUtf8Str + pOffsets[To]);
            pEndOffsets[WordCount] = pOffsets[To] + (0 < ToCharSize ? ToCharSize - 1 : 0);
        }
        if (pTags && WordCount < MaxOutUtf8StrByteCount) {
            pTags[WordCount] = Tag;
        }
        WordCount++;

        // check the output size
        if (0 > StrOutSize || StrOutSize > InUtf8StrByteCount) {
            // should never happen, but happened :-(
            return -1;
        }
        else {
            // add a new line separator
            if (fAdded) {
                Os << ' ';
            }
            // make sure this buffer does not contain ' ' since it is a delimiter
            std::replace(pTmpUtf8, pTmpUtf8 + StrOutSize, ' ', '_');
            // actually copy the data into the string builder
            pTmpUtf8[StrOutSize] = 0;
            Os << pTmpUtf8;
            fAdded = true;
        }
    }

    // we will include the 0 just in case some scriping languages expect 0-terminated buffers and cannot use the size
    Os << char(0);

    // get the actual output buffer as one string
    const std::string & OsStr = Os.str();
    const char * pStr = OsStr.c_str();
    const int StrLen = (int)OsStr.length();

    if (StrLen <= MaxOutUtf8StrByteCount) {
        memcpy(pOutUtf8Str, pStr, StrLen);
    }
    return StrLen;
}


//
// Same as TextToWords, but also returns original offsets from the input buffer for each word.
//
// pStartOffsets is an array of integers (first character of each word) with upto MaxOutUtf8StrByteCount elements
// pEndOffsets is an array of integers (last character of each word) with upto MaxOutUtf8StrByteCount elements
//
extern "C"
const int TextToWordsWithOffsets(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, int * pStartOffsets, int * pEndOffsets, const int MaxOutUtf8StrByteCount)
{
    return TextToWordsWithOffsetsWithModel(pInUtf8Str,InUtf8StrByteCount,
        pOutUtf8Str, pStartOffsets, pEndOffsets, NULL, MaxOutUtf8StrByteCount, NULL);
}


//
// Same as TextToWords, but allows to load a custom model
//
// The hModel parameter allows to use a custom model loaded with LoadModel API, if NULL then
//  the built in is used.
//
extern "C"
const int TextToWordsWithModel(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, const int MaxOutUtf8StrByteCount, void * hModel)
{
    return TextToWordsWithOffsetsWithModel(pInUtf8Str,InUtf8StrByteCount,
        pOutUtf8Str, NULL, NULL, NULL, MaxOutUtf8StrByteCount, hModel);
}


//
// Splits plain-text in UTF-8 encoding into words.
//
// Input:  UTF-8 string of one sentence or paragraph or document
// Output: Size in bytes of the output string and UTF-8 string of ' ' delimited words, if the return size <= MaxOutUtf8StrByteCount
//
//  Notes:
//
//  1. The return value is -1 in case of an error (make sure your input is a valid UTF-8, BOM is not required)
//  2. Words from the word-breaker will not contain spaces
//
extern "C"
const int TextToWords(const char * pInUtf8Str, int InUtf8StrByteCount, char * pOutUtf8Str, const int MaxOutUtf8StrByteCount)
{
    return TextToWordsWithOffsetsWithModel(pInUtf8Str, InUtf8StrByteCount, pOutUtf8Str, NULL, NULL, NULL, MaxOutUtf8StrByteCount, NULL);
}

extern "C"
const int TextToWordsWithTagsWithModel(const char * pInUtf8Str, int InUtf8StrByteCount, char * pOutUtf8Str, int* pTags, const int MaxOutUtf8StrByteCount, void * hModel) {
    return TextToWordsWithOffsetsWithModel(pInUtf8Str, InUtf8StrByteCount, pOutUtf8Str, NULL, NULL, pTags, MaxOutUtf8StrByteCount, hModel);
}

//
// This function is like TextToWords, but it only normalizes consequtive spaces, it is not as flexble
//  as TextToWords as it cannot take a tokenization and normalization rules, but it does space normalization
//  and srinking multiple spaces to one faster.
//
// Input:  UTF-8 string of one sentence or paragraph or document
//         space character to use, default is __FASpDelimiter__ (U+x2581)
// Output: Size in bytes of the output string and UTF-8 string of ' ' delimited words, if the return size <= MaxOutUtf8StrByteCount
//
// Notes:
//  The return value is -1 in case of an error (make sure your input is a valid UTF-8, BOM is not required)
//
extern "C"
const int NormalizeSpaces(const char * pInUtf8Str, int InUtf8StrByteCount, char * pOutUtf8Str, const int MaxOutUtf8StrByteCount, const int uSpace = __FASpDelimiter__)
{
    // we can get weird results if this is not true
    DebugLogAssert(__FAIsWhiteSpace__(uSpace));

    if (0 == InUtf8StrByteCount) {
        return -1;
    }

    // allocate buffer for UTF-32, sentence breaking results, word-breaking results
    std::vector< int > utf32input(InUtf8StrByteCount);
    int * pBuff = utf32input.data();
    if (NULL == pBuff) {
        return -1;
    }

    // convert input to UTF-32
    int MaxBuffSize = ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, InUtf8StrByteCount);
    if (MaxBuffSize <= 0 || MaxBuffSize > InUtf8StrByteCount) {
        return -1;
    }

    int i = 0; // index for reading
    int j = 0; // index for writing
    while (i < MaxBuffSize) {

        const int Ci = pBuff[i++];

        // check if the Ci is not a space
        if (!__FAIsWhiteSpace__(Ci)) {
            // copy it
            pBuff[j++] = Ci;
        // if Ci is a space, check if the previous character was not a sapce
        } else if (0 < j && uSpace != pBuff[j - 1]) {
            // copy normalized space
            pBuff[j++] = uSpace;
        }
    } // of while ...

    // trim the final space if there was no content characters after
    if (1 < j && pBuff[j - 1] == uSpace) {
        j--;
    }

    MaxBuffSize = j;

    // convert UTF-32 back to UTF-8
    const int StrOutSize = ::FAArrayToStrUtf8(pBuff, MaxBuffSize, pOutUtf8Str, MaxOutUtf8StrByteCount);
    if (0 <= StrOutSize && StrOutSize < MaxOutUtf8StrByteCount) {
        pOutUtf8Str [StrOutSize] = 0;
    }

    return StrOutSize;
}


// This function implements the fasttext hashing function
inline const uint32_t GetHash(const char * str, size_t strLen)
{
    uint32_t h = 2166136261;
    for (size_t i = 0; i < strLen; i++) {
        h = h ^ uint32_t(int8_t(str[i]));
        h = h * 16777619;
    }
    return h;
}


// EOS symbol
int32_t EOS_HASH = GetHash("</s>", 4);

const void AddWordNgrams(int32_t * hashArray, int& hashCount, int32_t wordNgrams, int32_t bucket)
{
    // hash size
    const int tokenCount = hashCount;
    for (int32_t i = 0; i < tokenCount; i++) {
        uint64_t h = hashArray[i];
        for (int32_t j = i + 1; j < i + wordNgrams; j++) {
            uint64_t tempHash = (j < tokenCount) ? hashArray[j] : EOS_HASH;  // for computing ngram, we pad by EOS_HASH to allow each word has ngram which begins at itself.
            h = h * 116049371 + tempHash;
            const int ngram_idx = j - i;
            hashArray[(ngram_idx * tokenCount) + i] = (h % bucket);
        }
    }
    hashCount += (wordNgrams - 1) * tokenCount;
}


// Get the tokens count by inspecting the spaces in input buffer
inline const int GetTokenCount(const char * input, const int bufferSize)
{
    if (bufferSize == 0)
    {
        return 0;
    }
    else
    {
        int spaceCount = 0;
        for (int i = 0; i < bufferSize; ++i)
        {
            if (input[i] == ' ')
            {
                spaceCount++;
            }
        }
        return spaceCount + 1;
    }
}


// Port the fast text getline function with modifications
// 1. do not have vocab
// 2. do not compute subwords info
const int ComputeHashes(const char * input, const int strLen, int32_t * hashArr, int wordNgrams, int bucketSize)
{
    int hashCount = 0;
    const char * pTemp = input;
    const char * pWordStart = pTemp;
    size_t wordLength = 0;

    // add unigram hash first while reading the tokens. Unlike fasttext, there's no EOS padding here.
    for (int pos = 0; pos <= strLen; pos++)
    {
        // check for end of line first otherwise check the character so we don't read past the buffer end
        if (pos == strLen || pTemp[pos] == ' ')  // if we hit word boundary pushback wordhash or end of sentence
        {
            uint32_t wordhash = GetHash(pWordStart, wordLength);
            hashArr[hashCount] = wordhash;
            ++hashCount;
            // move pointer to next word and reset length
            pWordStart = pTemp + pos + 1;
            wordLength = 0;
        }
        else // otherwise keep moving temp pointer
        {
            ++wordLength;
        }
    }

    //Add higher order ngrams (bigrams and up), each token has a ngram starting from itself makes the size ngram * ntokens.
    AddWordNgrams(hashArr, hashCount, wordNgrams, bucketSize);

    return hashCount;
}


// memory cap for stack memory allocation
const int MAX_ALLOCA_SIZE = 204800;


//
// This function implements the fasttext-like hashing logics. It assumes the input text is already tokenized and
//  tokens are merged by a single space.
//
// Example: input :  "This is ok ."
//          output (bigram):  hash(this), hash(is), hash(ok), hash(.), hash(this is), hash(is ok), hash(ok .), hash(. EOS)
//
//  This is different from fasttext hashing:
//     1. no EOS padding by default for unigram.
//     2. do not take vocab as input, all unigrams are hashed too (we assume later caller repalces unigram hashes
//          with ids from a vocab file).
//
extern "C"
const int TextToHashes(const char * pInUtf8Str, int InUtf8StrByteCount, int32_t * pHashArr, const int MaxHashArrLength, int wordNgrams, int bucketSize = 2000000)
{
    // must have positive ngram
    if (wordNgrams <= 0 && InUtf8StrByteCount < 0)
    {
        return -1;
    }

    // get token count
    const int tokenCount = GetTokenCount(pInUtf8Str, InUtf8StrByteCount);

    // if memory allocation is not enough for current output, return requested memory amount
    if (tokenCount * wordNgrams >= MaxHashArrLength)
    {
        return InUtf8StrByteCount * wordNgrams;
    }

    // if correctly tokenized and the memory usage is within the limit
    const int hashArrSize = ComputeHashes(pInUtf8Str, InUtf8StrByteCount, pHashArr, wordNgrams, bucketSize);
    assert(hashArrSize == wordNgrams * tokenCount);

    return hashArrSize;
}


//
// WordHyphenationWithModel - returns a hyphenated string for an input word.
//
// The hModel parameter allows to use a model loaded with LoadModel API,
//  NULL is not allowed since there is no built in model for this function
//
extern "C"
const int WordHyphenationWithModel(const char * pInUtf8Str, int InUtf8StrByteCount,
    char * pOutUtf8Str, const int MaxOutUtf8StrByteCount, void * hModel, const int uHy = __FADefaultHyphen__)
{
    // pModel is always initialized here
    const FAModelData * pModel = (const FAModelData *) hModel;

    // validate the parameters
    if (0 == InUtf8StrByteCount) {
        return 0;
    }
    if (0 > InUtf8StrByteCount || InUtf8StrByteCount > FALimits::MaxArrSize) {
        return -1;
    }
    if (NULL == pInUtf8Str) {
        return -1;
    }

    // allocate a buffer for UTF-32 representation of a word, also keep it within limits
    int pBuff [FALimits::MaxWordSize];

    // convert input to UTF-32
    const int MaxBuffSize = ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, FALimits::MaxWordSize);
    if (MaxBuffSize <= 0 || MaxBuffSize > FALimits::MaxWordSize) {
        return -1;
    }
    // make sure the utf32input does not contain 'U+0000' elements
    std::replace(pBuff, pBuff + MaxBuffSize, 0, 0x20);

    // keep inner hyphenation results here
    int pHyRes [FALimits::MaxWordSize];
    // get the sentence breaking results
    const int Res = pModel->m_HyEngine.Process(pBuff, MaxBuffSize, pHyRes, MaxBuffSize);
    if (-1 == Res) {
        return -1;
    }

    // one character buffer
    const int MaxSize = FAUtf8Const::MAX_CHAR_SIZE + 1;
    char Utf8Symbol [MaxSize];

    // convert UTF-32 hyphen to UTF-8 once to use it later
    char Utf8HySymbol [MaxSize];
    char * pHyEnd = FAIntToUtf8 (uHy, Utf8HySymbol, MaxSize);
    if (NULL == pHyEnd) {
        return -1;
    }
    const size_t Utf8HyLen = pHyEnd - Utf8HySymbol;

    // keep track of how many bytes were outputted or needed to get a full output
    int TotalOutputSizeNeeded = 0;

    for (int i = 0; i < MaxBuffSize; ++i) {

        // output current input symbol
        const int Symbol = pBuff [i];
        char * pEnd = FAIntToUtf8 (Symbol, Utf8Symbol, MaxSize);
        if (NULL == pEnd) {
            return -1;
        }
        const size_t Utf8SymbolLen = pEnd - Utf8Symbol;

        if (NULL != pOutUtf8Str && TotalOutputSizeNeeded + ((int) Utf8SymbolLen) <= MaxOutUtf8StrByteCount) {
            memcpy(pOutUtf8Str + TotalOutputSizeNeeded, Utf8Symbol, Utf8SymbolLen);
        }
        TotalOutputSizeNeeded += (int) Utf8SymbolLen;

        // get corresponding hyphenation id
        const int HyphId = pHyRes [i];

        // output a hyphen, if it is any kind of hyphenation
        if (FAFsmConst::HYPH_NO_HYPH < HyphId) {
            if (NULL != pOutUtf8Str && TotalOutputSizeNeeded + ((int) Utf8HyLen) <= MaxOutUtf8StrByteCount) {
                memcpy(pOutUtf8Str + TotalOutputSizeNeeded, Utf8HySymbol, Utf8HyLen);
            }
            TotalOutputSizeNeeded += (int) Utf8HyLen;
        }

    } // for (int i = 0; ...

    // we will include the 0 just in case some scriping languages expect 0-terminated buffers and cannot use the size
    if (NULL != pOutUtf8Str && TotalOutputSizeNeeded < MaxOutUtf8StrByteCount) {
        pOutUtf8Str[TotalOutputSizeNeeded] = 0;
        TotalOutputSizeNeeded++;
    }

    return TotalOutputSizeNeeded;
}


//
// Helper, sets up pNewModelData object with model data from memory
// Returns 0 in case of an error otherwise initialized pNewModelData object is returned
//
extern "C"
void* SetModelData(FAModelData * pNewModelData, const unsigned char * pImgBytes)
{
    if (NULL == pNewModelData) {
        return NULL;
    }
    if (NULL == pImgBytes) {
        return NULL;
    }

    // create a generic LDB object from bytes
    pNewModelData->m_Ldb.SetImage (pImgBytes);

    // get the configuration paramenters for [wbd]
    const int * pValues = NULL;
    int iSize = pNewModelData->m_Ldb.GetHeader ()->Get (FAFsmConst::FUNC_WBD, &pValues);

    // see if the [wbd] section is present
    if (-1 != iSize) {

        pNewModelData->m_hasWbd = true;
        // initialize WBD configuration
        pNewModelData->m_Conf.Initialize (&(pNewModelData->m_Ldb), pValues, iSize);
        // now initialize the engine
        pNewModelData->m_Engine.SetConf(&(pNewModelData->m_Conf));
    }

    // get the configuration paramenters for [pos-dict]
    pValues = NULL;
    iSize = pNewModelData->m_Ldb.GetHeader ()->Get (FAFsmConst::FUNC_POS_DICT, &pValues);

    // see if the [pos-dict] section is present
    if (-1 != iSize) {

        pNewModelData->m_hasSeg = true;

        // initialize dict configuration
        pNewModelData->m_DictConf.SetLDB (&(pNewModelData->m_Ldb));
        pNewModelData->m_DictConf.Init (pValues, iSize);

        // initialize algorithm based on pNewModelData->m_DictConf.GetTokAlgo()
        if (FAFsmConst::TOKENIZE_BPE == pNewModelData->m_DictConf.GetTokAlgo() ||
            FAFsmConst::TOKENIZE_BPE_OPT == pNewModelData->m_DictConf.GetTokAlgo()) {

            pNewModelData->m_SegEngineBpe.SetConf(&pNewModelData->m_DictConf);
            pNewModelData->m_pAlgo = &(pNewModelData->m_SegEngineBpe);

        } else if (FAFsmConst::TOKENIZE_BPE_OPT_WITH_MERGES == pNewModelData->m_DictConf.GetTokAlgo()) {

            pNewModelData->m_SegEngineBpeWithMerges.SetConf(&pNewModelData->m_DictConf);
            pNewModelData->m_pAlgo = &(pNewModelData->m_SegEngineBpeWithMerges);

        } else {

            pNewModelData->m_SegEngine.SetConf(&pNewModelData->m_DictConf);
            pNewModelData->m_pAlgo = &(pNewModelData->m_SegEngine);
        }

        // see if we need to treat UTF-8 bytes as input
        pNewModelData->m_useRawBytes = pNewModelData->m_DictConf.GetUseByteEncoding();
    }

    // get the configuration paramenters for hyphenation [w2h]
    pValues = NULL;
    iSize = pNewModelData->m_Ldb.GetHeader ()->Get (FAFsmConst::FUNC_W2H, &pValues);

    // see if the [pos-dict] section is present
    if (-1 != iSize) {

        pNewModelData->m_hasHy = true;

        // initialize hyph configuration
        pNewModelData->m_HyConf.SetLDB (&(pNewModelData->m_Ldb));
        pNewModelData->m_HyConf.Init (pValues, iSize);

        // initialize the algo with data
        pNewModelData->m_HyEngine.SetConf (&pNewModelData->m_HyConf);
    }


    // get the configuration paramenters for hyphenation [i2w]
    pNewModelData->m_min_token_id = 0;
    pNewModelData->m_max_token_id = FALimits::MaxArrSize;
    pNewModelData->m_hasI2w = false;

    pValues = NULL;
    iSize = pNewModelData->m_Ldb.GetHeader ()->Get (FAFsmConst::FUNC_I2W, &pValues);

    // see if the [pos-dict] section is present
    if (-1 != iSize) {

        for (int i = 0; i < iSize; ++i) {

            if (pValues [i] == FAFsmConst::PARAM_STRING_ARRAY && i + 1 < iSize) {

                const int DumpNum = pValues [++i];
                const unsigned char * pDump = pNewModelData->m_Ldb.GetDump (DumpNum);

                if (NULL == pDump) {
                    return NULL;
                }

                pNewModelData->m_i2w.SetImage(pDump);
                pNewModelData->m_hasI2w = true;

            } else if (pValues [i] == FAFsmConst::PARAM_TOKENID_MIN && i + 1 < iSize) {

                pNewModelData->m_min_token_id = pValues [++i];

            } else if (pValues [i] == FAFsmConst::PARAM_TOKENID_MAX && i + 1 < iSize) {

                pNewModelData->m_max_token_id = pValues [++i];
            }
        }

        if (1 < iSize && pValues [0] == FAFsmConst::PARAM_STRING_ARRAY) {

            const int DumpNum = pValues [1];
            const unsigned char * pDump = pNewModelData->m_Ldb.GetDump (DumpNum);

            if (NULL == pDump) {
                return NULL;
            }

            pNewModelData->m_i2w.SetImage(pDump);
            pNewModelData->m_hasI2w = true;
        }
    }

    return (void*) pNewModelData;
}


//
// Same as LoadModel except a model is created from a memory pointer
// Returns 0 in case of an error
//
extern "C"
void* SetModel(const unsigned char * pImgBytes, int ModelByteCount)
{
    FAModelData * pNewModelData = new FAModelData();
    if (NULL == pNewModelData) {
        return 0;
    }

    // load the bin file
    if (NULL == pImgBytes || 0 == ModelByteCount) {
        return 0;
    }

    // return the initialized model handle
    return SetModelData(pNewModelData, pImgBytes);
}


//
// Loads a model and return a handle.
// Returns 0 in case of an error.
//
extern "C"
void* LoadModel(const char * pszLdbFileName)
{
    FAModelData * pNewModelData = new FAModelData();
    if (NULL == pNewModelData) {
        return 0;
    }

    // load the bin file
    pNewModelData->m_Img.Load (pszLdbFileName);
    const unsigned char * pImgBytes = pNewModelData->m_Img.GetImageDump ();
    if (NULL == pImgBytes) {
        return 0;
    }

    // return the initialized model handle
    return SetModelData(pNewModelData, pImgBytes);
}


//
// Implements a word-piece algorithm. Returns ids of words or sub-words, returns upto MaxIdsArrLength ids,
// the rest of the array is unchanged, so the array can be set to initial length and fill with 0's for padding.
// If pStartOffsets and pEndOffsets are not NULL then fills in the start and end offset for each token.
// Return value is the number of ids copied into the array.
//
// Example:
//  input: Эpple pie.
//  fa_lex output: эpple/WORD э/WORD_ID_1208 pp/WORD_ID_9397 le/WORD_ID_2571 pie/WORD pie/WORD_ID_11345 ./WORD ./WORD_ID_1012
//  TextToIds output: [1208, 9397, 2571, 11345, 1012, ... <unchanged>]
//
extern "C"
const int TextToIdsWithOffsets_wp(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        int * pStartOffsets,
        int * pEndOffsets,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    // validate the parameters
    if (0 >= InUtf8StrByteCount || InUtf8StrByteCount > FALimits::MaxArrSize || NULL == pInUtf8Str || 0 == ModelPtr) {
        return 0;
    }

    // allocate buffer for UTF-8 --> UTF-32 conversion
    std::vector< int > utf32input(InUtf8StrByteCount);
    int * pBuff = utf32input.data();
    if (NULL == pBuff) {
        return 0;
    }

    // a container for the offsets
    std::vector< int > utf32offsets;
    int * pOffsets = NULL;

    // flag to alter the logic in case we don't need the offsets
    const bool fNeedOffsets = NULL != pStartOffsets && NULL != pEndOffsets;

    if (fNeedOffsets) {
        utf32offsets.resize(InUtf8StrByteCount);
        pOffsets = utf32offsets.data();
        if (NULL == pOffsets) {
            return 0;
        }
    }

    // convert input to UTF-32, track offsets if needed
    int BuffSize = fNeedOffsets ?
        ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, pOffsets, InUtf8StrByteCount) :
        ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff, InUtf8StrByteCount);
    if (BuffSize <= 0 || BuffSize > InUtf8StrByteCount) {
        return 0;
    }

    // needed for normalization
    std::vector< int > utf32input_norm;
    int * pNormBuff = NULL;
    std::vector< int > utf32norm_offsets;
    int * pNormOffsets = NULL;

    // get the model data
    const FAModelData * pModelData = (const FAModelData *)ModelPtr;
    const FAWbdConfKeeper * pConf = &(pModelData->m_Conf);
    const FAMultiMapCA * pCharMap = pConf->GetCharMap ();

    // do the normalization for the entire input
    if (pCharMap) {

        utf32input_norm.resize(InUtf8StrByteCount);
        pNormBuff = utf32input_norm.data();
        if (NULL == pNormBuff) {
            return 0;
        }
        if (fNeedOffsets) {
            utf32norm_offsets.resize(InUtf8StrByteCount);
            pNormOffsets = utf32norm_offsets.data();
            if (NULL == pNormOffsets) {
                return 0;
            }
        }

        BuffSize = fNeedOffsets ?
            ::FANormalize(pBuff, BuffSize, pNormBuff, pNormOffsets, InUtf8StrByteCount, pCharMap) :
            ::FANormalize(pBuff, BuffSize, pNormBuff, InUtf8StrByteCount, pCharMap);
        if (BuffSize <= 0 || BuffSize > InUtf8StrByteCount) {
            return 0;
        }

        // use normalized buffer as input
        pBuff = pNormBuff;
    }

    // keep sentence boundary information here
    const int WbdResMaxSize = BuffSize * 6;
    std::vector< int > WbdRes(WbdResMaxSize);
    int * pWbdRes = WbdRes.data();
    if (NULL == pWbdRes) {
        return 0;
    }

    // compute token and sub-token boundaries
    const int WbdOutSize = pModelData->m_Engine.Process(pBuff, BuffSize, pWbdRes, WbdResMaxSize);
    if (WbdOutSize > WbdResMaxSize || 0 != WbdOutSize % 3) {
        return 0;
    }

    int OutCount = 0;

    // iterate over the results
    for(int i = 0; i < WbdOutSize; i += 3) {

        // ignore tokens with IGNORE tag
        const int Tag = pWbdRes[i];
        if (WBD_IGNORE_TAG == Tag) {
            continue;
        }

        // For each token with WORD tag copy all subword tags into the output if
        //  this word is covered completely by the subwords without gaps,
        //  otherwise copy the UnkId tag (this is how it's done in the original BERT TokenizerFull).
        if (WBD_WORD_TAG == Tag) {

            const int TokenFrom = pWbdRes[i + 1];
            const int TokenTo = pWbdRes[i + 2];

            // see if we have subtokens for this token and they cover the token completely
            int j = i + 3;
            int numSubTokens = 0;
            bool subTokensCoveredAll = false;

            if (j < WbdOutSize) {

                int ExpectedFrom = TokenFrom;
                int SubTokenTag = pWbdRes[j];
                int SubTokenFrom = pWbdRes[j + 1];
                int SubTokenTo = pWbdRes[j + 2];

                // '<=' because last subtoken should be included
                while (j <= WbdOutSize && SubTokenTag > WBD_IGNORE_TAG && ExpectedFrom == SubTokenFrom) {

                    ExpectedFrom = SubTokenTo + 1;
                    numSubTokens++;
                    j += 3;
                    if (j < WbdOutSize) {
                        SubTokenTag = pWbdRes[j];
                        SubTokenFrom = pWbdRes[j + 1];
                        SubTokenTo = pWbdRes[j + 2];
                    } // else it will break at the while check
                }

                // if subtoken To is the same as token To then we split the token all the way
                if (0 < numSubTokens && ExpectedFrom - 1 == TokenTo) {
                    // output all subtokens tags
                    for(int k = 0; k < numSubTokens && OutCount < MaxIdsArrLength; ++k) {

                        const int TagIdx = ((k + 1) * 3) + i;
                        const int SubTokenTag = pWbdRes[TagIdx];

                        if (OutCount < MaxIdsArrLength) {

                            pIdsArr[OutCount] = SubTokenTag;

                            if (fNeedOffsets) {

                                const int SubTokenFrom = pWbdRes[TagIdx + 1];
                                const int FromOffset = pOffsets[(pCharMap) ? pNormOffsets [SubTokenFrom] : SubTokenFrom];
                                pStartOffsets[OutCount] = FromOffset;

                                const int SubTokenTo = pWbdRes[TagIdx + 2];
                                const int ToOffset = pOffsets[(pCharMap) ? pNormOffsets [SubTokenTo] : SubTokenTo];
                                const int ToCharSize = ::FAUtf8Size(pInUtf8Str + ToOffset);
                                pEndOffsets[OutCount] = ToOffset + (0 < ToCharSize ? ToCharSize - 1 : 0);
                            }

                            OutCount++;
                        }
                    }
                    subTokensCoveredAll = true;
                }
            }

            if (false == subTokensCoveredAll) {
                // output an unk tag
                if (OutCount < MaxIdsArrLength) {

                    pIdsArr[OutCount] = UnkId;

                    // for unknown tokens take offsets from the word
                    if (fNeedOffsets) {

                        const int FromOffset = pOffsets[(pCharMap) ? pNormOffsets [TokenFrom] : TokenFrom];
                        pStartOffsets[OutCount] = FromOffset;

                        const int ToOffset = pOffsets[(pCharMap) ? pNormOffsets [TokenTo] : TokenTo];
                        const int ToCharSize = ::FAUtf8Size(pInUtf8Str + ToOffset);
                        pEndOffsets[OutCount] = ToOffset + (0 < ToCharSize ? ToCharSize - 1 : 0);
                    }

                    OutCount++;
                }
            }

            // skip i forward if we looped over any subtokens
            i = (j - 3);

        } // of if (WBD_WORD_TAG == Tag) ...

        if (OutCount >= MaxIdsArrLength) {
            break;
        }
    }

    return OutCount;
}


//
// The same as TextToIdsWithOffsets_wp, except does not return offsets
//
extern "C"
const int TextToIds_wp(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    return TextToIdsWithOffsets_wp(ModelPtr,pInUtf8Str,InUtf8StrByteCount,pIdsArr,NULL,NULL,MaxIdsArrLength,UnkId);
}


//
// Implements a sentence piece algorithm, returns predictions from FATokenSegmentationTools_1best_t.
// The input is always prepended with ' ' / '▁' since this seems the case in the sentence piece.
// Returns upto MaxIdsArrLength ids, the rest of the array is unchanged, so the array can be set to
// initial length and fill with 0's for padding. Returns number of ids copied into the array.
//
// Example:
// printf "Sergei Alonichau I saw a girl with a \ttelescope." | spm_encode --model=xlnet/spiece.model
// ▁Sergei ▁Al oni chau ▁I ▁saw ▁a ▁girl ▁with ▁a ▁telescope .
//
// printf "Sergei Alonichau I saw a girl with a \ttelescope." | spm_encode --model=xlnet/spiece.model --output_format=id
// 14363 651 7201 25263 35 685 24 1615 33 24 16163 9
//
// TextToIds_sp output: 12, [14363 651 7201 25263 35 685 24 1615 33 24 16163 9]
//
extern "C"
const int TextToIdsWithOffsets_sp(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        int * pStartOffsets,
        int * pEndOffsets,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    // validate the parameters
    if (0 >= InUtf8StrByteCount || InUtf8StrByteCount > FALimits::MaxArrSize || NULL == pInUtf8Str || 0 == ModelPtr) {
        return 0;
    }

    // allocate buffer for UTF-8 --> UTF-32 conversion
    std::vector< int > utf32input(InUtf8StrByteCount + 1);
    int * pBuff = utf32input.data();
    if (NULL == pBuff) {
        return 0;
    }
    pBuff[0] = __FASpDelimiter__; // always add a space in the beginning, SP uses U+2581 as a space mark

    // a container for the offsets
    std::vector< int > utf32offsets;
    int * pOffsets = NULL;

    // flag to alter the logic in case we don't need the offsets
    const bool fNeedOffsets = NULL != pStartOffsets && NULL != pEndOffsets;

    if (fNeedOffsets) {
        utf32offsets.resize(InUtf8StrByteCount + 1);
        pOffsets = utf32offsets.data();
        if (NULL == pOffsets) {
            return 0;
        }
        pOffsets[0] = -1; // an offset of the appended space (it is not a part of the input)
    }

    // get the model data
    const FAModelData * pModelData = (const FAModelData *)ModelPtr;
    const FADictConfKeeper * pConf = &(pModelData->m_DictConf);
    const FAMultiMapCA * pCharMap = pConf->GetCharMap ();

    // see if we need to get a summy space added
    const int BUFF_DATA_OFFSET = pConf->GetNoDummyPrefix() ? 0 : 1;

    // convert input to UTF-32 or bytes (write output past the added first space)
    int BuffSize;
    if(false == pModelData->m_useRawBytes) {
        BuffSize = fNeedOffsets ?
            ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff + BUFF_DATA_OFFSET, pOffsets + BUFF_DATA_OFFSET, InUtf8StrByteCount) :
            ::FAStrUtf8ToArray(pInUtf8Str, InUtf8StrByteCount, pBuff + BUFF_DATA_OFFSET, InUtf8StrByteCount);
    } else {
        BuffSize = fNeedOffsets ?
            ::FAStrUtf8AsBytesToArray(pInUtf8Str, InUtf8StrByteCount, pBuff + BUFF_DATA_OFFSET, pOffsets + BUFF_DATA_OFFSET, InUtf8StrByteCount) :
            ::FAStrUtf8AsBytesToArray(pInUtf8Str, InUtf8StrByteCount, pBuff + BUFF_DATA_OFFSET, InUtf8StrByteCount);
    }
    if (BuffSize <= 0 || BuffSize > InUtf8StrByteCount) {
        return 0;
    }
    BuffSize += BUFF_DATA_OFFSET; // to accomodate the first space

    // needed for normalization
    std::vector< int > utf32input_norm;
    int * pNormBuff = NULL;
    std::vector< int > utf32norm_offsets;
    int * pNormOffsets = NULL;

    // do normalization, if needed
    if (NULL != pCharMap) {

        const int MaxNormBuffSize = (InUtf8StrByteCount + 1) * 2;
        utf32input_norm.resize(MaxNormBuffSize);
        pNormBuff = utf32input_norm.data();
        if (NULL == pNormBuff) {
            return 0;
        }
        if (fNeedOffsets) {
            utf32norm_offsets.resize(MaxNormBuffSize);
            pNormOffsets = utf32norm_offsets.data();
            if (NULL == pNormOffsets) {
                return 0;
            }
        }

        // do the normalization for the entire input
        const int ActualNormBuffSize = fNeedOffsets ?
            ::FANormalize(pBuff, BuffSize, pNormBuff, pNormOffsets, MaxNormBuffSize, pCharMap) :
            ::FANormalize(pBuff, BuffSize, pNormBuff, MaxNormBuffSize, pCharMap);

        if (ActualNormBuffSize <= 0 || ActualNormBuffSize > MaxNormBuffSize) {
            pCharMap = NULL;
            // don't proceed without normalization, TODO: 99% times it does not change anything... so it is ok to proceed
            return 0;
        } else {
            BuffSize = ActualNormBuffSize;
            pBuff = pNormBuff;
        }
    }

    // Replace every space sequence with U+2581 in-place
    //
    // Note: This operation affect offsets. Since the output sequence is always the
    //       same length or shorter we can update the offsets in-place.
    //       If normalization is enbled the offsets are computed as a superposition of
    //       normalization and utf-32 offsets, then transformation should be applied to
    //       normalization offsets, otherwise to utf-32 offsets.
    //
    int * pAdjustedOffsets = fNeedOffsets ? (NULL != pCharMap ? pNormOffsets : pOffsets) : NULL;

    int i = 0; // index for reading
    int j = 0; // index for writing
    while (i < BuffSize) {

        const int Ci = pBuff[i];

        // check if the Ci is not a space
        if (!__FAIsWhiteSpace__(Ci)) {
            // copy it
            pBuff[j] = Ci;
            if (fNeedOffsets) {
                pAdjustedOffsets[j] = pAdjustedOffsets[i];
            }
            j++;
        // if Ci is a space, check if the previous character was not a space
        } else if (0 == j || __FASpDelimiter__ != pBuff[j - 1]) {
            // copy normalized space
            pBuff[j] = __FASpDelimiter__;
            if (fNeedOffsets) {
                pAdjustedOffsets[j] = pAdjustedOffsets[i];
            }
            j++;
        }

        i++;

    } // of while ...

    // trim the final space if there was no content characters after
    if (1 < j && pBuff[j - 1] == __FASpDelimiter__) {
        j--;
    }

    // adjust the length
    BuffSize = j;

    // do the segmentation
    const int WbdResMaxSize = BuffSize * 3;
    std::vector< int > WbdResults(WbdResMaxSize);
    int * pWbdResults = WbdResults.data ();

    // tokenize input with a selected algorithm
    const int WbdOutSize = pModelData->m_pAlgo->Process (pBuff, BuffSize, pWbdResults, WbdResMaxSize, UnkId);
    if (WbdOutSize > WbdResMaxSize || 0 != WbdOutSize % 3) {
        return 0;
    }

    int OutSize = 0;
    int IdOffset = pConf->GetIdOffset (); // see if we need to shift output IDs by a constant
    // return the ids only
    for (int i = 0; i < WbdOutSize && OutSize < MaxIdsArrLength; i += 3) {

        // copy id
        const int id = pWbdResults [i];
        pIdsArr [OutSize] = id + IdOffset;

        // copy offsets if needed
        if (fNeedOffsets) {

            const int TokenFrom = pWbdResults [i + 1];
            const int FromOffset = pOffsets[(pCharMap) ? pNormOffsets [TokenFrom] : TokenFrom];
            pStartOffsets[OutSize] = FromOffset;

            const int TokenTo = pWbdResults [i + 2];
            const int ToOffset = pOffsets[(pCharMap) ? pNormOffsets [TokenTo] : TokenTo];
            const int ToCharSize = ::FAUtf8Size(pInUtf8Str + ToOffset);
            pEndOffsets[OutSize] = ToOffset + (0 < ToCharSize ? ToCharSize - 1 : 0);
        }

        OutSize++;
    }

    return OutSize;
}


//
// The same as TextToIdsWithOffsets_sp, except does not return offsets
//
extern "C"
const int TextToIds_sp(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    return TextToIdsWithOffsets_sp(ModelPtr,pInUtf8Str,InUtf8StrByteCount,pIdsArr,NULL,NULL,MaxIdsArrLength,UnkId);
}


//
// Implements a word-piece or sentence piece algorithms which is defined by the loaded model.
// Returns ids of words or sub-words, returns upto MaxIdsArrLength ids, the rest of the array
// is unchanged, so the array can be set to initial length and fill with 0's for padding.
// Returns number of ids copied into the array.
//

extern "C"
const int TextToIdsWithOffsets(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        int * pStartOffsets,
        int * pEndOffsets,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    if (0 == ModelPtr) {
        return 0;
    }

    // check if loaded model has segmentation data
    const FAModelData * pModelData = (const FAModelData *)ModelPtr;

    if (!pModelData->m_hasSeg)
    {
        // call word-piece algorithm
        return TextToIdsWithOffsets_wp(
                ModelPtr,
                pInUtf8Str,
                InUtf8StrByteCount,
                pIdsArr,
                pStartOffsets,
                pEndOffsets,
                MaxIdsArrLength,
                UnkId
            );
    }
    else
    {
        // call sentence-piece algorithm
        return TextToIdsWithOffsets_sp(
                ModelPtr,
                pInUtf8Str,
                InUtf8StrByteCount,
                pIdsArr,
                pStartOffsets,
                pEndOffsets,
                MaxIdsArrLength,
                UnkId
            );
    }
}


//
// Implements a word-piece or sentence piece algorithms which is defined by the loaded model.
// Returns ids of words or sub-words, returns upto MaxIdsArrLength ids, the rest of the array
// is unchanged, so the array can be set to initial length and fill with 0's for padding.
// Returns number of ids copied into the array.
//

extern "C"
const int TextToIds(
        void* ModelPtr,
        const char * pInUtf8Str,
        int InUtf8StrByteCount,
        int32_t * pIdsArr,
        const int MaxIdsArrLength,
        const int UnkId = 0
)
{
    if (0 == ModelPtr) {
        return 0;
    }

    // check if loaded model has segmentation data
    const FAModelData * pModelData = (const FAModelData *)ModelPtr;

    if (!pModelData->m_hasSeg)
    {
        // call word-piece algorithm
        return TextToIdsWithOffsets_wp(ModelPtr, pInUtf8Str, InUtf8StrByteCount, pIdsArr, NULL, NULL, MaxIdsArrLength, UnkId);
    }
    else
    {
        // call sentence-piece algorithm
        return TextToIdsWithOffsets_sp(ModelPtr, pInUtf8Str, InUtf8StrByteCount, pIdsArr, NULL, NULL, MaxIdsArrLength, UnkId);
    }
}


//
// Frees memory from the model, after this call ModelPtr is no longer valid
//  Double calls to this function with the same argument will case access violation
//
extern "C"
int FreeModel(void* ModelPtr)
{
    if (NULL == ModelPtr) {
        return 0;
    }

    delete (FAModelData*) ModelPtr;
    return 1;
}


//
// Allows to change the "no-dummy-prefix" (NoDummyPrefix) without the recompilation of the models
// Note: it is the best to use the mode the same way it was trained / compiled leave this value to what it was set via ldb.conf.small file
//
extern "C"
int SetNoDummyPrefix(void* ModelPtr, bool fNoDummyPrefix)
{
    if (NULL == ModelPtr) {
        return 0;
    }

    FAModelData* pModel = (FAModelData*) ModelPtr;
    pModel->m_DictConf.SetNoDummyPrefix(fNoDummyPrefix);
    return 1;
}


//
// Returns text string given a sequence of Ids
//  Note: the model file should contain [i2w] configuration or separate *.i2w model file should be used
//
// return value is the actual string length
// if the actual string length is more than MaxOutUtf8StrByteCount then pOutUtf8Str content is undefined
//
extern "C"
int IdsToText (void* ModelPtr, const int32_t * pIdsArr, const int IdsCount, char * pOutUtf8Str, const int MaxOutUtf8StrByteCount, bool SkipSpecialTokens)
{
    if (NULL == ModelPtr) {
        return 0;
    }
    if (0 == IdsCount || NULL == pIdsArr) {
        return 0;
    }

    const FAModelData* pModel = (FAModelData*) ModelPtr;
    if (!pModel->m_hasI2w) {
        return 0;
    }

    int ActualLength = 0;

    for (int i = 0; i < IdsCount; ++i) {

        // get the next id
        const int id = pIdsArr [i];

        // skip special tokens, if needed
        if (SkipSpecialTokens && (id < pModel->m_min_token_id || id > pModel->m_max_token_id)) {
            continue;
        }

        // get token text
        const unsigned char * pToken = NULL;
        int TokenLength = pModel->m_i2w.GetAt (id, &pToken);
        if (0 > TokenLength) {
            return 0; // unknon id
        }

        // don't output space in the leading position
        if (0 == ActualLength && 0 < TokenLength && 0x20 == pToken[0]) {
            pToken++;
            TokenLength--;
        }

        // copy token text into the output buffer
        if (0 < TokenLength && MaxOutUtf8StrByteCount - ActualLength >= TokenLength) {
            memcpy(pOutUtf8Str + ActualLength, pToken, TokenLength);
        }

        ActualLength += TokenLength;
    }

    // add a terminating 0 for some interpreters
    if (MaxOutUtf8StrByteCount > ActualLength) {
        pOutUtf8Str [ActualLength] = 0;
    }
    ActualLength++;

    // return the actual length of the output (the minimum length needed to keep entire output)
    return ActualLength;
}

// This class is used to hold bracket results from WRE.
class BracketOutput : public FABrResultA {
    public:
    void AddRes(const int BrId, const int From, const int To) override  {

        // TODO(twwhatever): store from/to/etc. in a way we can retrieve them.
        // Probably just pass in the output arrays and fill them directly.
        // But need to also verify and return the output count.
    }
};

// Builds tagged text from input utf8, begin, end, tag arrays
class TaggedText : public FATaggedTextCA {
    private:
    std::vector<std::vector<int>> words_;
    std::vector<int> tags_;
    std::vector<int> offsets_;

   public:
    TaggedText(    const char * pInUtf8Str,
    int InUtf8StrByteCount,
    const int * pInStartOffsets,
    const int * pInEndOffsets,
    const int * pInTags, 
    int InTokenCount) {
        int Word[FALimits::MaxWordLen];

        for (auto i = 0; i < InTokenCount; ++i) {
            // UTF-8 -> UTF-32
            const int SymbolCount = FAStrUtf8ToArray(pInUtf8Str + pInStartOffsets[i], pInEndOffsets[i] - pInStartOffsets[i], Word, FALimits::MaxWordLen);
            FAAssert(0 <= SymbolCount && SymbolCount <= FALimits::MaxWordLen, "word length exceeds max");
            words_.push_back(std::vector<int>(Word, Word + SymbolCount));
            tags_.push_back(pInTags[i]);
            offsets_.push_back(pInStartOffsets[i]);
        }
    }

    // FATaggedTextCA
    const int GetWordCount() const override {
        return static_cast<int>(words_.size());
    }

    const int GetWord(const int Num, const int ** pWord) const override {
        *pWord = &words_[Num][0];
        return static_cast<int>(words_[Num].size());
    }

    const int GetTag(const int Num) const override {
        return tags_[Num];
    }

    const int GetOffset(const int Num) const override {
        return offsets_[Num];
    }

};

class TaggedWords : public FATaggedTextA {
    std::vector<std::vector<int>> words_;
    std::vector<int> tags_;
    std::vector<int> offsets_;
    public: 

    // FATaggedTextCA
    const int GetWordCount() const override {
        return static_cast<int>(words_.size());
    }

    const int GetWord(const int Num, const int ** pWord) const override {
        *pWord = &words_[Num][0];
        return static_cast<int>(words_[Num].size());
    }

    const int GetTag(const int Num) const override {
        return tags_[Num];
    }

    const int GetOffset(const int Num) const override {
        return offsets_[Num];
    }

    // FATaggedTextA
    void AddWord(const int * pWord, const int Length, const int Tag) override {
        words_.emplace_back(pWord, pWord + Length);
        tags_.push_back(Tag);
    }
    
    void AddWord (
            const int * pWord, 
            const int Length, 
            const int Tag, 
            const int Offset
        ) override {
            words_.emplace_back(pWord, pWord + Length);
            tags_.push_back(Tag);
            offsets_.push_back(Offset);
        }

    void Clear() override {
        words_.clear();
        tags_.clear();
        offsets_.clear();
    }

};

// TODO(twwhatever): Port from FAParseTree
class ParseTree : public FAParseTreeA {
    private:

    struct Node {
        int next_;   // right neighbour, -1 if no right neighbour
        int child_;  // left child, -1 if no left child
        int label_;
    };

    std::vector<Node> nodes_;
    std::vector<int> roots_;
    std::vector<int> root2label_;

    public:

    void Init(const int Count) override {
        nodes_.resize(Count);
        roots_.resize(Count);
        root2label_.resize(Count);

        if (Count == 0) {
            return;
        }

        const int Count_1 = Count - 1;

        for (int i = 0; i < Count_1; ++i) {
            nodes_[i].label_ = i;
            nodes_[i].next_ = i + 1;
            nodes_[i].child_ = -1;
            roots_[i] = i;
            root2label_[i] = i;
        }
        nodes_[Count_1].label_ = Count_1;
        nodes_[Count_1].next_ = -1;
        nodes_[Count_1].child_ = -1;
        roots_[Count_1] = Count_1;
        root2label_[Count_1] = Count_1;
    }


    void AddNode (const int Label, const int FromPos, const int ToPos) override {
           // allocate a new node
    const int NewNode = nodes_.size ();
    nodes_.resize (NewNode + 1);

    // get from-node and to-node
    const int ToNode = roots_ [ToPos];
    const int FromNode = roots_ [FromPos];

    // this is needed only if inclusion of consituents is allowed at single
    // stage of parsing (higher level constituents should be added first)
    roots_ [FromPos] = NewNode;

    // get nodes' data
    Node & NewNodeData = nodes_ [NewNode];
    Node & FromNodeData = nodes_ [FromNode];
    Node & ToNodeData = nodes_ [ToNode];

    // copy the content of a FromNode into a NewNode
    NewNodeData = FromNodeData;

    // setup a new label for an old FromNode
    FromNodeData.label_ = Label;
    // setup a new next-node link for an old FromNode
    FromNodeData.next_ = ToNodeData.next_;
    // setup a new child-node link for an old FromNode
    FromNodeData.child_ = NewNode;

    // setup -1 as a Next node for ToNode
    if (FromPos != ToPos)
        ToNodeData.next_ = -1;
    else
        NewNodeData.next_ = -1; 
    }

    void Update () override {
        roots_.resize(0);
        root2label_.resize(0);

        int NodeIdx = 0;

        while (-1 != NodeIdx) {
            auto& nodeData = nodes_[NodeIdx];
            roots_.push_back(NodeIdx);
            root2label_.push_back(nodeData.label_);

            NodeIdx = nodeData.next_;
        }
    }

    const int GetUpperNodes (const int ** ppNodes) const override {
        *ppNodes = &roots_[0];
        return static_cast<int>(roots_.size());
    }

    const int GetUpperLabels (const int ** ppLabels) const override {
        *ppLabels = &root2label_[0];
        return static_cast<int>(root2label_.size());
    }

    const int GetNext (const int Node) const override {
        return nodes_[Node].next_;
    }

    const int GetChild (const int Node) const override {
        return nodes_[Node].child_;
    }

    const int GetLabel (const int Node) const override {
        return nodes_[Node].label_;
    }
};

class Wre {
    private:
        FAParserConfKeeper conf_;
        FAMorphLDB_t<int>& ldb_;
        FADictInterpreter_t<int> tagDict_;
        FAWreLexTools_t<int> wre_;
        FAMultiMap_pack acts_;
        FAAllocator pool_;



    public:

    Wre(FAMorphLDB_t<int>& ldb) : ldb_(ldb) {
        // Load PRM LDB
        const FADictConfKeeper* pDictConf = ldb_.GetTagDictConf();

        if (pDictConf) {
            tagDict_.SetConf(pDictConf, ldb_.GetInTr());
        }

        // Load and set up the compiled WRE grammar.
        // TODO(twwhatever): I think we get everything from the ldb.
        const int* pValues = nullptr;
        const int Size = ldb_.GetHeader()->Get(FAFsmConst::FUNC_WRE, &pValues);
        conf_.Initialize(&ldb_, pValues, Size);
        wre_.Initialize(&pool_, &conf_, &tagDict_);

    }

    void Process(FAParseTreeA * pTree, const FATaggedTextCA * pIn) {
        const int WordCount = pIn->GetWordCount();
        pTree->Init(WordCount);
        wre_.Reset(WordCount);

        for (int i = 0; i < WordCount; ++i) {
            const int* pWord;
            const int WordLen = pIn->GetWord(i, &pWord);
            const int Tag = pIn->GetTag(i);
            wre_.AddWord(pWord, WordLen, Tag);
        }

        wre_.SetParseTree(pTree);
        wre_.Process();

    }
};

// TODO(twwhatever): I need some way to handle MWEs.  Maybe better to just port FAMergeMwe
// more or less entirely rather than pick and choose.

class MergeMwe {

    private:

    const FARSDfaCA* pDfa_;
    bool IgnoreCase_;
    int MweDelim_;

    int GetTokenCount(const int Pos, const FATaggedTextCA * pIn) const {
        const int WordCount = pIn->GetWordCount ();

    int State = pDfa_->GetInitial ();
    DebugLogAssert (-1 != State);

    int BestJ = Pos;

    for (int j = Pos; j < WordCount; ++j) {

        const int * pWord;
        const int WordLen = pIn->GetWord (j, &pWord);

        for (int k = 0; k < WordLen && -1 != State; ++k) {

            int Symbol = pWord [k];

            if (IgnoreCase_) {
                Symbol = FAUtf32ToLower (Symbol) ;
            }

            State = pDfa_->GetDest (State, Symbol);
        }
        if (-1 == State) {
            break;
        }
        if (pDfa_->IsFinal (State)) {
            BestJ = j;
        }

        State = pDfa_->GetDest (State, MweDelim_);

    } // for (j = i; ...

    const int TokenCount = BestJ + 1 - Pos;
    DebugLogAssert (1 <= TokenCount);

    return TokenCount;
    }

    public:
    MergeMwe(const FARSDfaCA* pDfa, bool ignoreCase, int mweDelim) : pDfa_(pDfa), IgnoreCase_(ignoreCase), MweDelim_(mweDelim) {}

    void Process(FATaggedTextA * pOut, const FATaggedTextCA * pIn) const {
    DebugLogAssert (pOut && pIn);

    int j;

    // MWE have the same length limit as an ordinary word
    const int MaxMweLen = FALimits::MaxWordLen + 1;
    int MweBuff [MaxMweLen];

    pOut->Clear ();

    const int WordCount = pIn->GetWordCount ();

    for (int i = 0; i < WordCount; ++i) {

        const int MweTokenCount = GetTokenCount (i, pIn);

        if (1 < MweTokenCount) {

            int MweLen = 0;

            for (j = 0; j < MweTokenCount; ++j) {

                const int * pWord;
                const int WordLen = pIn->GetWord (j + i, &pWord);

                if (MweLen + WordLen + 1 <= MaxMweLen) {
                    // copy token text
                    memcpy (MweBuff + MweLen, pWord, sizeof (int) * WordLen);
                    // copy a delimiter
                    MweBuff [MweLen + WordLen] = MweDelim_;
                    // increase MWE length
                    MweLen += WordLen + 1;
                } else {
                    // MWE is too long ...
                    break;
                }
            } // of for (j = 0; ...

            if (j == MweTokenCount) {

                DebugLogAssert (0 < MweLen);

                const int Offset = pIn->GetOffset (i);
                const int Tag = pIn->GetTag (i + MweTokenCount - 1);

                // MweLen - 1, ignores trailing delimiter
                pOut->AddWord (MweBuff, MweLen - 1, Tag, Offset);

                // skip middle tokens
                i += (MweTokenCount - 1);
                continue;
            }
        } // if (1 < MweTokenCount) ...

        // copy a single word
        const int * pWord;
        const int WordLen = pIn->GetWord (i, &pWord);
        const int Tag =  pIn->GetTag (i);
        const int Offset = pIn->GetOffset (i);

        pOut->AddWord (pWord, WordLen, Tag, Offset);

    } // of for (int i = 0; ...
    }
};

static std::pair<int, int> treeToTaggedIntervals_rec(const FAParseTreeA& tree, const int Root, std::vector<int>& tags, std::vector<int>& froms, std::vector<int>& tos) {
    int Node = Root;
    int From = -1;
    int To = -1;
    int Tag = -1;

    while (-1 != Node) {
        const int Label = tree.GetLabel(Node);
        if (0 <= Label) {
            // Add token to current span
            if (From == -1) {
                From = Label;
            }
            To = Label;
        } else {
            // Tagged tree node.
            Tag = -Label;
        }

        const int Child = tree.GetChild(Node);

        if (-1 != Child) {
            auto childInterval = treeToTaggedIntervals_rec(tree, Child, tags, froms, tos);
            auto subFrom = childInterval.first;
            auto subTo = childInterval.second;
            if (subFrom != -1) {
                From = subFrom;
            }
            if (subTo != -1) {
                To = subTo;
            }
        }
        // We have completed a tree at the current level.
        if (Tag != -1) {
            tags.push_back(Tag);
            froms.push_back(From);
            tos.push_back(To);
        }

        Node = tree.GetNext(Node);
    }
    return std::make_pair(From, To);
}

static void treeToTaggedIntervals(const FAParseTreeA& tree, std::vector<int>& tags, std::vector<int>& froms, std::vector<int>& tos) {
    const int* pNodes;
    const int Count = tree.GetUpperNodes(&pNodes);
    if (0 < Count) {
        treeToTaggedIntervals_rec(tree, *pNodes, tags, froms, tos);
    }
}

extern "C"
int ParseWre(
    const char * pInUtf8Str,
    int InUtf8StrByteCount,
    const int * pInStartOffsets,
    const int * pInEndOffsets,
    const int * pInTags, 
    int InTokenCount,
    int * pOutFroms,
    int * pOutTos,
    int * pOutTags,
    int MaxOutWordCount,
    void * hMweModel,
    void * hWreModel
)
{
    // TODO(twwhatever): test_wre.cpp is not the way.  The way we got working is actually
    // fa_ts2ts_mwe | fa_ts2ps --alg=wre.  I think we can combine them here though.
    // We need to implement FAMergeMwe (or move it from compile).  Then we need to implement
    // FATaggedTextA in such a way that AddWord feeds into AddWord for WRE.

    if (NULL == hMweModel || NULL == hWreModel) {
        return -1;
    }
    // TODO(twwhatever): I don't think it's possible to implicitly load the model here,
    // since it seems like that's hard-coded for word breaking models.

    FAModelData* pFAMweModel = reinterpret_cast<FAModelData*>(hMweModel);
    FAModelData* pFAWreModel = reinterpret_cast<FAModelData*>(hWreModel);
    if (NULL == pFAMweModel || NULL == pFAWreModel) {
        return -1;
    }

    // Configure Multi-word expression model
    FAMorphLDB_t<int> ldb;
    ldb.SetImage(pFAMweModel->m_Img.GetImageDump());
    const FADictConfKeeper * pConf = ldb.GetTagDictConf();
    const bool IgnoreCase = pConf->GetIgnoreCase();
    const FARSDfaCA * pDfa = pConf->GetRsDfa();
    MergeMwe mweMerger(pDfa, IgnoreCase, int(' '));

    // Configure WRE parser
    FAMorphLDB_t<int> wreLdb;
    wreLdb.SetImage(pFAWreModel->m_Img.GetImageDump());
    Wre wre(wreLdb);

    // Convert input
    TaggedText text(pInUtf8Str, InUtf8StrByteCount, pInStartOffsets, pInEndOffsets, pInTags, InTokenCount);

    // Match multiword expressions
    TaggedWords mweResult;
    mweMerger.Process(&mweResult, &text);

    // Match WRE
    ParseTree wreTree;
    wre.Process(&wreTree, &mweResult);
    
    std::vector<int> Tags;
    std::vector<int> Froms;
    std::vector<int> Tos;
    treeToTaggedIntervals(wreTree, Tags, Froms, Tos);

    // TODO(twwhatever): remap multiword expressions.
    std::vector<int> fromMap;
    std::vector<int> toMap;
    int textIndex = 0;
    for (int i = 0; i < mweResult.GetWordCount(); ++i, ++textIndex) {
        toMap.push_back(textIndex);
        while (textIndex < text.GetWordCount() && text.GetOffset(textIndex) < mweResult.GetOffset(i)) {
            ++textIndex;
        }
        if (i > 0) {
            toMap[i - 1] = textIndex - 1; 
        }
        fromMap.push_back(textIndex);
    }

    for (int i = 0; i < MaxOutWordCount && i < Tags.size(); ++i) {
        pOutTags[i] = Tags[i];
        pOutFroms[i] = fromMap[Froms[i]];
        pOutTos[i] = toMap[Tos[i]];
    }

    return Tags.size();
}
