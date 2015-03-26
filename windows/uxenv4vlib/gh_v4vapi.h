/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * *   Redistributions of source code must retain the above
 *     copyright notice, this list of conditions and the
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the
 *     following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * uXen changes:
 *
 * Copyright 2015, Bromium, Inc.
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef __V4VAPI_H__
#define __V4VAPI_H__

#if !defined(XENV4V_DRIVER)
#define V4V_EXCLUDE_INTERNAL
#endif
#include <xen/v4v.h>

/* This structure is used for datagram reads and writes. When sending a
 * datagram, extra space must be reserved at the front of the buffer to
 * format the @addr values in the following structure to indicate the
 * destination address. When receiving data, the receive buffer should also
 * supply the extra head room for the source information that will be
 * returned by V4V. The size of the send/receive should include the extra
 * space for the datagram structure.
 */

#define V4V_DATAGRAM_FLAG_IGNORE_DLO        ( 1UL << 0)

#pragma pack(push, 1)
typedef struct v4v_datagram_struct {
    v4v_addr_t addr;
    uint16_t flags;
    /* data starts here */
} v4v_datagram_t;
#pragma pack(pop)

/* Typedef for internal stream header structure */
typedef struct v4v_stream_header v4v_stream_t, *Pv4v_stream_t;

/* ========================== IOCTL Interface ============================= */
#define V4V_DRIVER_NAME    L"xenv4v"
#define V4V_DEVICE_NAME    L"\\Device\\xenv4v"
#define V4V_SYMBOLIC_NAME  L"\\DosDevices\\Global\\v4vdev"
#define V4V_USER_FILE_NAME L"\\\\.\\Global\\v4vdev"
#define V4V_BASE_FILE_NAME L"v4vdev"

#define V4V_SYS_FILENAME   L"%SystemRoot%\\system32\\drivers\\xenv4v.sys"

/* Default internal max backlog length for pending connections */
#define V4V_SOMAXCONN 128

typedef struct v4v_init_values_struct {
    VOID *rx_event;
    ULONG32 ring_length;
} v4v_init_values_t;

typedef struct v4v_bind_values_struct {
    struct v4v_ring_id ringId;
} v4v_bind_values_t;

typedef struct v4v_listen_values_struct {
    ULONG32 backlog;
} v4v_listen_values_t;

typedef union v4v_accept_private_struct {
    struct {
        ULONG32 a;
        ULONG32 b;
    } d;
    struct {
        ULONG64 a;
    } q;
} v4v_accept_private_t;

typedef struct v4v_accept_values_struct {
    VOID *fileHandle;
    VOID *rx_event;
    struct v4v_addr peer_addr;
    v4v_accept_private_t priv;
} v4v_accept_values_t;

typedef struct v4v_connect_values_struct {
    v4v_stream_t sh;
    struct v4v_addr ringAddr;
} v4v_connect_values_t;

typedef struct v4v_wait_values_struct {
    v4v_stream_t sh;
} v4v_wait_values_t;

typedef enum v4v_getinfo_type_enum {
    V4V_INFO_UNSET    = 0,
    V4V_GET_LOCAL_INFO = 1,
    V4V_GET_PEER_INFO  = 2
} v4v_getinfo_type_t;

typedef struct v4v_getinfo_values_struct {
    v4v_getinfo_type_t type;
    struct v4v_ring_id ring_info;
} v4v_getinfo_values_t;

typedef struct v4v_mapring_values_struct {
    v4v_ring_t *ring;
} v4v_mapring_values_t;

typedef struct v4v_poke_values_struct {
    v4v_addr_t dst;
} v4v_poke_values_t;



#if defined(_WIN64)
#define V4V_64BIT 0x800
#else
#define V4V_64BIT 0x000
#endif

/* V4V I/O Control Function Codes */
#define V4V_FUNC_INITIALIZE 0x10
#define V4V_FUNC_BIND       0x11
#define V4V_FUNC_LISTEN     0x12
#define V4V_FUNC_ACCEPT     0x13
#define V4V_FUNC_CONNECT    0x14
#define V4V_FUNC_WAIT       0x15
#define V4V_FUNC_DISCONNECT 0x16
#define V4V_FUNC_GETINFO    0x17
#define V4V_FUNC_DUMPRING   0x18
#define V4V_FUNC_NOTIFY     0x19
#define V4V_FUNC_MAPRING    0x1a
#define V4V_FUNC_POKE       0x1b
#define V4V_FUNC_DEBUG      0x1c

/* V4V I/O Control Codes */
#if defined(_WIN64)
#define V4V_IOCTL_INITIALIZE CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_INITIALIZE|V4V_64BIT, METHOD_BUFFERED, FILE_ANY_ACCESS)
#else
#define V4V_IOCTL_INITIALIZE CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_INITIALIZE, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#define V4V_IOCTL_BIND       CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_BIND, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define V4V_IOCTL_LISTEN     CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_LISTEN, METHOD_BUFFERED, FILE_ANY_ACCESS)
#if defined(_WIN64)
#define V4V_IOCTL_ACCEPT     CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_ACCEPT|V4V_64BIT, METHOD_BUFFERED, FILE_ANY_ACCESS)
#else
#define V4V_IOCTL_ACCEPT     CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_ACCEPT, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#define V4V_IOCTL_CONNECT    CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_CONNECT, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define V4V_IOCTL_WAIT       CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_WAIT, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define V4V_IOCTL_DISCONNECT CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_DISCONNECT, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define V4V_IOCTL_GETINFO    CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_GETINFO, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define V4V_IOCTL_DUMPRING   CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_DUMPRING, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define V4V_IOCTL_NOTIFY     CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_NOTIFY, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define V4V_IOCTL_MAPRING    CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_MAPRING, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define V4V_IOCTL_POKE       CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_POKE, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define V4V_IOCTL_DEBUG      CTL_CODE(FILE_DEVICE_UNKNOWN, V4V_FUNC_DEBUG, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* =========================== User Mode API ============================== */

#if !defined(XENV4V_DRIVER)

/* The following must be included before this header:
 * #include <windows.h>
 * #include <winioctl.h>
 */

/* V4V for Windows uses the basic file I/O model standard Windows API
 * functions. All access to the V4V device is accomplished through use of a
 * Windows file handle returned by a call to CreateFile() in v4v_open().
 * Several other V4V calls can then be made to initialize the V4V file (which
 * represents a particular V4V channel) for the specific operations desired.
 * Once open and configured, Windows API file functions can be used to
 * read/write/control V4V file IO. The following are the functions that would
 * be used with V4V:
 *
 * ReadFile()/ReadFileEx()
 * WriteFile()/WriteFileEx()
 * CancelIo()/CancelIoEx()
 *
 * Note that V4V supports for file IO both synchronous blocking mode or
 * asynchronous mode through use of an OVERLAPPED structure or IO completion
 * routines. The caller should not attempt to manipulate the V4V device
 * directly with DeviceIoControl() calls. The proper IOCTLs are sent through
 * the set of functions in the V4V API below. The V4V API also returns an
 * event handle that is signalled when data arrives on a V4V channel. This
 * handle can be used in any Windows API functions that operate on events
 * (e.g. WaitForMultipleObjects() in conjunction with OVERLAPPED IO events).
 *
 * V4V supports both datagram/connectionless and sream/connection types of
 * communication.
 *
 * Datagrams:
 * A V4V channel must simply be bound to send and receive datagrams. When
 * reading datagrams, if the buffer is smaller than next message size the
 * extra bytes will be discarded. When writing, the message cannot exceed
 * the maximum message the V4V channel can accomodate and ERROR_MORE_DATA
 * is returned. If the destination does not exist, ERROR_VC_DISCONNECTED
 * is returned. If the channel is not bound then ERROR_INVALID_FUNCTION
 * will be returned for all IO operations.
 *
 * Streams:
 * v4v_listen()/v4v_accept()/v4v_connect()/v4v_connect_wait() are used to
 * establish a stream channel. Read operations will read the next chunk
 * of the stream data out of the V4V channel. Note the read length may
 * be less than the supplied buffer. Currently, if stream data chunk is
 * bigger than the supplied buffer, ERROR_MORE_DATA will be returned
 * indicating a bigger buffer should be used. When writing data chunks
 * the call will block or pend until enough room is available for the
 * send. The written chunk cannot exceed the maximum message the V4V
 * channel can accomodate and ERROR_MORE_DATA is returned. Attempts
 * to read and write after a reset or disconnection will result in
 * ERROR_VC_DISCONNECTED being returned.
 */

/* Define V4V_USE_INLINE_API to specify inline for the V4V API below */
#if defined(V4V_USE_INLINE_API)
#define V4V_INLINE_API __inline
#else
#define V4V_INLINE_API
#endif

/* Default @ringId for v4v_bind() to specify no specific binding information */
static const v4v_ring_id_t V4V_DEFAULT_CONNECT_ID = {{V4V_PORT_NONE, V4V_DOMID_NONE}, V4V_DOMID_NONE};

#define V4V_FLAG_NONE       0x00000000
#define V4V_FLAG_OVERLAPPED 0x00000001

/* Overlapped sanity check macro */
#define V4V_CHECK_OVERLAPPED(c, o) \
    if ((c->flags & V4V_FLAG_OVERLAPPED)&&(o == NULL)) { \
        SetLastError(ERROR_INVALID_PARAMETER); \
        return FALSE; \
    }

/* The following structure represents a V4V channel either opened with
 * v4v_open() or returned from a listening V4V channel in a call to
 * v4v_accept().
 *
 * The @v4v_handle is the file handle for an open instance of the V4V device.
 * This value is used in subsequent calls to read and write. The
 * @recv_event is a Windows auto-reset event handle that becomes signaled
 * when data arrived on the V4V channel associated with the open file.
 *
 * The @flags field can be set to V4V_FLAG_OVERLAPPED if the caller intends
 * to use overlapped or asynchronous file IO with the V4V handle. If blocking
 * IO mode is desired, then the flag should be set to V4V_FLAG_NONE. The
 * should be set before any call to the V4V functions and should not @flags
 * later be changed.
 */
typedef struct v4v_context_struct {
    HANDLE v4v_handle; /* handle for open V4V file */
    HANDLE recv_event; /* data arrival, new connection for accept */
    ULONG  flags;     /* configuration flags set by caller */
} v4v_context_t;

/* This routine opens a V4V file and associated channel. The @context
 * structure is passed in to the routine and if the call is successful, the
 * @v4v_handle and @recv_event handles will be valid and ready for use in
 * further V4V calls to initialize the channel.
 *
 * The @ring_size argument indicates how large the local receive ring for the
 * channel should be in bytes.
 *
 * If the V4V file is being opened with the V4V_FLAG_OVERLAPPED flag set then
 * the v4v_open() operation must be done asynchronously using the @ov overlapped
 * value. Otherwise @ov should be NULL and accept call will block until the
 * open is complete.
 *
 * The new open file handle and receive event are returned event though an
 * a overlapped call may not have yet completed. Until an overlapped call
 * completes the values in the @context should not be use.
 *
 * Returns TRUE on success or FALSE on error, in which case more
 * information can be obtained by calling GetLastError().
 */


static V4V_INLINE_API BOOLEAN
v4v_open(v4v_context_t *context, ULONG ring_size, OVERLAPPED *ov)
{
    HANDLE hd;
    v4v_init_values_t init = {0};
    BOOLEAN rc;
    DWORD br;

    if (context == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* V4V_CHECK_OVERLAPPED(context, ov); */
    /* make V4V more liberal about how context is initialized */
    context->flags = ov ? V4V_FLAG_OVERLAPPED : V4V_FLAG_NONE;

    context->recv_event = NULL;
    context->v4v_handle = INVALID_HANDLE_VALUE;

    hd = CreateFileW(V4V_USER_FILE_NAME, GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL | ((context->flags & V4V_FLAG_OVERLAPPED) ? FILE_FLAG_OVERLAPPED : 0),
                     NULL);
    if (hd == INVALID_HANDLE_VALUE)
        return FALSE;

    init.ring_length = ring_size;
    init.rx_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (init.rx_event == NULL) {
        CloseHandle(hd);
        return FALSE;
    }

    do {
        SetLastError(ERROR_SUCCESS);

        rc = DeviceIoControl(hd, V4V_IOCTL_INITIALIZE, &init, sizeof(v4v_init_values_t), NULL, 0, &br, ov);
        if (context->flags & V4V_FLAG_OVERLAPPED) {
            if ((GetLastError() != ERROR_SUCCESS) && (GetLastError() != ERROR_IO_PENDING)) {
                break;
            }
        } else if (!rc) {
            break;
        }

        context->v4v_handle = hd;
        context->recv_event = init.rx_event;

        return TRUE;
    } while (FALSE);

    CloseHandle(init.rx_event);
    CloseHandle(hd);
    return FALSE;
}

/* All users of V4V must call v4v_bind() before calling any of the other V4V
 * functions (excpetion v4v_close()) or before performing IO operations. When
 * binding, the @ringId->addr.domain field must be set to V4V_DOMID_NONE or
 * the bind operation will fail. Internally this value will be set to the
 * current domain ID.
 *
 * For V4V channels intended for datagram use, the @ringId->addr.port field
 * can be specified or not. If not specified, a random port number will be
 * assigned internally. The @ringId->partner value can be specified if
 * datagrams are to only be received from a specific partner domain for the
 * current V4V channel. If V4V_DOMID_ANY is specified, then datagrams from
 * any domain can be recieved. Note that V4V will send datagrams to channels
 * that match a specific domain ID before sending them to one bound with
 * (@ringId->partner == V4V_DOMID_ANY).
 *
 * The above rules apply when binding to start a listener though in general
 * one would want to specify a well known port for a listener. When binding
 * to do a connect, V4V_DEFAULT_CONNECT_ID can be used to allow internal
 * values to be selected.
 *
 * If the V4V file was opened with the V4V_FLAG_OVERLAPPED flag set then the
 * v4v_bind() operation must be done asynchronously using the @ov overlapped
 * value. Otherwise @ov should be NULL and accept call will block until the
 * bind is complete.
 *
 * Returns TRUE on success or FALSE on error, in which case more information
 * can be obtained by calling GetLastError(). ERROR_INVALID_FUNCTION will
 * be returned if the file is not in the proper state following a call to
 * v4v_open().
 */
static V4V_INLINE_API BOOLEAN
v4v_bind(v4v_context_t *context, v4v_ring_id_t *ringId, OVERLAPPED *ov)
{
    v4v_bind_values_t bind;
    DWORD br;
    BOOLEAN rc;

    if ((context == NULL) || (ringId == NULL)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    V4V_CHECK_OVERLAPPED(context, ov);

    memcpy(&bind.ringId, ringId, sizeof(v4v_ring_id_t));
    SetLastError(ERROR_SUCCESS);

    rc = DeviceIoControl(context->v4v_handle, V4V_IOCTL_BIND, &bind, sizeof(v4v_bind_values_t), NULL, 0, &br, ov);
    if (context->flags & V4V_FLAG_OVERLAPPED) {
        if ((GetLastError() != ERROR_SUCCESS) && (GetLastError() != ERROR_IO_PENDING)) {
            return FALSE;
        }
    } else if (!rc) {
        return FALSE;
    }
    return TRUE;
}

/* Information can be gotten about the local or peer address. If
 * V4V_GET_LOCAL_INFO is specified, @ring_infoOut will contain the ring
 * information that the channel is locally bound with. If V4V_GET_PEER_INFO
 * is specified then @ring_infoOut->addr will contain the remote peer
 * address information. V4V_GET_PEER_INFO can only be used on V4V channels in the
 * connected or accepted states.
 *
 * If the V4V file was opened with the V4V_FLAG_OVERLAPPED flag set then the
 * v4v_get_info() operation can be done asynchronously using the @ov overlapped
 * value. Otherwise @ov should be NULL and accept call will block until the
 * get info operation completes.
 *
 * For non-overlapped calls, the @ring_infoOut value will be filled in at the
 * end of the call. For overlapped calls, the caller must fetch the
 * v4v_getinfo_values_t structure during GetOverlappedResult() or in the
 * FileIOCompletionRoutine.
 *
 * The caller must suppy the @infoOut argument. Upon synchronous completion
 * of the call, this structure will have the @infoOut.ring_info field filled
 * in (the other fields should be ignored). For overlapped calls, the caller
 * retain the @infoOut structure until IO is completed (this is effectively
 * the output buffer for the IOCLT). During GetOverlappedResult() or in the
 * FileIOCompletionRoutine the @infoOut.ring_info value can be fetched and
 * @acceptOut released etc.
 *
 * Returns TRUE on success or FALSE on error, in which case more information
 * can be obtained by calling GetLastError(). ERROR_INVALID_FUNCTION will
 * be returned if the file is not in the proper state following a call to
 * get the information.
 */
static V4V_INLINE_API BOOLEAN
v4v_get_info(v4v_context_t *context, v4v_getinfo_type_t type, v4v_getinfo_values_t *infoOut, OVERLAPPED *ov)
{
    v4v_getinfo_values_t info = {V4V_INFO_UNSET, {{V4V_PORT_NONE, V4V_DOMID_NONE}, V4V_DOMID_NONE}};
    DWORD br;
    BOOLEAN rc;

    if ((context == NULL) || (infoOut == NULL)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    V4V_CHECK_OVERLAPPED(context, ov);

    info.type = type;
    ZeroMemory(infoOut, sizeof(v4v_getinfo_values_t));
    infoOut->type = V4V_INFO_UNSET;
    SetLastError(ERROR_SUCCESS);

    rc = DeviceIoControl(context->v4v_handle, V4V_IOCTL_GETINFO,
                         &info, sizeof(v4v_getinfo_values_t),
                         infoOut, sizeof(v4v_getinfo_values_t), &br, ov);
    if (context->flags & V4V_FLAG_OVERLAPPED) {
        if ((GetLastError() != ERROR_SUCCESS) && (GetLastError() != ERROR_IO_PENDING)) {
            return FALSE;
        }
    } else if (!rc) {
        return FALSE;
    }

    return TRUE;
}


/* The kernel allocated v4v_ring_t can be mapped into user space for
 * zero-copy operation
 *
 * If the V4V file was opened with the V4V_FLAG_OVERLAPPED flag set then the
 * v4v_map() operation must be done asynchronously using the @ov overlapped
 * value. Otherwise @ov should be NULL and accept call will block until the
 * get info operation completes.
 *
 * For non-overlapped calls, the @ring value will be filled in at the
 * end of the call. For overlapped calls, the caller must fetch the
 * v4v_mapring_values_t structure after calling GetOverlappedResult() or in the
 * FileIOCompletionRoutine.
 *
 * The caller must suppy the @ring argument. Upon synchronous completion
 * of the call, this structure will have the @ring.ring field filled
 * in. For overlapped calls, the caller should maintain
 * the @ring structure until IO is completed. During GetOverlappedResult() or in the
 * FileIOCompletionRoutine the @ring.ring value can be fetched and
 * @ring released etc.
 *
 * Returns TRUE on success or FALSE on error, in which case more information
 * can be obtained by calling GetLastError(). ERROR_INVALID_FUNCTION will
 * be returned if the file is not in the proper state following a call to
 * get the information.
 */
static V4V_INLINE_API BOOLEAN
v4v_map(v4v_context_t *context, v4v_mapring_values_t *ring, OVERLAPPED *ov)
{
    DWORD br;
    BOOLEAN rc;
    v4v_mapring_values_t mr = {0};

    if ((context == NULL) || (ring == NULL)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    V4V_CHECK_OVERLAPPED(context, ov);

    ZeroMemory(ring, sizeof(v4v_mapring_values_t));
    SetLastError(ERROR_SUCCESS);

    rc = DeviceIoControl(context->v4v_handle, V4V_IOCTL_MAPRING,
                         &mr, sizeof(v4v_mapring_values_t),
                         ring, sizeof(v4v_mapring_values_t), &br, ov);
    if (context->flags & V4V_FLAG_OVERLAPPED) {
        if ((GetLastError() != ERROR_SUCCESS) && (GetLastError() != ERROR_IO_PENDING)) {
            return FALSE;
        }
    } else if (!rc) {
        return FALSE;
    }

    return TRUE;
}

/* This utility routine will dump the current state of the V4V ring to the
 * various driver trace targets (like KD, Xen etc).
 *
 * If the V4V file was opened with the V4V_FLAG_OVERLAPPED flag set then the
 * v4v_get_info() operation can be done asynchronously using the @ov overlapped
 * value. Otherwise @ov should be NULL and accept call will block until the
 * get info operation completes.
 *
 * Returns TRUE on success or FALSE on error, in which case more information
 * can be obtained by calling GetLastError(). ERROR_INVALID_FUNCTION will
 * be returned if the file is not in the proper state following a call to
 * dump the ring.
 */
static V4V_INLINE_API BOOLEAN
gh_v4v_dump_ring(v4v_context_t *context, OVERLAPPED *ov)
{
    DWORD br;
    BOOLEAN rc;

    if (context == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    V4V_CHECK_OVERLAPPED(context, ov);

    SetLastError(ERROR_SUCCESS);

    rc = DeviceIoControl(context->v4v_handle, V4V_IOCTL_DUMPRING, NULL, 0, NULL, 0, &br, ov);
    if (context->flags & V4V_FLAG_OVERLAPPED) {
        if ((GetLastError() != ERROR_SUCCESS) && (GetLastError() != ERROR_IO_PENDING)) {
            return FALSE;
        }
    } else if (!rc) {
        return FALSE;
    }

    return TRUE;
}

/* This routine is called to tell the hypervisor that we have removed
 * data from a mapped ring, it applies to all rings in the system
 * so only needs to be called once per block of reads/ dispatch.
 * It causes the hypervisor to signal other vms that they are
 * able to resume sending data.
 *
 * Returns TRUE on success or FALSE on error, in which case more information
 * can be obtained by calling GetLastError().
 *
 */
static V4V_INLINE_API BOOLEAN
gh_v4v_notify(v4v_context_t *context, OVERLAPPED *ov)
{
    DWORD br;
    BOOLEAN rc;

    if (context == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    V4V_CHECK_OVERLAPPED(context, ov);

    SetLastError(ERROR_SUCCESS);

    rc = DeviceIoControl(context->v4v_handle, V4V_IOCTL_NOTIFY, NULL, 0, NULL, 0, &br, ov);
    if (context->flags & V4V_FLAG_OVERLAPPED) {
        if ((GetLastError() != ERROR_SUCCESS) && (GetLastError() != ERROR_IO_PENDING)) {
            return FALSE;
        }
    } else if (!rc) {
        return FALSE;
    }

    return TRUE;
}

/* This routine is called to tell the hypervisor to poke a ring
 * ring in another domain - this should cause a suspended domain
 * to reconnect to v4v
 *
 * Returns TRUE on success or FALSE on error, in which case more information
 * can be obtained by calling GetLastError().
 *
 */
static V4V_INLINE_API BOOLEAN
v4v_poke(v4v_context_t *context, v4v_addr_t *dst, OVERLAPPED *ov)
{
    v4v_poke_values_t poke;
    DWORD br;
    BOOLEAN rc;

    if ((context == NULL) || (dst == NULL)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    V4V_CHECK_OVERLAPPED(context, ov);

    ZeroMemory(&poke, sizeof(v4v_poke_values_t));
    memcpy(&poke.dst, dst, sizeof(v4v_addr_t));
    SetLastError(ERROR_SUCCESS);

    rc = DeviceIoControl(context->v4v_handle, V4V_IOCTL_POKE, &poke,
                         sizeof(v4v_poke_values_t), NULL, 0, &br, ov);
    if (context->flags & V4V_FLAG_OVERLAPPED) {
        if ((GetLastError() != ERROR_SUCCESS) && (GetLastError() != ERROR_IO_PENDING)) {
            return FALSE;
        }
    } else if (!rc) {
        return FALSE;
    }
    return TRUE;
}

/* This routine should be used to close the @context handles returned from a
 * call to v4v_open(). It can be called at any time to close the file handle
 * and terminate all outstanding IO.
 *
 * Returns TRUE on success or FALSE on error, in which case more information
 * can be obtained by calling GetLastError().
 */
static V4V_INLINE_API BOOLEAN
v4v_close(v4v_context_t *context)
{
    BOOLEAN rc = TRUE;

    if (context == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (context->recv_event != NULL) {
        if (CloseHandle(context->recv_event))
            context->recv_event = NULL;
        else
            rc = FALSE;
    }

    if ((context->v4v_handle != INVALID_HANDLE_VALUE) && (context->v4v_handle != NULL)) {
        if (CloseHandle(context->v4v_handle))
            context->v4v_handle = INVALID_HANDLE_VALUE;
        else
            rc = FALSE;
    }

    return rc;
}

static V4V_INLINE_API BOOLEAN
v4v_debug(v4v_context_t *context, OVERLAPPED *ov)
{
    DWORD br;
    BOOLEAN rc;

    if (context == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    V4V_CHECK_OVERLAPPED(context, ov);

    SetLastError(ERROR_SUCCESS);

    rc = DeviceIoControl(context->v4v_handle, V4V_IOCTL_DEBUG, NULL, 0, NULL, 0, &br, ov);
    if (context->flags & V4V_FLAG_OVERLAPPED) {
        if ((GetLastError() != ERROR_SUCCESS) && (GetLastError() != ERROR_IO_PENDING)) {
            return FALSE;
        }
    } else if (!rc) {
        return FALSE;
    }

    return TRUE;
}

#endif /* XENV4V_DRIVER */

#endif /* !__V4VAPI_H__ */
