#ifndef EBERLE_PS5_PARALLEL_TOPLEVEL_MERGE_H_
#define EBERLE_PS5_PARALLEL_TOPLEVEL_MERGE_H_

#include <iostream>
#include <vector>

#include "../utils/types.h"
#include "../utils/utility-functions.h"
#include "../utils/lcp-string-losertree.h"
#include "../sequential/eberle-lcp-mergesort.h"

#include "../../tools/jobqueue.h"
#include "../../tools/stringtools.h"

#include "../../parallel/bingmann-parallel_sample_sort.h"

//#define PARALLEL_LCP_MERGE_DEBUG_JOB_TYPE_ON_CREATION
//#define PARALLEL_LCP_MERGE_DEBUG_MERGE_JOBS_DETAILED
#define PARALLEL_LCP_MERGE_DEBUG_JOB_CREATION
#define PARALLEL_LCP_MERGE_DEBUG_TOP_LEVEL_MERGE_DURATION

namespace eberle_parallel_mergesort_lcp_loosertree
{

using namespace std;

using namespace types;
using namespace eberle_lcp_utils;
using namespace eberle_utils;
using namespace eberle_mergesort_lcp;

using namespace jobqueue;
using namespace stringtools;

using namespace bingmann_parallel_sample_sort_lcp;

//typedefs
typedef unsigned char* string;
typedef unsigned int UINT;

//constants
static const bool USE_WORK_SHARING = true;
static const size_t MERGE_BULK_SIZE = 3000;
static const size_t SHARE_WORK_THRESHOLD = 3 * MERGE_BULK_SIZE;

//method definitions

static inline void
createJobs(JobQueue &jobQueue, AS* input, string* output, pair<size_t, size_t>* ranges, unsigned numStreams, size_t numberOfElements,
        unsigned baseLcp);

//definitions

typedef uint64_t CHAR_TYPE;
string * outputBase;

//structs defining the jobs

struct CopyDataJob : public Job
{
    AS* input;
    string* output;
    size_t length;

    CopyDataJob(AS* input, string* output, size_t length) :
            input(input), output(output), length(length)
    {
#ifdef PARALLEL_LCP_MERGE_DEBUG_JOB_TYPE_ON_CREATION
#pragma omp critical (OUTPUT)
        {
            cout << "CopyDataJob (output: " << (output - outputBase) << ", length: " << length << ")" << endl;
        }
#endif // PARALLEL_LCP_MERGE_DEBUG_JOB_TYPE_ON_CREATION
    }

    virtual bool
    run(JobQueue& jobQueue)
    {
        (void) jobQueue;
        //memcpy(output, input, length * sizeof(AS));

        for (string* end = output + length; output < end; output++, input++)
        {
            *output = input->text;
        }

        return true;
    }
};

struct BinaryMergeJob : public Job
{
    AS* input1;
    size_t length1;
    AS* input2;
    size_t length2;
    string* output;

    BinaryMergeJob(AS* input1, size_t length1, AS* input2, size_t length2, string* output) :
            input1(input1), length1(length1), input2(input2), length2(length2), output(output)
    {
#ifdef PARALLEL_LCP_MERGE_DEBUG_JOB_TYPE_ON_CREATION
#pragma omp critical (OUTPUT)
        {
            cout << "BinaryMergeJob (output: " << (output - outputBase) << ", length1: " << length1 << ", length2: " << length2 << ")" << endl;
        }
#endif // PARALLEL_LCP_MERGE_DEBUG_JOB_TYPE_ON_CREATION
    }

    virtual bool
    run(JobQueue& jobQueue)
    {
        (void) jobQueue;
        input1->lcp = 0;
        input2->lcp = 0;
        eberle_lcp_merge(input1, length1, input2, length2, output);

        return true;
    }
};
size_t lengthOfLongestJob(0);

template<unsigned K>
    struct MergeJob : public Job
    {
        AS* input;
        string* output;
        pair<size_t, size_t>* ranges;
        size_t length;
        unsigned baseLcp;
        unsigned nextBaseLcp;

        MergeJob(AS* input, string* output, pair<size_t, size_t>* ranges, size_t length, unsigned baseLcp, unsigned nextBaseLcp) :
                input(input), output(output), ranges(ranges), length(length), baseLcp(baseLcp), nextBaseLcp(nextBaseLcp)
        {
#ifdef PARALLEL_LCP_MERGE_DEBUG_JOB_TYPE_ON_CREATION
#pragma omp critical (OUTPUT)
            {
                cout << "MergeJob<" << K << "> (output: " << (output - outputBase) << ", baseLcp: " << baseLcp << ", nextBaseLcp: " << nextBaseLcp
                        << ", length: " << length << ")" << endl;
#ifdef PARALLEL_LCP_MERGE_DEBUG_MERGE_JOBS_DETAILED
                for (unsigned k = 0; k < K; ++k)
                {
                    cout << k << ": " << ranges[k].first << " length: " << ranges[k].second << endl;
                }
                cout << endl;
#endif // PARALLEL_LCP_MERGE_DEBUG_MERGE_JOBS_DETAILED
            }
#endif // PARALLEL_LCP_MERGE_DEBUG_JOB_TYPE_ON_CREATION
        }

        /*
         * returns true if all elements have been written to output
         * false if the merge has been stopped to free work.
         */
        inline bool
        mergeToOutput(JobQueue& jobQueue, LcpStringLoserTree<K> * loserTree)
        {
            for (size_t lastLength = length; length >= MERGE_BULK_SIZE; length -= MERGE_BULK_SIZE, output += MERGE_BULK_SIZE)
            {
                if (lengthOfLongestJob == lastLength)
                    lengthOfLongestJob = length;

                if (lengthOfLongestJob < length)
                    lengthOfLongestJob = length; // else if to prevent work sharing when we just increased lengthOfLongestJob
                else if (USE_WORK_SHARING && jobQueue.has_idle() && length > SHARE_WORK_THRESHOLD && lengthOfLongestJob == length)
                    return false;

                loserTree->writeElementsToStream(output, MERGE_BULK_SIZE);
                lastLength = length;
            }

            loserTree->writeElementsToStream(output, length);

            return true;
        }

        virtual bool
        run(JobQueue& jobQueue)
        {
            for (unsigned k = 0; k < K; k++)
            { // this ensures that the streams are all equally compared at the start
                input[ranges[k].first].lcp = baseLcp;
            }

            //merge
            LcpStringLoserTree<K> *loserTree = new LcpStringLoserTree<K>(input, ranges);

            if (!mergeToOutput(jobQueue, loserTree))
            {
                // share work
                pair < size_t, size_t > newRanges[K];
                loserTree->getRangesOfRemaining(newRanges, input);

                createJobs(jobQueue, input, output, newRanges, K, length, nextBaseLcp);

                if (lengthOfLongestJob == length)
                    lengthOfLongestJob = 0;
            }

            delete loserTree;

            return true;
        }

        ~MergeJob()
        {
            delete ranges;
        }
    };

struct InitialSplitJob : public Job
{
    AS* input;
    string* output;
    pair<size_t, size_t>* ranges;
    size_t length;
    unsigned numStreams;

    InitialSplitJob(AS* input, string* output, pair<size_t, size_t>* ranges, size_t length, unsigned numStreams) :
            input(input), output(output), ranges(ranges), length(length), numStreams(numStreams)
    {
        lengthOfLongestJob = length; // prevents that the first MergeJob immediately starts splitting itself
        outputBase = output;
    }

    virtual bool
    run(JobQueue& jobQueue)
    {
        createJobs(jobQueue, input, output, ranges, numStreams, length, unsigned(0));
        lengthOfLongestJob = 0;
        return true;
    }

    ~InitialSplitJob()
    {
        delete ranges;
    }
};

// Implementations follow.

static inline void
enqueueMergeJob(JobQueue& jobQueue, AS* input, string* output, pair<size_t, size_t>* ranges, size_t length, unsigned numStreams, unsigned baseLcp,
        unsigned nextBaseLcp)
{
    switch (numStreams)
    {
    case 2:
        jobQueue.enqueue(new BinaryMergeJob(input + ranges[0].first, ranges[0].second, input + ranges[1].first, ranges[1].second, output));
        break;
    case 4:
        jobQueue.enqueue(new MergeJob<4>(input, output, ranges, length, baseLcp, nextBaseLcp));
        break;
    case 8:
        jobQueue.enqueue(new MergeJob<8>(input, output, ranges, length, baseLcp, nextBaseLcp));
        break;
    case 16:
        jobQueue.enqueue(new MergeJob<16>(input, output, ranges, length, baseLcp, nextBaseLcp));
        break;
    case 32:
        jobQueue.enqueue(new MergeJob<32>(input, output, ranges, length, baseLcp, nextBaseLcp));
        break;
    case 64:
        jobQueue.enqueue(new MergeJob<64>(input, output, ranges, length, baseLcp, nextBaseLcp));
        break;
    default:
        cerr << "CANNOT MERGE! TO MANY STREAMS: " << numStreams;
        break;
    }
}

static inline unsigned
findNextSplitter(AS* &inputStream, AS* end, unsigned baseLcp, unsigned maxAllowedLcp, CHAR_TYPE &lastCharacter, CHAR_TYPE keyMask)
{
    AS* streamStart = inputStream;
    inputStream++;

    for (; inputStream < end; ++inputStream)
    {
        unsigned lcp = inputStream->lcp;

        if (lcp <= maxAllowedLcp)
        {
            CHAR_TYPE character = get_char<CHAR_TYPE>(inputStream->text, baseLcp);
            if ((character & keyMask) != (lastCharacter & keyMask))
            {
                lastCharacter = character;
                return inputStream - streamStart;
            }
        }
    }

    lastCharacter = numeric_limits < CHAR_TYPE > ::max();
    return inputStream - streamStart;
}

static inline void
createJobs(JobQueue &jobQueue, AS* input, string* output, pair<size_t, size_t>* ranges, unsigned numStreams, size_t numberOfElements,
        unsigned baseLcp)
{
#ifdef PARALLEL_LCP_MERGE_DEBUG_JOB_CREATION
    cout << endl << "CREATING JOBS at baseLcp: " << baseLcp << ", numberOfElements: " << numberOfElements << endl;
#else
    (void) numberOfElements;
#endif // PARALLEL_LCP_MERGE_DEBUG_JOB_CREATION

    AS* inputStreams[numStreams];
    AS* ends[numStreams];
    CHAR_TYPE splitterCharacter[numStreams];

    for (unsigned k = 0; k < numStreams; ++k)
    {
        if (ranges[k].second > 0)
        {
            inputStreams[k] = input + ranges[k].first;
            ends[k] = inputStreams[k] + ranges[k].second;
            splitterCharacter[k] = get_char<CHAR_TYPE>(inputStreams[k][0].text, baseLcp);
        }
        else
        {
            splitterCharacter[k] = numeric_limits < CHAR_TYPE > ::max();
        }
    }

    unsigned indexesOfFound[numStreams];
    unsigned createdJobsCtr = 0;
    unsigned keyWidth = 8;

    unsigned toShortCtr = 0;
    unsigned notToShortCtr = 0;

    while (true)
    {
        unsigned maxAllowedLcp(baseLcp + keyWidth - 1);
        CHAR_TYPE keyMask = numeric_limits < CHAR_TYPE > ::max() << ((key_traits<CHAR_TYPE>::add_depth - keyWidth) * 8);

        CHAR_TYPE currBucket = numeric_limits < CHAR_TYPE > ::max();
        unsigned numberOfFoundBuckets = 0;

        for (unsigned k = 0; k < numStreams; ++k)
        {
            CHAR_TYPE splitter = splitterCharacter[k] & keyMask;
            if (splitter <= currBucket)
            {
                if (splitter < currBucket)
                {
                    currBucket = splitter;

                    indexesOfFound[0] = k;
                    numberOfFoundBuckets = 1;
                }
                else
                {
                    indexesOfFound[numberOfFoundBuckets] = k;
                    numberOfFoundBuckets++;
                }
            }
        }

        if (currBucket == (numeric_limits < CHAR_TYPE > ::max() & keyMask))
        {
            break;
        }

        size_t length = 0;

        switch (numberOfFoundBuckets)
        {
        case 1:
        {
            unsigned streamIdx = indexesOfFound[0];
            AS* inputStart = inputStreams[streamIdx];
            length += findNextSplitter(inputStreams[streamIdx], ends[streamIdx], baseLcp, maxAllowedLcp, splitterCharacter[streamIdx], keyMask);
            jobQueue.enqueue(new CopyDataJob(inputStart, output, length));
            break;
        }
        case 2:
        {
            unsigned streamIdx1 = indexesOfFound[0];
            AS* inputStart1 = inputStreams[streamIdx1];
            size_t length1 = findNextSplitter(inputStreams[streamIdx1], ends[streamIdx1], baseLcp, maxAllowedLcp, splitterCharacter[streamIdx1],
                    keyMask);

            unsigned streamIdx2 = indexesOfFound[1];
            AS* inputStart2 = inputStreams[streamIdx2];
            size_t length2 = findNextSplitter(inputStreams[streamIdx2], ends[streamIdx2], baseLcp, maxAllowedLcp, splitterCharacter[streamIdx2],
                    keyMask);

            jobQueue.enqueue(new BinaryMergeJob(inputStart1, length1, inputStart2, length2, output));
            length = length1 + length2;
            break;
        }

        default:
        {
            unsigned numNewStreams = getNextHigherPowerOfTwo(numberOfFoundBuckets);
            pair < size_t, size_t > *newRange = new pair<size_t, size_t> [numNewStreams];

            unsigned k = 0;
            for (; k < numberOfFoundBuckets; ++k)
            {
                unsigned idx = indexesOfFound[k];
                AS* inputStart = inputStreams[idx];
                size_t currLength = findNextSplitter(inputStreams[idx], ends[idx], baseLcp, maxAllowedLcp, splitterCharacter[idx], keyMask);
                newRange[k] = make_pair(inputStart - input, currLength);
                length += currLength;
            }
            for (; k < numNewStreams; k++)
            {
                newRange[k] = make_pair(0, 0); // this stream is not used
            }

            enqueueMergeJob(jobQueue, input, output, newRange, length, numNewStreams, baseLcp, maxAllowedLcp + 1);
            break;
        }
        }

        output += length;
        createdJobsCtr++;

        if (keyWidth > 1)
        {
            if (length < MERGE_BULK_SIZE)
                toShortCtr++;
            else
                notToShortCtr++;

            if (toShortCtr + notToShortCtr > 30)
            {
                if ((float(toShortCtr) / float(toShortCtr + notToShortCtr)) > float(0.5))
                {
                    keyWidth = max(unsigned(1), keyWidth - 1);
                    cout << "decreased keyWidth to " << keyWidth << endl;
                    toShortCtr = 0;
                    notToShortCtr = 0;
                }
            }
        }
    }

#ifdef PARALLEL_LCP_MERGE_DEBUG_JOB_CREATION
    cout << "Created " << createdJobsCtr << " Jobs!" << endl;
#endif // PARALLEL_LCP_MERGE_DEBUG_JOB_CREATION
}

static inline
void
parallelMerge(AS* input, string* output, pair<size_t, size_t>* ranges, size_t length, unsigned numStreams)
{
    JobQueue jobQueue;
    cout << "doing parallel merge for " << numStreams << " streams" << endl;
    jobQueue.enqueue(new InitialSplitJob(input, output, ranges, length, numStreams));
    jobQueue.loop();
}

void
eberle_parallel_mergesort_lcp_loosertree(string *strings, size_t n)
{
    int realNumaNodes = numa_num_configured_nodes();
    unsigned numNumaNodes = max(unsigned(4), unsigned(realNumaNodes)); // this max ensures a parallel merge on developer machine
    int numThreadsPerPart = numa_num_configured_cpus() / numNumaNodes;

//allocate memory for annotated strings
    AS *tmp = static_cast<AS *>(malloc(n * sizeof(AS)));
    string* shadow = new string[n]; // allocate shadow pointer array

    pair < size_t, size_t > *ranges = new pair<size_t, size_t> [numNumaNodes];
    calculateRanges(ranges, numNumaNodes, n);

// enable nested parallel regions
    omp_set_nested(true);

#pragma omp parallel for
    for (unsigned k = 0; k < numNumaNodes; k++)
    {
        size_t start = ranges[k].first;
        size_t length = ranges[k].second;

        StringPtr strptr(strings + start, shadow + start, length);
        parallel_sample_sort_numa(strptr, k % realNumaNodes, numThreadsPerPart);

        //create AS* array
        MeasureTime < 0 > timer;
        timer.start();

        for (size_t pos = 0; pos < length; pos++)
        {
            tmp[start + pos].text = strptr.str(pos);
            tmp[start + pos].lcp = strptr.lcp(pos);
        }

        timer.stop();
        cout << endl << "Creating AS* needed: " << timer.delta() << " s" << endl << endl;
    }

    delete[] shadow;

#ifdef PARALLEL_LCP_MERGE_DEBUG_TOP_LEVEL_MERGE_DURATION
    MeasureTime < 0 > timer;
    timer.start();
    parallelMerge(tmp, strings, ranges, n, numNumaNodes);
    timer.stop();
    cout << endl << "top level merge needed: " << timer.delta() << " s" << endl << endl;
#else
    parallelMerge(tmp, output, ranges, n, numNumaNodes);
#endif

    free(tmp);
}

CONTESTANT_REGISTER_PARALLEL(eberle_parallel_mergesort_lcp_loosertree, "eberle/ps5-parallel-toplevel-merge",
        "NUMA aware sorting algorithm running pS5 on local memory and then doing a parallel merge by Andreas Eberle")

}
// namespace eberle_parallel_mergesort_lcp_loosertree

#endif // EBERLE_PS5_PARALLEL_TOPLEVEL_MERGE_H_

