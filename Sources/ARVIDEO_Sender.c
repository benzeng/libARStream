/**
 * @file ARVIDEO_Sender.c
 * @brief Video sender over network
 * @date 03/21/2013
 * @author nicolas.brulez@parrot.com
 */

#include <config.h>

/*
 * System Headers
 */

#include <stdlib.h>
#include <string.h>

#include <errno.h>

/*
 * Private Headers
 */

#include "ARVIDEO_Buffers.h"
#include "ARVIDEO_NetworkHeaders.h"

/*
 * ARSDK Headers
 */

#include <libARVideo/ARVIDEO_Sender.h>
#include <libARSAL/ARSAL_Mutex.h>
#include <libARSAL/ARSAL_Print.h>
#include <libARSAL/ARSAL_Endianness.h>

/*
 * Macros
 */

#define ARVIDEO_SENDER_TAG "ARVIDEO_Sender"
/**
 * Latency used when the network can't give us a valid value
 */
#define ARVIDEO_SENDER_DEFAULT_ESTIMATED_LATENCY_MS (100)

/*
 * Types
 */

typedef struct {
    uint32_t frameNumber;
    uint32_t frameSize;
    uint8_t *frameBuffer;
    int isHighPriority;
} ARVIDEO_Sender_Frame_t;

struct ARVIDEO_Sender_t {
    /* Configuration on New */
    ARNETWORK_Manager_t *manager;
    int dataBufferID;
    int ackBufferID;
    ARVIDEO_Sender_FrameUpdateCallback_t callback;
    uint32_t maxNumberOfNextFrames;

    /* Current frame storage */
    ARVIDEO_Sender_Frame_t currentFrame;
    int currentFrameNbFragments;
    int currentFrameCbWasCalled;
    ARSAL_Mutex_t packetsToSendMutex;
    ARVIDEO_NetworkHeaders_AckPacket_t packetsToSend;

    /* Acknowledge storage */
    ARSAL_Mutex_t ackMutex;
    ARVIDEO_NetworkHeaders_AckPacket_t ackPacket;

    /* Next frame storage */
    ARSAL_Mutex_t nextFrameMutex;
    ARSAL_Cond_t  nextFrameCond;
    uint32_t nextFrameNumber;
    uint32_t indexAddNextFrame;
    uint32_t indexGetNextFrame;
    uint32_t numberOfWaitingFrames;
    ARVIDEO_Sender_Frame_t *nextFrames;

    /* Thread status */
    int threadsShouldStop;
    int dataThreadStarted;
    int ackThreadStarted;
};

typedef struct {
    ARVIDEO_Sender_t *sender;
    uint32_t frameNumber;
    int fragmentIndex;
} ARVIDEO_Sender_NetworkCallbackParam_t;

/*
 * Internal functions declarations
 */

/**
 * @brief Flush the new frame queue
 * @param sender The sender to flush
 */
static void ARVIDEO_Sender_FlushQueue (ARVIDEO_Sender_t *sender);

/**
 * @brief Add a frame to the new frame queue
 * @param sender The sender which should send the frame
 * @param size The frame size, in bytes
 * @param buffer Pointer to the buffer which contains the frame
 * @param wasFlushFrame Boolean-like (0/1) flag, active if the frame is added after a flush (high priority frame)
 * @return the number of frames previously in queue (-1 if queue is full)
 */
static int ARVIDEO_Sender_AddToQueue (ARVIDEO_Sender_t *sender, uint32_t size, uint8_t *buffer, int wasFlushFrame);

/**
 * @brief Pop a frame from the new frame queue
 * @param sender The sender
 * @param newFrame Pointer in which the function will save the new frame infos
 * @param previousFrameFinished Boolean-like (0/1) flag, active if the previous frame was finished
 * @return 1 if a new frame is available
 * @return 0 if no new frame should be sent (queue is empty, or filled with lo-priority frame)
 */
static int ARVIDEO_Sender_PopFromQueue (ARVIDEO_Sender_t *sender, ARVIDEO_Sender_Frame_t *newFrame, int previousFrameFinished);

/**
 * @brief ARNETWORK_Manager_Callback_t for ARNETWORK_... calls
 * @param IoBufferId Unused as we always send on one unique buffer
 * @param dataPtr Unused as we don't need to free the memory
 * @param customData (ARVIDEO_Sender_NetworkCallbackParam_t *) Sender + fragment index
 * @param status Network information
 * @return ARNETWORK_MANAGER_CALLBACK_RETURN_DEFAULT
 *
 * @warning customData is a malloc'd pointer, and must be freed within this callback, during last call
 */
eARNETWORK_MANAGER_CALLBACK_RETURN ARVIDEO_Sender_NetworkCallback (int IoBufferId, uint8_t *dataPtr, void *customData, eARNETWORK_MANAGER_CALLBACK_STATUS status);

/*
 * Internal functions implementation
 */

static void ARVIDEO_Sender_FlushQueue (ARVIDEO_Sender_t *sender)
{
    ARSAL_Mutex_Lock (&(sender->nextFrameMutex));
    while (sender->numberOfWaitingFrames > 0)
    {
        ARVIDEO_Sender_Frame_t *nextFrame = &(sender->nextFrames [sender->indexGetNextFrame]);
        sender->callback (ARVIDEO_SENDER_STATUS_FRAME_CANCEL, nextFrame->frameBuffer, nextFrame->frameSize);
        sender->indexGetNextFrame++;
        sender->indexGetNextFrame %= sender->maxNumberOfNextFrames;
        sender->numberOfWaitingFrames--;
    }
    ARSAL_Mutex_Unlock (&(sender->nextFrameMutex));
}

static int ARVIDEO_Sender_AddToQueue (ARVIDEO_Sender_t *sender, uint32_t size, uint8_t *buffer, int wasFlushFrame)
{
    int retVal;
    ARSAL_Mutex_Lock (&(sender->nextFrameMutex));
    retVal = sender->numberOfWaitingFrames;
    if (sender->numberOfWaitingFrames < sender->maxNumberOfNextFrames)
    {
        ARVIDEO_Sender_Frame_t *nextFrame = &(sender->nextFrames [sender->indexAddNextFrame]);
        sender->nextFrameNumber++;
        nextFrame->frameNumber = sender->nextFrameNumber;
        nextFrame->frameBuffer = buffer;
        nextFrame->frameSize   = size;
        nextFrame->isHighPriority = wasFlushFrame;

        sender->indexAddNextFrame++;
        sender->indexAddNextFrame %= sender->maxNumberOfNextFrames;

        sender->numberOfWaitingFrames++;
        if (wasFlushFrame == 1)
        {
            // First frame in the buffer, send signal
            ARSAL_Cond_Signal (&(sender->nextFrameCond));
        }
    }
    else
    {
        retVal = -1;
    }
    ARSAL_Mutex_Unlock (&(sender->nextFrameMutex));
    return retVal;
}

static int ARVIDEO_Sender_PopFromQueue (ARVIDEO_Sender_t *sender, ARVIDEO_Sender_Frame_t *newFrame, int previousFrameFinished)
{
    int retVal = 0;
    ARSAL_Mutex_Lock (&(sender->nextFrameMutex));
    // Check if a frame is ready and of good priority
    if (sender->numberOfWaitingFrames > 0)
    {
        ARVIDEO_Sender_Frame_t *frame = &(sender->nextFrames [sender->indexGetNextFrame]);
        if ((frame->isHighPriority == 1) ||
            (previousFrameFinished == 1))
        {
            retVal = 1;
            sender->numberOfWaitingFrames--;
        }
    }
    // If not, wait for a frame ready event
    if (retVal == 0)
    {
        int latency = ARNETWORK_Manager_GetEstimatedLatency (sender->manager);
        if (latency < 0) // Unable to get latency
        {
            latency = ARVIDEO_SENDER_DEFAULT_ESTIMATED_LATENCY_MS;
        }
        latency++; // Add 1ms to avoid optimistic latency, and 0ms latency

        ARSAL_Cond_Timedwait (&(sender->nextFrameCond), &(sender->nextFrameMutex), latency);
        if (sender->numberOfWaitingFrames > 0)
        {
            // Accept new frame only if it's high priority, or if we have finished the current one
            ARVIDEO_Sender_Frame_t *frame = &(sender->nextFrames [sender->indexGetNextFrame]);
            if ((frame->isHighPriority == 1) ||
                (previousFrameFinished == 1))
            {
                retVal = 1;
                sender->numberOfWaitingFrames--;
            }
        }
    }
    // If we got a new frame, copy it
    if (retVal == 1)
    {
        ARVIDEO_Sender_Frame_t *frame = &(sender->nextFrames [sender->indexGetNextFrame]);
        sender->indexGetNextFrame++;
        sender->indexGetNextFrame %= sender->maxNumberOfNextFrames;

        newFrame->frameNumber = frame->frameNumber;
        newFrame->frameBuffer = frame->frameBuffer;
        newFrame->frameSize   = frame->frameSize;
    }
    ARSAL_Mutex_Unlock (&(sender->nextFrameMutex));
    return retVal;
}

eARNETWORK_MANAGER_CALLBACK_RETURN ARVIDEO_Sender_NetworkCallback (int IoBufferId, uint8_t *dataPtr, void *customData, eARNETWORK_MANAGER_CALLBACK_STATUS status)
{
    eARNETWORK_MANAGER_CALLBACK_RETURN retVal = ARNETWORK_MANAGER_CALLBACK_RETURN_DEFAULT;

    /* Get params */
    ARVIDEO_Sender_NetworkCallbackParam_t *cbParams = (ARVIDEO_Sender_NetworkCallbackParam_t *)customData;

    /* Get Sender */
    ARVIDEO_Sender_t *sender = cbParams->sender;

    /* Get packetIndex */
    int packetIndex = cbParams->fragmentIndex;

    /* Get frameNumber */
    uint32_t frameNumber = cbParams->frameNumber;

    /* Remove "unused parameter" warnings */
    IoBufferId = IoBufferId;
    dataPtr = dataPtr;

    switch (status)
    {
    case ARNETWORK_MANAGER_CALLBACK_STATUS_SENT:
        ARSAL_Mutex_Lock (&(sender->packetsToSendMutex));
        // Modify packetsToSend only if it refers to the frame we're sending
        if (frameNumber == sender->packetsToSend.numFrame)
        {
            ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARVIDEO_SENDER_TAG, "Sent packet %d", packetIndex);
            if (1 == ARVIDEO_NetworkHeaders_AckPacketUnsetFlag (&(sender->packetsToSend), packetIndex))
            {
                ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARVIDEO_SENDER_TAG, "All packets were sent");
            }
        }
        else
        {
            ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARVIDEO_SENDER_TAG, "Sent a packet for an old frame [packet %d, current frame %d]", frameNumber,sender->packetsToSend.numFrame);
        }
        ARSAL_Mutex_Unlock (&(sender->packetsToSendMutex));
        /* Free cbParams */
        free (cbParams);
        break;
    case ARNETWORK_MANAGER_CALLBACK_STATUS_CANCEL:
        /* Free cbParams */
        free (cbParams);
        break;
    default:
        break;
    }
    return retVal;
}

/*
 * Implementation
 */

void ARVIDEO_Sender_InitVideoDataBuffer (ARNETWORK_IOBufferParam_t *bufferParams, int bufferID)
{
    ARVIDEO_Buffers_InitVideoDataBuffer (bufferParams, bufferID);
}

void ARVIDEO_Sender_InitVideoAckBuffer (ARNETWORK_IOBufferParam_t *bufferParams, int bufferID)
{
    ARVIDEO_Buffers_InitVideoAckBuffer (bufferParams, bufferID);
}

ARVIDEO_Sender_t* ARVIDEO_Sender_New (ARNETWORK_Manager_t *manager, int dataBufferID, int ackBufferID, ARVIDEO_Sender_FrameUpdateCallback_t callback, uint32_t framesBufferSize)
{
    ARVIDEO_Sender_t *retSender = NULL;
    int stillValid = 1;
    int packetsToSendMutexWasInit = 0;
    int ackMutexWasInit = 0;
    int nextFrameMutexWasInit = 0;
    int nextFrameCondWasInit = 0;
    int nextFramesArrayWasCreated = 0;
    /* ARGS Check */
    if ((manager == NULL) ||
        (callback == NULL))
    {
        return retSender;
    }

    /* Alloc new sender */
    retSender = malloc (sizeof (ARVIDEO_Sender_t));
    if (retSender == NULL)
    {
        stillValid = 0;
    }

    /* Copy parameters */
    if (stillValid == 1)
    {
        retSender->manager = manager;
        retSender->dataBufferID = dataBufferID;
        retSender->ackBufferID = ackBufferID;
        retSender->callback = callback;
        retSender->maxNumberOfNextFrames = framesBufferSize;
    }

    /* Setup internal mutexes/sems */
    if (stillValid == 1)
    {
        int mutexInitRet = ARSAL_Mutex_Init (&(retSender->packetsToSendMutex));
        if (mutexInitRet != 0)
        {
            stillValid = 0;
        }
        else
        {
            packetsToSendMutexWasInit = 1;
        }
    }
    if (stillValid == 1)
    {
        int mutexInitRet = ARSAL_Mutex_Init (&(retSender->ackMutex));
        if (mutexInitRet != 0)
        {
            stillValid = 0;
        }
        else
        {
            ackMutexWasInit = 1;
        }
    }
    if (stillValid == 1)
    {
        int mutexInitRet = ARSAL_Mutex_Init (&(retSender->nextFrameMutex));
        if (mutexInitRet != 0)
        {
            stillValid = 0;
        }
        else
        {
            nextFrameMutexWasInit = 1;
        }
    }
    if (stillValid == 1)
    {
        int condInitRet = ARSAL_Cond_Init (&(retSender->nextFrameCond));
        if (condInitRet != 0)
        {
            stillValid = 0;
        }
        else
        {
            nextFrameCondWasInit = 1;
        }
    }

    /* Allocate next frame storage */
    if (stillValid == 1)
    {
        retSender->nextFrames = malloc (framesBufferSize * sizeof (ARVIDEO_Sender_Frame_t));
        if (retSender->nextFrames == NULL)
        {
            stillValid = 0;
        }
        else
        {
            nextFramesArrayWasCreated = 1;
        }
    }

    /* Setup internal variables */
    if (stillValid == 1)
    {
        retSender->currentFrame.frameNumber = 0;
        retSender->currentFrame.frameBuffer = NULL;
        retSender->currentFrame.frameSize   = 0;
        retSender->currentFrame.isHighPriority = 0;
        retSender->currentFrameNbFragments = 0;
        retSender->currentFrameCbWasCalled = 0;
        retSender->nextFrameNumber = 0;
        retSender->indexAddNextFrame = 0;
        retSender->indexGetNextFrame = 0;
        retSender->numberOfWaitingFrames = 0;
        retSender->threadsShouldStop = 0;
        retSender->dataThreadStarted = 0;
        retSender->ackThreadStarted = 0;
    }

    if ((stillValid == 0) &&
        (retSender != NULL))
    {
        if (packetsToSendMutexWasInit == 1)
        {
            ARSAL_Mutex_Destroy (&(retSender->packetsToSendMutex));
        }
        if (ackMutexWasInit == 1)
        {
            ARSAL_Mutex_Destroy (&(retSender->ackMutex));
        }
        if (nextFrameMutexWasInit == 1)
        {
            ARSAL_Mutex_Destroy (&(retSender->nextFrameMutex));
        }
        if (nextFrameCondWasInit == 1)
        {
            ARSAL_Cond_Destroy (&(retSender->nextFrameCond));
        }
        if (nextFramesArrayWasCreated == 1)
        {
            free (retSender->nextFrames);
        }
        free (retSender);
        retSender = NULL;
    }

    return retSender;
}

void ARVIDEO_Sender_StopSender (ARVIDEO_Sender_t *sender)
{
    if (sender != NULL)
    {
        sender->threadsShouldStop = 1;
    }
}

int ARVIDEO_Sender_Delete (ARVIDEO_Sender_t **sender)
{
    int retVal = -1;
    if ((sender != NULL) &&
        (*sender != NULL))
    {
        int canDelete = 0;
        if (((*sender)->dataThreadStarted == 0) &&
            ((*sender)->ackThreadStarted == 0))
        {
            canDelete = 1;
        }

        if (canDelete == 1)
        {
            ARSAL_Mutex_Destroy (&((*sender)->packetsToSendMutex));
            ARSAL_Mutex_Destroy (&((*sender)->ackMutex));
            ARSAL_Mutex_Destroy (&((*sender)->nextFrameMutex));
            ARSAL_Cond_Destroy (&((*sender)->nextFrameCond));
            free ((*sender)->nextFrames);
            free (*sender);
            *sender = NULL;
        }
        else
        {
            ARSAL_PRINT (ARSAL_PRINT_ERROR, ARVIDEO_SENDER_TAG, "Call ARVIDEO_Sender_StopSender before calling this function");
        }
        retVal = canDelete;
    }
    return retVal;
}

int ARVIDEO_Sender_SendNewFrame (ARVIDEO_Sender_t *sender, uint8_t *frameBuffer, uint32_t frameSize, int flushPreviousFrames)
{
    int res = -1;
    // Args check
    if ((sender == NULL) ||
        (frameBuffer == NULL) ||
        ((flushPreviousFrames != 0) &&
         (flushPreviousFrames != 1)))
    {
        return res;
    }
    if (flushPreviousFrames == 1)
    {
        ARVIDEO_Sender_FlushQueue (sender);
    }
    res = ARVIDEO_Sender_AddToQueue (sender, frameSize, frameBuffer, flushPreviousFrames);
    return res;
}

void* ARVIDEO_Sender_RunDataThread (void *ARVIDEO_Sender_t_Param)
{
    /* Local declarations */
    ARVIDEO_Sender_t *sender = (ARVIDEO_Sender_t *)ARVIDEO_Sender_t_Param;
    uint8_t *sendFragment = NULL;
    uint32_t sendSize = 0;
    uint16_t nbPackets = 0;
    int cnt;
    int numbersOfFragmentsSentForCurrentFrame = 0;
    int lastFragmentSize = 0;
    ARVIDEO_NetworkHeaders_DataHeader_t *header = NULL;
    ARVIDEO_Sender_Frame_t nextFrame = {0};

    /* Parameters check */
    if (sender == NULL)
    {
        ARSAL_PRINT (ARSAL_PRINT_ERROR, ARVIDEO_SENDER_TAG, "Error while starting %s, bad parameters", __FUNCTION__);
        return (void *)0;
    }

    /* Alloc and check */
    sendFragment = malloc (ARVIDEO_NETWORK_HEADERS_FRAGMENT_SIZE + sizeof (ARVIDEO_NetworkHeaders_DataHeader_t));
    if (sendFragment == NULL)
    {
        ARSAL_PRINT (ARSAL_PRINT_ERROR, ARVIDEO_SENDER_TAG, "Error while starting %s, can not alloc memory", __FUNCTION__);
        return (void *)0;
    }
    header = (ARVIDEO_NetworkHeaders_DataHeader_t *)sendFragment;

    ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARVIDEO_SENDER_TAG, "Sender thread running");
    sender->dataThreadStarted = 1;

    while (sender->threadsShouldStop == 0)
    {
        int waitRes;
        waitRes = ARVIDEO_Sender_PopFromQueue (sender, &nextFrame, sender->currentFrameCbWasCalled);
        ARSAL_Mutex_Lock (&(sender->ackMutex));
        if (waitRes == 1)
        {
            ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARVIDEO_SENDER_TAG, "Previous frame was sent in %d packets. Frame size was %d packets", numbersOfFragmentsSentForCurrentFrame, nbPackets);
            numbersOfFragmentsSentForCurrentFrame = 0;
            /* We have a new frame to send */
            ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARVIDEO_SENDER_TAG, "New frame needs to be sent");

            /* Cancel current frame if it was not already sent */
            if (sender->currentFrameCbWasCalled == 0)
            {
#ifdef DEBUG
                ARVIDEO_NetworkHeaders_AckPacketDump ("Cancel frame:", &(sender->ackPacket));
#endif

                ARNETWORK_Manager_FlushInputBuffer (sender->manager, sender->dataBufferID);

                sender->callback (ARVIDEO_SENDER_STATUS_FRAME_CANCEL, sender->currentFrame.frameBuffer, sender->currentFrame.frameSize);
            }
            sender->currentFrameCbWasCalled = 0; // New frame

            /* Save next frame data into current frame data */
            sender->currentFrame.frameNumber = nextFrame.frameNumber;
            sender->currentFrame.frameBuffer = nextFrame.frameBuffer;
            sender->currentFrame.frameSize   = nextFrame.frameSize;
            sendSize = nextFrame.frameSize;

            /* Reset ack packet - No packets are ack on the new frame */
            sender->ackPacket.numFrame = sender->currentFrame.frameNumber;
            ARVIDEO_NetworkHeaders_AckPacketReset (&(sender->ackPacket));

            /* Reset packetsToSend - update frame number */
            ARSAL_Mutex_Lock (&(sender->packetsToSendMutex));
            sender->packetsToSend.numFrame = sender->currentFrame.frameNumber;
            ARVIDEO_NetworkHeaders_AckPacketReset (&(sender->packetsToSend));
            ARSAL_Mutex_Unlock (&(sender->packetsToSendMutex));

            /* Update video data header with the new frame number */
            header->frameNumber = sender->currentFrame.frameNumber;

            /* Compute number of fragments / size of the last fragment */
            if (0 < sendSize)
            {
                lastFragmentSize = ARVIDEO_NETWORK_HEADERS_FRAGMENT_SIZE;
                nbPackets = sendSize / ARVIDEO_NETWORK_HEADERS_FRAGMENT_SIZE;
                if (sendSize % ARVIDEO_NETWORK_HEADERS_FRAGMENT_SIZE)
                {
                    nbPackets++;
                    lastFragmentSize = sendSize % ARVIDEO_NETWORK_HEADERS_FRAGMENT_SIZE;
                }
            }

            ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARVIDEO_SENDER_TAG, "New frame has size %d (=%d packets)", sendSize, nbPackets);
        }
        ARSAL_Mutex_Unlock (&(sender->ackMutex));
        /* END OF NEW FRAME BLOCK */

        /* Flag all non-ack packets as "packet to send" */
        ARSAL_Mutex_Lock (&(sender->packetsToSendMutex));
        ARSAL_Mutex_Lock (&(sender->ackMutex));
        for (cnt = 0; cnt < nbPackets; cnt++)
        {
            if (0 == ARVIDEO_NetworkHeaders_AckPacketFlagIsSet (&(sender->ackPacket), cnt))
            {
                ARVIDEO_NetworkHeaders_AckPacketSetFlag (&(sender->packetsToSend), cnt);
            }
        }
        ARSAL_Mutex_Unlock (&(sender->ackMutex));

        /* Send all "packets to send" */
        for (cnt = 0; cnt < nbPackets; cnt++)
        {
            if (ARVIDEO_NetworkHeaders_AckPacketFlagIsSet (&(sender->packetsToSend), cnt))
            {
                numbersOfFragmentsSentForCurrentFrame ++;
                int currFragmentSize = (cnt == nbPackets-1) ? lastFragmentSize : ARVIDEO_NETWORK_HEADERS_FRAGMENT_SIZE;
                header->fragmentNumber = cnt;
                header->fragmentsPerFrame = nbPackets;
                memcpy (&sendFragment[sizeof (ARVIDEO_NetworkHeaders_DataHeader_t)], &(sender->currentFrame.frameBuffer)[ARVIDEO_NETWORK_HEADERS_FRAGMENT_SIZE*cnt], currFragmentSize);
                ARVIDEO_Sender_NetworkCallbackParam_t *cbParams = malloc (sizeof (ARVIDEO_Sender_NetworkCallbackParam_t));
                cbParams->sender = sender;
                cbParams->fragmentIndex = cnt;
                cbParams->frameNumber = sender->packetsToSend.numFrame;
                ARSAL_Mutex_Unlock (&(sender->packetsToSendMutex));
                ARNETWORK_Manager_SendData (sender->manager, sender->dataBufferID, sendFragment, currFragmentSize + sizeof (ARVIDEO_NetworkHeaders_DataHeader_t), (void *)cbParams, ARVIDEO_Sender_NetworkCallback, 1);
                ARSAL_Mutex_Lock (&(sender->packetsToSendMutex));
            }
        }
        ARSAL_Mutex_Unlock (&(sender->packetsToSendMutex));
    }
    /* END OF PROCESS LOOP */

    ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARVIDEO_SENDER_TAG, "Sender thread ended");
    sender->dataThreadStarted = 0;

    return (void *)0;
}


void* ARVIDEO_Sender_RunAckThread (void *ARVIDEO_Sender_t_Param)
{
    ARVIDEO_NetworkHeaders_AckPacket_t recvPacket;
    int recvSize;
    ARVIDEO_Sender_t *sender = (ARVIDEO_Sender_t *)ARVIDEO_Sender_t_Param;

    ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARVIDEO_SENDER_TAG, "Ack thread running");
    sender->ackThreadStarted = 1;

    ARVIDEO_NetworkHeaders_AckPacketReset (&recvPacket);

    while (sender->threadsShouldStop == 0)
    {
        eARNETWORK_ERROR err = ARNETWORK_Manager_ReadDataWithTimeout (sender->manager, sender->ackBufferID, (uint8_t *)&recvPacket, sizeof (recvPacket), &recvSize, 1000);
        if (ARNETWORK_OK != err)
        {
            ARSAL_PRINT (ARSAL_PRINT_ERROR, ARVIDEO_SENDER_TAG, "Error while reading ACK data: %s", ARNETWORK_Error_ToString (err));
        }
        else if (recvSize != sizeof (recvPacket))
        {
            ARSAL_PRINT (ARSAL_PRINT_ERROR, ARVIDEO_SENDER_TAG, "Read %d octets, expected %d", recvSize, sizeof (recvPacket));
        }
        else
        {
            /* Switch recvPacket endianness */
            recvPacket.numFrame = dtohl (recvPacket.numFrame);
            recvPacket.highPacketsAck = dtohll (recvPacket.highPacketsAck);
            recvPacket.lowPacketsAck = dtohll (recvPacket.lowPacketsAck);

            /* Apply recvPacket to sender->ackPacket if frame numbers are the same */
            ARSAL_Mutex_Lock (&(sender->ackMutex));
            if (sender->ackPacket.numFrame == recvPacket.numFrame)
            {
                ARVIDEO_NetworkHeaders_AckPacketSetFlags (&(sender->ackPacket), &recvPacket);
                if ((sender->currentFrameCbWasCalled == 0) &&
                    (ARVIDEO_NetworkHeaders_AckPacketAllFlagsSet (&(sender->ackPacket), sender->currentFrameNbFragments) == 1))
                {
                    sender->callback (ARVIDEO_SENDER_STATUS_FRAME_SENT, sender->currentFrame.frameBuffer, sender->currentFrame.frameSize);
                    sender->currentFrameCbWasCalled = 1;
                    ARSAL_Mutex_Lock (&(sender->nextFrameMutex));
                    ARSAL_Cond_Signal (&(sender->nextFrameCond));
                    ARSAL_Mutex_Unlock (&(sender->nextFrameMutex));
                }
            }
            ARSAL_Mutex_Unlock (&(sender->ackMutex));
        }
    }

    ARSAL_PRINT (ARSAL_PRINT_DEBUG, ARVIDEO_SENDER_TAG, "Ack thread ended");
    sender->ackThreadStarted = 0;
    return (void *)0;
}
