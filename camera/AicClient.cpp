/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Contains implementation of classes that encapsulate connection to camera
 * services in the emulator via local_camera srv socket.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "EmulatedCamera_AicClient"
#include <cutils/log.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "EmulatedCamera.h"
#include "AicClient.h"

#define LOG_QUERIES 0
#if LOG_QUERIES
#define LOGQ(...)   ALOGD(__VA_ARGS__)
#else
#define LOGQ(...)   (void(0))

#endif  // LOG_QUERIES
namespace android {

/****************************************************************************
 * Aic client base
 ***************************************************************************/

AicClient::AicClient()
    : mSocketFD(-1)
{
}

AicClient::~AicClient()
{
    if (mSocketFD >= 0) {
        close(mSocketFD);
    }
}

/****************************************************************************
 * Aic client API
 ***************************************************************************/

status_t AicClient::connectClient(const int local_srv_port)
{
    struct sockaddr_in so_addr;
    ALOGV("%s: port %d", __FUNCTION__, local_srv_port);

    /* Make sure that client is not connected already. */
    if (mSocketFD >= 0) {
        ALOGE("%s: Aic client is already connected", __FUNCTION__);
        return EINVAL;
    }

    /* Connect to the local_camera server */
    mSocketFD = socket(AF_INET, SOCK_STREAM, 0);
    if (mSocketFD < 0) {
        ALOGE("%s: Unable to create socket to the camera service port %d: %s",
             __FUNCTION__, local_srv_port, strerror(errno));
        return errno ? errno : EINVAL;
    }

    bzero(&so_addr, sizeof(so_addr));
    so_addr.sin_family = AF_INET;
    so_addr.sin_port = htons(local_srv_port);
    so_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(mSocketFD, (struct sockaddr *)&so_addr, sizeof(so_addr)) < 0) {
        ALOGE("%s: Unable to connect to the camera service port %d: %s",
             __FUNCTION__, local_srv_port, strerror(errno));
        return errno ? errno : EINVAL;
    }

    /* Add timeout to read and write operations */
    struct timeval tv;
    tv.tv_sec = 10;  /* 10 Secs Timeout */
    tv.tv_usec = 0;
    setsockopt(mSocketFD, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv,
               sizeof(struct timeval));

    /* enable TCP NO DELAY too */
    int    flag;
    flag = 1;
    setsockopt(mSocketFD, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag,
               sizeof(flag));

    return NO_ERROR;
}

void AicClient::disconnectClient()
{
    ALOGV("%s", __FUNCTION__);

    if (mSocketFD >= 0) {
        close(mSocketFD);
        mSocketFD = -1;
    }
}

status_t AicClient::sendMessage(const void* data, size_t data_size)
{
    if (mSocketFD < 0) {
        ALOGE("%s: Aic client is not connected", __FUNCTION__);
        return EINVAL;
    }

    LOGQ("Sending '%.*s' on fd:%d", data_size, data, mSocketFD);

    int wr_res = send(mSocketFD, data, data_size, MSG_NOSIGNAL);
    if (wr_res != data_size) {
        ALOGE("%s: Unable to write message %d (size=%d): %s",
              __FUNCTION__, wr_res, data_size, strerror(errno));
        return errno ? errno : EIO;
    }
    return NO_ERROR;
}

status_t AicClient::receiveMessage(void** data, size_t* data_size)
{
    *data = NULL;
    *data_size = 0;

    if (mSocketFD < 0) {
        ALOGE("%s: Aic client is not connected", __FUNCTION__);
        return EINVAL;
    }

    /* The way the service replies to a query, it sends payload size first, and
     * then it sends the payload itself. Note that payload size is sent as a
     * string, containing 8 characters representing a hexadecimal payload size
     * value. Note also, that the string doesn't contain zero-terminator. */
    size_t payload_size;
    char payload_size_str[9];

    int rd_res = read(mSocketFD, payload_size_str, 8);
    if (rd_res != 8) {
        ALOGE("%s: Unable to obtain payload size: %s",
             __FUNCTION__, strerror(errno));
        return errno ? errno : EIO;
    }

    /* Convert payload size. */
    errno = 0;
    payload_size_str[8] = '\0';
    payload_size = strtol(payload_size_str, NULL, 16);
    if (errno) {
        ALOGE("%s: Invalid payload size '%s'", __FUNCTION__, payload_size_str);
        return EIO;
    }

    /* Allocate payload data buffer, and read the payload there. */
    *data = malloc(payload_size);
    if (*data == NULL) {
        ALOGE("%s: Unable to allocate %d bytes payload buffer",
             __FUNCTION__, payload_size);
        return ENOMEM;
    }

    int payload_read = 0;
    int payload_left = payload_size;
    char *ptr = (char *)*data;
    while ((rd_res = read(mSocketFD, ptr, payload_left)) > 0) {
        payload_left -= rd_res;
        payload_read += rd_res;
        ptr += rd_res;

        if (static_cast<size_t>(payload_read) == payload_size) {
            *data_size = payload_size;
            return NO_ERROR;
        }
    }

    if (static_cast<size_t>(payload_read) != payload_size) {
        ALOGE("%s: Read size %d doesnt match expected payload size %d: %s",
             __FUNCTION__, payload_read, payload_size, strerror(errno));
        free(*data);
        *data = NULL;
        return errno ? errno : EIO;
    }
    return NO_ERROR;
}

status_t AicClient::doQuery(QemuQuery* query)
{
    LOGQ("%s", __FUNCTION__);

    /* Make sure that query has been successfuly constructed. */
    if (query->mQueryDeliveryStatus != NO_ERROR) {
        ALOGE("%s: Query is invalid", __FUNCTION__);
        return query->mQueryDeliveryStatus;
    }

    LOGQ("Send query '%s'", query->mQuery);

    /* Send the query. */
    status_t res = sendMessage(query->mQuery, strlen(query->mQuery) + 1);
    if (res == NO_ERROR) {
        /* Read the response. */
        res = receiveMessage(reinterpret_cast<void**>(&query->mReplyBuffer),
                      &query->mReplySize);
        if (res == NO_ERROR) {
            LOGQ("Response to query '%s': Status = '%*.s', %d bytes in response",
                 query->mQuery, query->mReplySize, query->mReplyBuffer,
                 query->mReplySize);
        } else {
            ALOGE("%s Response to query '%s' has failed: %s",
                 __FUNCTION__, query->mQuery, strerror(res));
        }
    } else {
        ALOGE("%s: Send query '%s' failed: %s",
             __FUNCTION__, query->mQuery, strerror(res));
    }

    /* Complete the query, and return its completion handling status. */
    const status_t res1 = query->completeQuery(res);
    ALOGE_IF(res1 != NO_ERROR && res1 != res,
            "%s: Error %d in query '%s' completion",
            __FUNCTION__, res1, query->mQuery);
    return res1;
}


/****************************************************************************
 * Aic client for an 'emulated camera' service.
 ***************************************************************************/

/*
 * Emulated camera queries
 */

/* Connect to the camera device. */
const char CameraAicClient::mQueryConnect[]    = "connect";
/* Disconnect from the camera device. */
const char CameraAicClient::mQueryDisconnect[] = "disconnect";
/* Query info from the webcam. */
const char CameraAicClient::mQueryInfo[]       = "infos";
/* Start capturing video from the camera device. */
const char CameraAicClient::mQueryStart[]      = "start";
/* Stop capturing video from the camera device. */
const char CameraAicClient::mQueryStop[]       = "stop";
/* Get next video frame from the camera device. */
const char CameraAicClient::mQueryFrame[]      = "frame";

CameraAicClient::CameraAicClient()
    : AicClient()
{
}

CameraAicClient::~CameraAicClient()
{

}

status_t CameraAicClient::queryConnect()
{
    ALOGV("%s", __FUNCTION__);

    QemuQuery query(mQueryConnect);
    doQuery(&query);
    const status_t res = query.getCompletionStatus();
    ALOGE_IF(res != NO_ERROR, "%s: Query failed: %s",
            __FUNCTION__, query.mReplyData ? query.mReplyData :
                                             "No error message");
    return res;
}

status_t CameraAicClient::queryDisconnect()
{
    ALOGV("%s", __FUNCTION__);

    QemuQuery query(mQueryDisconnect);
    doQuery(&query);
    const status_t res = query.getCompletionStatus();
    ALOGE_IF(res != NO_ERROR, "%s: Query failed: %s",
            __FUNCTION__, query.mReplyData ? query.mReplyData :
                                             "No error message");
    return res;
}

status_t CameraAicClient::queryInfo(char **p_info_string)
{
    ALOGV("%s", __FUNCTION__);

    if (!p_info_string) {
        ALOGE("%s: invalid parameter", __FUNCTION__);
    }

    QemuQuery query(mQueryInfo);
    if (doQuery(&query) || !query.isQuerySucceeded()) {
        ALOGE("%s: Camera info query failed: %s", __FUNCTION__,
             query.mReplyData ? query.mReplyData : "No error message");
        return query.getCompletionStatus();
    }

    /* Make sure there is info returned. */
    if (query.mReplyDataSize == 0) {
        ALOGE("%s: No camera info returned.", __FUNCTION__);
        return EINVAL;
    }

    *p_info_string = (char *)malloc(query.mReplyDataSize + 1);
    if (!*p_info_string) {
        ALOGE("%s: Failed to allocated info buffer", __FUNCTION__);
        return ENOMEM;
    }
    snprintf(*p_info_string, query.mReplyDataSize + 1, "%s", query.mReplyData);

    return NO_ERROR;//res;
}

status_t CameraAicClient::queryStart(uint32_t pixel_format,
                                      int width,
                                      int height)
{
    ALOGV("%s", __FUNCTION__);

    char query_str[256];
    snprintf(query_str, sizeof(query_str), "%s dim=%dx%d pix=%d",
             mQueryStart, width, height, pixel_format);
    QemuQuery query(query_str);
    doQuery(&query);
    const status_t res = query.getCompletionStatus();
    ALOGE_IF(res != NO_ERROR, "%s: Query failed: %s",
            __FUNCTION__, query.mReplyData ? query.mReplyData :
                                             "No error message");
    return res;
}

status_t CameraAicClient::queryStop()
{
    ALOGV("%s", __FUNCTION__);

    QemuQuery query(mQueryStop);
    doQuery(&query);
    const status_t res = query.getCompletionStatus();
    ALOGE_IF(res != NO_ERROR, "%s: Query failed: %s",
            __FUNCTION__, query.mReplyData ? query.mReplyData :
                                             "No error message");
    return res;
}

status_t CameraAicClient::queryFrame(void* vframe,
                                      void* pframe,
                                      size_t vframe_size,
                                      size_t pframe_size,
                                      float r_scale,
                                      float g_scale,
                                      float b_scale,
                                      float exposure_comp)
{
    ALOGV("%s", __FUNCTION__);

    char query_str[256];
    snprintf(query_str, sizeof(query_str), "%s video=%d preview=%d whiteb=%g,%g,%g expcomp=%g",
             mQueryFrame, (vframe && vframe_size) ? vframe_size : 0,
             (pframe && pframe_size) ? pframe_size : 0, r_scale, g_scale, b_scale,
             exposure_comp);
    QemuQuery query(query_str);
    doQuery(&query);
    const status_t res = query.getCompletionStatus();
    if( res != NO_ERROR) {
        ALOGE("%s: Query failed: %s",
             __FUNCTION__, query.mReplyData ? query.mReplyData :
                                              "No error message");
        return res;
    }

    /* Copy requested frames. */
    size_t cur_offset = 0;
    const uint8_t* frame = reinterpret_cast<const uint8_t*>(query.mReplyData);
    /* Video frame is always first. */
    if (vframe != NULL && vframe_size != 0) {
        /* Make sure that video frame is in. */
        if ((query.mReplyDataSize - cur_offset) >= vframe_size) {
            memcpy(vframe, frame, vframe_size);
            cur_offset += vframe_size;
        } else {
            ALOGE("%s: Reply %d bytes is to small to contain %d bytes video frame",
                 __FUNCTION__, query.mReplyDataSize - cur_offset, vframe_size);
            return EINVAL;
        }
    }
    if (pframe != NULL && pframe_size != 0) {
        /* Make sure that preview frame is in. */
        if ((query.mReplyDataSize - cur_offset) >= pframe_size) {
            memcpy(pframe, frame + cur_offset, pframe_size);
            cur_offset += pframe_size;
        } else {
            ALOGE("%s: Reply %d bytes is to small to contain %d bytes preview frame",
                 __FUNCTION__, query.mReplyDataSize - cur_offset, pframe_size);
            return EINVAL;
        }
    }

    return NO_ERROR;
}

}; /* namespace android */
