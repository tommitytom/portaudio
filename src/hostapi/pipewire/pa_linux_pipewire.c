/*
 * Portable Audio I/O Library PipeWire implementation
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The text above constitutes the entire PortAudio license; however,
 * the PortAudio community also makes the following non-binding requests:
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version. It is also
 * requested that these non-binding requests be included along with the
 * license above.
 */

/** @file
 @ingroup hostapi_src

 @brief PipeWire implementation of the PortAudio host API.
*/

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"
#include "pa_ringbuffer.h"
#include "pa_debugprint.h"

/* -------------------------------------------------------------------------------------------------
 * Optional runtime loading of libpipewire (PA_PIPEWIRE_DYNAMIC). When enabled, the backend is
 * compiled but libpipewire-0.3 is NOT linked; it is dlopen'd on first use so a binary that ships the
 * PipeWire host API still launches on a machine without libpipewire-0.3.so.0 (PaPipeWire_Initialize
 * then soft-fails and Pa_Initialize omits the PipeWire host API). Mirrors the ALSA PA_ALSA_DYNAMIC
 * pattern. NOTE: only the real EXPORTED pw_* functions are loaded; pw_core_add_listener /
 * pw_core_get_registry / pw_core_sync / pw_registry_add_listener are header-only macros (they call
 * spa interface methods on the proxies) and need no dlsym.
 * ------------------------------------------------------------------------------------------------- */
#ifdef PA_PIPEWIRE_DYNAMIC
#include <dlfcn.h>

#define PA_PW_FUNCS(_) \
    _(pw_init) _(pw_deinit) _(pw_context_new) _(pw_context_connect) _(pw_context_destroy) \
    _(pw_core_disconnect) _(pw_proxy_destroy) _(pw_properties_new) _(pw_properties_set) \
    _(pw_stream_new) _(pw_stream_add_listener) _(pw_stream_connect) _(pw_stream_disconnect) \
    _(pw_stream_destroy) _(pw_stream_dequeue_buffer) _(pw_stream_queue_buffer) _(pw_stream_get_time_n) \
    _(pw_thread_loop_new) _(pw_thread_loop_destroy) _(pw_thread_loop_get_loop) _(pw_thread_loop_start) \
    _(pw_thread_loop_stop) _(pw_thread_loop_lock) _(pw_thread_loop_unlock) _(pw_thread_loop_signal) \
    _(pw_thread_loop_wait)

/* A function pointer per call, typed off the real prototype (pipewire.h included above). Declared
 * BEFORE the redirect #defines so __typeof__ sees the real symbols. */
#define PA_PW_DECL(name) static __typeof__(name) *pa_dyn_##name = NULL;
PA_PW_FUNCS(PA_PW_DECL)
#undef PA_PW_DECL

static void *g_pwLib = NULL;

static int PaPipeWire_LoadLibrary( void )
{
    const char *soname = getenv( "PA_PIPEWIRE_SONAME" );
    if( !soname || !*soname ) soname = "libpipewire-0.3.so.0";
    if( g_pwLib ) return 1;
    dlerror();
    g_pwLib = dlopen( soname, RTLD_NOW | RTLD_GLOBAL );
    if( !g_pwLib ) return 0;
#define PA_PW_LOAD(name) \
    *(void **)( &pa_dyn_##name ) = dlsym( g_pwLib, #name ); \
    if( !pa_dyn_##name ) { dlclose( g_pwLib ); g_pwLib = NULL; return 0; }
    PA_PW_FUNCS(PA_PW_LOAD)
#undef PA_PW_LOAD
    return 1;
}

static void PaPipeWire_UnloadLibrary( void )
{
    if( g_pwLib ) { dlclose( g_pwLib ); g_pwLib = NULL; }
}

/* Redirect every call site to its pointer (must come AFTER the __typeof__ declarations above). The
 * four header macros above are intentionally NOT redirected. */
#define pw_init                  pa_dyn_pw_init
#define pw_deinit                pa_dyn_pw_deinit
#define pw_context_new           pa_dyn_pw_context_new
#define pw_context_connect       pa_dyn_pw_context_connect
#define pw_context_destroy       pa_dyn_pw_context_destroy
#define pw_core_disconnect       pa_dyn_pw_core_disconnect
#define pw_proxy_destroy         pa_dyn_pw_proxy_destroy
#define pw_properties_new        pa_dyn_pw_properties_new
#define pw_properties_set        pa_dyn_pw_properties_set
#define pw_stream_new            pa_dyn_pw_stream_new
#define pw_stream_add_listener   pa_dyn_pw_stream_add_listener
#define pw_stream_connect        pa_dyn_pw_stream_connect
#define pw_stream_disconnect     pa_dyn_pw_stream_disconnect
#define pw_stream_destroy        pa_dyn_pw_stream_destroy
#define pw_stream_dequeue_buffer pa_dyn_pw_stream_dequeue_buffer
#define pw_stream_queue_buffer   pa_dyn_pw_stream_queue_buffer
#define pw_stream_get_time_n     pa_dyn_pw_stream_get_time_n
#define pw_thread_loop_new       pa_dyn_pw_thread_loop_new
#define pw_thread_loop_destroy   pa_dyn_pw_thread_loop_destroy
#define pw_thread_loop_get_loop  pa_dyn_pw_thread_loop_get_loop
#define pw_thread_loop_start     pa_dyn_pw_thread_loop_start
#define pw_thread_loop_stop      pa_dyn_pw_thread_loop_stop
#define pw_thread_loop_lock      pa_dyn_pw_thread_loop_lock
#define pw_thread_loop_unlock    pa_dyn_pw_thread_loop_unlock
#define pw_thread_loop_signal    pa_dyn_pw_thread_loop_signal
#define pw_thread_loop_wait      pa_dyn_pw_thread_loop_wait
#else
static int  PaPipeWire_LoadLibrary( void ) { return 1; }  /* libpipewire is linked; nothing to load */
static void PaPipeWire_UnloadLibrary( void ) {}
#endif /* PA_PIPEWIRE_DYNAMIC */


/* Default latency values in seconds */
#define PA_PIPEWIRE_DEFAULT_LOW_LATENCY     0.01
#define PA_PIPEWIRE_DEFAULT_HIGH_LATENCY    0.08

/* Default sample rate when PipeWire doesn't report one */
#define PA_PIPEWIRE_DEFAULT_SAMPLE_RATE     48000

/* Maximum number of devices we'll track during enumeration */
#define PA_PIPEWIRE_MAX_DEVICES             256

/* Ring buffer size for blocking mode (in frames at high latency) */
#define PA_PIPEWIRE_RING_FRAMES_FACTOR      4


#define PA_PIPEWIRE_SET_LAST_HOST_ERROR( errorCode, errorText ) \
    PaUtil_SetLastHostErrorInfo( paPipeWire, errorCode, errorText )


/* Forward declarations */
#ifdef __cplusplus
extern "C"
{
#endif

PaError PaPipeWire_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );

#ifdef __cplusplus
}
#endif

static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTime GetStreamTime( PaStream *stream );
static double GetStreamCpuLoad( PaStream* stream );
static PaError ReadStream( PaStream* stream, void *buffer, unsigned long frames );
static PaError WriteStream( PaStream* stream, const void *buffer, unsigned long frames );
static signed long GetStreamReadAvailable( PaStream* stream );
static signed long GetStreamWriteAvailable( PaStream* stream );


/* ---- Data structures ---- */

typedef struct PaPipeWireDeviceInfo
{
    uint32_t    nodeId;
    char       *nodeName;
    int         isSource;   /* 1 = input/source, 0 = output/sink */
    int         priority;
    int         maxChannels;
    double      defaultSampleRate;
}
PaPipeWireDeviceInfo;

typedef struct
{
    PaUtilHostApiRepresentation  inheritedHostApiRep;
    PaUtilStreamInterface        callbackStreamInterface;
    PaUtilStreamInterface        blockingStreamInterface;
    PaUtilAllocationGroup       *allocations;
    PaHostApiIndex               hostApiIndex;

    /* PipeWire connection */
    struct pw_thread_loop       *loop;
    struct pw_context            *context;
    struct pw_core              *core;
    struct pw_registry          *registry;
    struct spa_hook              coreListener;
    struct spa_hook              registryListener;

    /* Device enumeration state */
    PaPipeWireDeviceInfo         enumDevices[PA_PIPEWIRE_MAX_DEVICES];
    int                          enumDeviceCount;
    int                          syncSeq;
    int                          enumerationDone;
}
PaPipeWireHostApiRepresentation;


typedef struct PaPipeWireStream
{
    PaUtilStreamRepresentation   streamRepresentation;
    PaUtilCpuLoadMeasurer        cpuLoadMeasurer;
    PaUtilBufferProcessor        bufferProcessor;
    PaPipeWireHostApiRepresentation *hostApi;

    /* PipeWire streams */
    struct pw_stream            *outputStream;
    struct pw_stream            *inputStream;
    struct spa_hook              outputListener;
    struct spa_hook              inputListener;

    /* Stream parameters */
    int                          inputChannelCount;
    int                          outputChannelCount;
    int                          inputFrameSize;
    int                          outputFrameSize;
    double                       sampleRate;
    uint32_t                     inputNodeId;
    uint32_t                     outputNodeId;

    /* State */
    volatile sig_atomic_t        isActive;
    volatile sig_atomic_t        isStopped;
    volatile sig_atomic_t        callbackAbort;

    /* Mode */
    int                          callbackMode;

    /* Ring buffers (used for blocking mode AND full-duplex input buffering) */
    PaUtilRingBuffer             inputRing;
    PaUtilRingBuffer             outputRing;
    void                        *inputRingData;
    void                        *outputRingData;
}
PaPipeWireStream;


/* ---- PipeWire format conversion ---- */

static enum spa_audio_format PaSampleFormatToSpa( PaSampleFormat format )
{
    switch( format & ~paNonInterleaved )
    {
        case paFloat32: return SPA_AUDIO_FORMAT_F32;
        case paInt32:   return SPA_AUDIO_FORMAT_S32;
        case paInt24:   return SPA_AUDIO_FORMAT_S24_32;
        case paInt16:   return SPA_AUDIO_FORMAT_S16;
        case paInt8:    return SPA_AUDIO_FORMAT_S8;
        case paUInt8:   return SPA_AUDIO_FORMAT_U8;
        default:        return SPA_AUDIO_FORMAT_F32;
    }
}

static unsigned long RoundUpPow2( unsigned long v )
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}


/* ---- Registry events for device enumeration ---- */

static void OnRegistryGlobal( void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props )
{
    PaPipeWireHostApiRepresentation *pwHostApi = (PaPipeWireHostApiRepresentation *)data;
    const char *mediaClass;
    const char *nodeName;
    const char *nodeDesc;
    const char *channelsStr;
    const char *rateStr;
    const char *priorityStr;
    int isSource;
    PaPipeWireDeviceInfo *dev;

    (void)permissions;
    (void)version;

    if( strcmp( type, PW_TYPE_INTERFACE_Node ) != 0 )
        return;

    if( !props )
        return;

    mediaClass = spa_dict_lookup( props, PW_KEY_MEDIA_CLASS );
    if( !mediaClass )
        return;

    if( strcmp( mediaClass, "Audio/Sink" ) == 0 )
        isSource = 0;
    else if( strcmp( mediaClass, "Audio/Source" ) == 0 )
        isSource = 1;
    else
        return;

    if( pwHostApi->enumDeviceCount >= PA_PIPEWIRE_MAX_DEVICES )
        return;

    nodeDesc = spa_dict_lookup( props, PW_KEY_NODE_DESCRIPTION );
    nodeName = spa_dict_lookup( props, PW_KEY_NODE_NAME );

    dev = &pwHostApi->enumDevices[pwHostApi->enumDeviceCount];
    dev->nodeId = id;
    dev->isSource = isSource;

    /* Use description if available, fall back to node name */
    if( nodeDesc && nodeDesc[0] )
        dev->nodeName = strdup( nodeDesc );
    else if( nodeName && nodeName[0] )
        dev->nodeName = strdup( nodeName );
    else
        dev->nodeName = strdup( "Unknown PipeWire Device" );

    channelsStr = spa_dict_lookup( props, PW_KEY_AUDIO_CHANNELS );
    dev->maxChannels = channelsStr ? atoi( channelsStr ) : 2;
    if( dev->maxChannels <= 0 )
        dev->maxChannels = 2;

    rateStr = spa_dict_lookup( props, PW_KEY_AUDIO_RATE );
    dev->defaultSampleRate = rateStr ? atoi( rateStr ) : PA_PIPEWIRE_DEFAULT_SAMPLE_RATE;
    if( dev->defaultSampleRate <= 0 )
        dev->defaultSampleRate = PA_PIPEWIRE_DEFAULT_SAMPLE_RATE;

    priorityStr = spa_dict_lookup( props, PW_KEY_PRIORITY_SESSION );
    dev->priority = priorityStr ? atoi( priorityStr ) : 0;

    pwHostApi->enumDeviceCount++;
}

static const struct pw_registry_events registryEvents = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = OnRegistryGlobal,
};


/* ---- Core events for sync ---- */

static void OnCoreDone( void *data, uint32_t id, int seq )
{
    PaPipeWireHostApiRepresentation *pwHostApi = (PaPipeWireHostApiRepresentation *)data;

    if( id == PW_ID_CORE && seq == pwHostApi->syncSeq )
    {
        pwHostApi->enumerationDone = 1;
        pw_thread_loop_signal( pwHostApi->loop, 0 );
    }
}

static void OnCoreError( void *data, uint32_t id, int seq, int res, const char *message )
{
    PaPipeWireHostApiRepresentation *pwHostApi = (PaPipeWireHostApiRepresentation *)data;

    (void)id;
    (void)seq;
    (void)res;

    PA_DEBUG(( "PipeWire core error: %s\n", message ));

    pwHostApi->enumerationDone = 1;
    pw_thread_loop_signal( pwHostApi->loop, 0 );
}

static const struct pw_core_events coreEvents = {
    PW_VERSION_CORE_EVENTS,
    .done = OnCoreDone,
    .error = OnCoreError,
};


/* ---- Process callbacks ---- */

static void OnOutputProcess( void *userdata )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)userdata;
    struct pw_buffer *pwBuf;
    struct spa_buffer *spaBuf;
    void *outputData;
    uint32_t maxFrames;
    uint32_t stride;
    int callbackResult;
    unsigned long framesProcessed;
    PaStreamCallbackTimeInfo timeInfo = {0, 0, 0};
    PaStreamCallbackFlags statusFlags = 0;

    pwBuf = pw_stream_dequeue_buffer( stream->outputStream );
    if( !pwBuf )
        return;

    spaBuf = pwBuf->buffer;
    outputData = spaBuf->datas[0].data;
    if( !outputData )
    {
        pw_stream_queue_buffer( stream->outputStream, pwBuf );
        return;
    }

    stride = stream->outputFrameSize;
    maxFrames = spaBuf->datas[0].maxsize / stride;
    if( pwBuf->requested > 0 && pwBuf->requested < maxFrames )
        maxFrames = pwBuf->requested;

    if( stream->callbackMode )
    {
        /* Callback mode */
        PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

        PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, statusFlags );

        /* Handle input if full-duplex: read from ring buffer filled by input callback */
        if( stream->inputStream && stream->inputRingData )
        {
            ring_buffer_size_t available = PaUtil_GetRingBufferReadAvailable( &stream->inputRing );
            if( available > 0 )
            {
                ring_buffer_size_t inFrames = available;
                if( (uint32_t)inFrames > maxFrames )
                    inFrames = (ring_buffer_size_t)maxFrames;

                /* Read into a temp buffer on the stack */
                char tempBuf[8192];
                uint32_t bytesNeeded = inFrames * stream->inputFrameSize;
                if( bytesNeeded > sizeof(tempBuf) )
                {
                    inFrames = sizeof(tempBuf) / stream->inputFrameSize;
                    bytesNeeded = inFrames * stream->inputFrameSize;
                }

                PaUtil_ReadRingBuffer( &stream->inputRing, tempBuf, inFrames );
                PaUtil_SetInputFrameCount( &stream->bufferProcessor, inFrames );
                PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor, 0, tempBuf, 0 );
            }
            else
            {
                PaUtil_SetInputFrameCount( &stream->bufferProcessor, 0 );
                PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor, 0, NULL, 0 );
            }
        }

        PaUtil_SetOutputFrameCount( &stream->bufferProcessor, maxFrames );
        PaUtil_SetInterleavedOutputChannels( &stream->bufferProcessor, 0, outputData, 0 );

        callbackResult = stream->callbackAbort ? paAbort : paContinue;
        framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );

        spaBuf->datas[0].chunk->offset = 0;
        spaBuf->datas[0].chunk->stride = stride;
        spaBuf->datas[0].chunk->size = framesProcessed * stride;

        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

        if( callbackResult == paAbort || callbackResult == paComplete )
        {
            stream->callbackAbort = 1;
            stream->isActive = 0;

            if( stream->streamRepresentation.streamFinishedCallback )
                stream->streamRepresentation.streamFinishedCallback(
                    stream->streamRepresentation.userData );
        }
    }
    else
    {
        /* Blocking mode - read from output ring buffer */
        ring_buffer_size_t available = PaUtil_GetRingBufferReadAvailable( &stream->outputRing );
        ring_buffer_size_t toRead = (ring_buffer_size_t)maxFrames;
        if( toRead > available )
            toRead = available;

        if( toRead > 0 )
        {
            PaUtil_ReadRingBuffer( &stream->outputRing, outputData, toRead );
            /* Zero-fill any remaining frames */
            if( (uint32_t)toRead < maxFrames )
                memset( (char *)outputData + toRead * stride, 0, (maxFrames - toRead) * stride );
        }
        else
        {
            memset( outputData, 0, maxFrames * stride );
        }

        spaBuf->datas[0].chunk->offset = 0;
        spaBuf->datas[0].chunk->stride = stride;
        spaBuf->datas[0].chunk->size = maxFrames * stride;

        /* Signal the blocking write that space is available */
        pw_thread_loop_signal( stream->hostApi->loop, 0 );
    }

    pw_stream_queue_buffer( stream->outputStream, pwBuf );
}

static void OnInputProcess( void *userdata )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)userdata;
    struct pw_buffer *pwBuf;
    struct spa_buffer *spaBuf;
    void *inputData;
    uint32_t frames;

    pwBuf = pw_stream_dequeue_buffer( stream->inputStream );
    if( !pwBuf )
        return;

    spaBuf = pwBuf->buffer;
    inputData = spaBuf->datas[0].data;
    if( !inputData )
    {
        pw_stream_queue_buffer( stream->inputStream, pwBuf );
        return;
    }

    frames = spaBuf->datas[0].chunk->size / stream->inputFrameSize;

    if( stream->inputRingData )
    {
        /* Full-duplex callback mode or blocking mode: write to ring buffer */
        ring_buffer_size_t toWrite = (ring_buffer_size_t)frames;
        ring_buffer_size_t space = PaUtil_GetRingBufferWriteAvailable( &stream->inputRing );
        if( toWrite > space )
            toWrite = space;

        if( toWrite > 0 )
            PaUtil_WriteRingBuffer( &stream->inputRing, inputData, toWrite );

        if( !stream->callbackMode )
            pw_thread_loop_signal( stream->hostApi->loop, 0 );
    }
    else if( stream->callbackMode )
    {
        /* Input-only callback mode (no ring buffer needed) */
        PaStreamCallbackTimeInfo timeInfo = {0, 0, 0};
        PaStreamCallbackFlags statusFlags = 0;
        int callbackResult;
        unsigned long framesProcessed;

        PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

        PaUtil_BeginBufferProcessing( &stream->bufferProcessor, &timeInfo, statusFlags );

        PaUtil_SetInputFrameCount( &stream->bufferProcessor, frames );
        PaUtil_SetInterleavedInputChannels( &stream->bufferProcessor, 0, inputData, 0 );

        callbackResult = stream->callbackAbort ? paAbort : paContinue;
        framesProcessed = PaUtil_EndBufferProcessing( &stream->bufferProcessor, &callbackResult );

        PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );

        if( callbackResult == paAbort || callbackResult == paComplete )
        {
            stream->callbackAbort = 1;
            stream->isActive = 0;

            if( stream->streamRepresentation.streamFinishedCallback )
                stream->streamRepresentation.streamFinishedCallback(
                    stream->streamRepresentation.userData );
        }
    }

    pw_stream_queue_buffer( stream->inputStream, pwBuf );
}

static const struct pw_stream_events outputStreamEvents = {
    PW_VERSION_STREAM_EVENTS,
    .process = OnOutputProcess,
};

static const struct pw_stream_events inputStreamEvents = {
    PW_VERSION_STREAM_EVENTS,
    .process = OnInputProcess,
};


/* ---- Helper to create and connect a PipeWire stream ---- */

static PaError CreateAndConnectStream( PaPipeWireStream *stream,
                                        struct pw_stream **pwStream,
                                        struct spa_hook *listener,
                                        const struct pw_stream_events *events,
                                        enum spa_direction direction,
                                        int channelCount,
                                        double sampleRate,
                                        uint32_t targetNodeId )
{
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT( buffer, sizeof(buffer) );
    const struct spa_pod *params[1];
    struct spa_audio_info_raw rawInfo;
    struct pw_properties *props;
    char targetStr[32];

    props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, direction == SPA_DIRECTION_OUTPUT ? "Playback" : "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        NULL );

    if( targetNodeId != SPA_ID_INVALID )
    {
        snprintf( targetStr, sizeof(targetStr), "%u", targetNodeId );
        pw_properties_set( props, PW_KEY_TARGET_OBJECT, targetStr );
    }

    *pwStream = pw_stream_new(
        stream->hostApi->core,
        direction == SPA_DIRECTION_OUTPUT ? "PortAudio Playback" : "PortAudio Capture",
        props );

    if( !*pwStream )
        return paInsufficientMemory;

    spa_zero( *listener );
    pw_stream_add_listener( *pwStream, listener, events, stream );

    spa_zero( rawInfo );
    rawInfo.format = SPA_AUDIO_FORMAT_F32;
    rawInfo.rate = (uint32_t)sampleRate;
    rawInfo.channels = (uint32_t)channelCount;

    params[0] = spa_format_audio_raw_build( &b, SPA_PARAM_EnumFormat, &rawInfo );

    return pw_stream_connect( *pwStream,
        direction == SPA_DIRECTION_OUTPUT ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
        PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS,
        params, 1 ) == 0 ? paNoError : paUnanticipatedHostError;
}


/* ---- Initialization ---- */

PaError PaPipeWire_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    PaPipeWireHostApiRepresentation *pwHostApi;
    PaDeviceInfo *deviceInfoArray;
    int i, deviceCount;
    int defaultInput = paNoDevice;
    int defaultOutput = paNoDevice;
    int highestInputPriority = -1;
    int highestOutputPriority = -1;

    /* libpipewire absent (PA_PIPEWIRE_DYNAMIC): leave *hostApi NULL and return paNoError so
     * Pa_Initialize skips the PipeWire host API instead of aborting all audio. No-op when linked. */
    if( !PaPipeWire_LoadLibrary() )
    {
        *hostApi = NULL;
        return paNoError;
    }

    pw_init( NULL, NULL );

    pwHostApi = (PaPipeWireHostApiRepresentation *)PaUtil_AllocateZeroInitializedMemory(
        sizeof(PaPipeWireHostApiRepresentation) );
    if( !pwHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    pwHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !pwHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    pwHostApi->hostApiIndex = hostApiIndex;

    /* Create thread loop and context */
    pwHostApi->loop = pw_thread_loop_new( "pa-pipewire", NULL );
    if( !pwHostApi->loop )
    {
        PA_DEBUG(( "PipeWire: failed to create thread loop\n" ));
        result = paNoError;
        goto error_no_pipewire;
    }

    pwHostApi->context = pw_context_new(
        pw_thread_loop_get_loop( pwHostApi->loop ), NULL, 0 );
    if( !pwHostApi->context )
    {
        PA_DEBUG(( "PipeWire: failed to create context\n" ));
        result = paNoError;
        goto error_no_pipewire;
    }

    if( pw_thread_loop_start( pwHostApi->loop ) < 0 )
    {
        PA_DEBUG(( "PipeWire: failed to start thread loop\n" ));
        result = paNoError;
        goto error_no_pipewire;
    }

    pw_thread_loop_lock( pwHostApi->loop );

    /* Connect to the PipeWire daemon */
    pwHostApi->core = pw_context_connect( pwHostApi->context, NULL, 0 );
    if( !pwHostApi->core )
    {
        pw_thread_loop_unlock( pwHostApi->loop );
        PA_DEBUG(( "PipeWire: could not connect to daemon\n" ));
        result = paNoError;
        goto error_no_pipewire;
    }

    /* Set up core listener for sync */
    spa_zero( pwHostApi->coreListener );
    pw_core_add_listener( pwHostApi->core, &pwHostApi->coreListener, &coreEvents, pwHostApi );

    /* Create registry and listen for devices */
    pwHostApi->registry = pw_core_get_registry( pwHostApi->core, PW_VERSION_REGISTRY, 0 );
    if( !pwHostApi->registry )
    {
        pw_thread_loop_unlock( pwHostApi->loop );
        PA_DEBUG(( "PipeWire: could not get registry\n" ));
        result = paNoError;
        goto error_no_pipewire;
    }

    spa_zero( pwHostApi->registryListener );
    pw_registry_add_listener( pwHostApi->registry, &pwHostApi->registryListener,
                              &registryEvents, pwHostApi );

    /* Sync and wait for device enumeration to complete */
    pwHostApi->enumerationDone = 0;
    pwHostApi->syncSeq = pw_core_sync( pwHostApi->core, PW_ID_CORE, 0 );

    while( !pwHostApi->enumerationDone )
        pw_thread_loop_wait( pwHostApi->loop );

    pw_thread_loop_unlock( pwHostApi->loop );

    /* Build PortAudio device list from enumerated PipeWire devices */
    deviceCount = pwHostApi->enumDeviceCount;

    *hostApi = &pwHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paPipeWire;
    (*hostApi)->info.name = "PipeWire";
    (*hostApi)->info.deviceCount = 0;
    (*hostApi)->info.defaultInputDevice = paNoDevice;
    (*hostApi)->info.defaultOutputDevice = paNoDevice;

    if( deviceCount > 0 )
    {
        (*hostApi)->deviceInfos = (PaDeviceInfo **)PaUtil_GroupAllocateZeroInitializedMemory(
            pwHostApi->allocations, sizeof(PaDeviceInfo *) * deviceCount );
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        deviceInfoArray = (PaDeviceInfo *)PaUtil_GroupAllocateZeroInitializedMemory(
            pwHostApi->allocations, sizeof(PaDeviceInfo) * deviceCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i = 0; i < deviceCount; ++i )
        {
            PaDeviceInfo *deviceInfo = &deviceInfoArray[i];
            PaPipeWireDeviceInfo *enumDev = &pwHostApi->enumDevices[i];
            char *deviceName;

            deviceInfo->structVersion = 2;
            deviceInfo->hostApi = hostApiIndex;

            deviceName = (char *)PaUtil_GroupAllocateZeroInitializedMemory(
                pwHostApi->allocations, strlen( enumDev->nodeName ) + 1 );
            if( !deviceName )
            {
                result = paInsufficientMemory;
                goto error;
            }
            strcpy( deviceName, enumDev->nodeName );
            deviceInfo->name = deviceName;

            if( enumDev->isSource )
            {
                deviceInfo->maxInputChannels = enumDev->maxChannels;
                deviceInfo->maxOutputChannels = 0;
            }
            else
            {
                deviceInfo->maxInputChannels = 0;
                deviceInfo->maxOutputChannels = enumDev->maxChannels;
            }

            deviceInfo->defaultLowInputLatency = PA_PIPEWIRE_DEFAULT_LOW_LATENCY;
            deviceInfo->defaultLowOutputLatency = PA_PIPEWIRE_DEFAULT_LOW_LATENCY;
            deviceInfo->defaultHighInputLatency = PA_PIPEWIRE_DEFAULT_HIGH_LATENCY;
            deviceInfo->defaultHighOutputLatency = PA_PIPEWIRE_DEFAULT_HIGH_LATENCY;
            deviceInfo->defaultSampleRate = enumDev->defaultSampleRate;

            (*hostApi)->deviceInfos[i] = deviceInfo;
            ++(*hostApi)->info.deviceCount;

            /* Track default devices by priority */
            if( enumDev->isSource && enumDev->priority > highestInputPriority )
            {
                highestInputPriority = enumDev->priority;
                defaultInput = i;
            }
            if( !enumDev->isSource && enumDev->priority > highestOutputPriority )
            {
                highestOutputPriority = enumDev->priority;
                defaultOutput = i;
            }
        }

        (*hostApi)->info.defaultInputDevice = defaultInput;
        (*hostApi)->info.defaultOutputDevice = defaultOutput;
        PA_DEBUG(( "PipeWire defaults: input=%d output=%d\n", defaultInput, defaultOutput ));
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &pwHostApi->callbackStreamInterface,
        CloseStream, StartStream, StopStream, AbortStream,
        IsStreamStopped, IsStreamActive, GetStreamTime, GetStreamCpuLoad,
        PaUtil_DummyRead, PaUtil_DummyWrite,
        PaUtil_DummyGetReadAvailable, PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface( &pwHostApi->blockingStreamInterface,
        CloseStream, StartStream, StopStream, AbortStream,
        IsStreamStopped, IsStreamActive, GetStreamTime, PaUtil_DummyGetCpuLoad,
        ReadStream, WriteStream, GetStreamReadAvailable, GetStreamWriteAvailable );

    return result;

error_no_pipewire:
    /* PipeWire not available - clean up and return NULL hostApi gracefully */
    if( pwHostApi )
    {
        if( pwHostApi->loop )
        {
            pw_thread_loop_stop( pwHostApi->loop );

            if( pwHostApi->context )
                pw_context_destroy( pwHostApi->context );

            pw_thread_loop_destroy( pwHostApi->loop );
        }

        if( pwHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( pwHostApi->allocations );
            PaUtil_DestroyAllocationGroup( pwHostApi->allocations );
        }

        PaUtil_FreeMemory( pwHostApi );
    }

    *hostApi = NULL;
    return paNoError;

error:
    if( pwHostApi )
    {
        if( pwHostApi->loop )
        {
            pw_thread_loop_lock( pwHostApi->loop );
            if( pwHostApi->registry )
                pw_proxy_destroy( (struct pw_proxy *)pwHostApi->registry );
            if( pwHostApi->core )
                pw_core_disconnect( pwHostApi->core );
            pw_thread_loop_unlock( pwHostApi->loop );

            pw_thread_loop_stop( pwHostApi->loop );

            if( pwHostApi->context )
                pw_context_destroy( pwHostApi->context );

            pw_thread_loop_destroy( pwHostApi->loop );
        }

        if( pwHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( pwHostApi->allocations );
            PaUtil_DestroyAllocationGroup( pwHostApi->allocations );
        }

        PaUtil_FreeMemory( pwHostApi );
    }

    return result;
}


/* ---- Terminate ---- */

static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaPipeWireHostApiRepresentation *pwHostApi = (PaPipeWireHostApiRepresentation *)hostApi;
    int i;

    if( pwHostApi->loop )
    {
        pw_thread_loop_lock( pwHostApi->loop );

        if( pwHostApi->registry )
        {
            spa_hook_remove( &pwHostApi->registryListener );
            pw_proxy_destroy( (struct pw_proxy *)pwHostApi->registry );
        }

        if( pwHostApi->core )
        {
            spa_hook_remove( &pwHostApi->coreListener );
            pw_core_disconnect( pwHostApi->core );
        }

        pw_thread_loop_unlock( pwHostApi->loop );
        pw_thread_loop_stop( pwHostApi->loop );

        if( pwHostApi->context )
            pw_context_destroy( pwHostApi->context );

        pw_thread_loop_destroy( pwHostApi->loop );
    }

    /* Free device names that were allocated with PaUtil_StringDuplicate */
    for( i = 0; i < pwHostApi->enumDeviceCount; ++i )
    {
        if( pwHostApi->enumDevices[i].nodeName )
            free( pwHostApi->enumDevices[i].nodeName );
    }

    if( pwHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( pwHostApi->allocations );
        PaUtil_DestroyAllocationGroup( pwHostApi->allocations );
    }

    pw_deinit();

    PaUtil_FreeMemory( pwHostApi );

    PaPipeWire_UnloadLibrary();  /* no-op unless PA_PIPEWIRE_DYNAMIC */
}


/* ---- IsFormatSupported ---- */

static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;

    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        if( inputSampleFormat & paCustomFormat )
            return paSampleFormatNotSupported;

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        if( inputChannelCount > hostApi->deviceInfos[inputParameters->device]->maxInputChannels )
            return paInvalidChannelCount;

        if( inputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo;
    }
    else
    {
        inputChannelCount = 0;
    }

    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        if( outputSampleFormat & paCustomFormat )
            return paSampleFormatNotSupported;

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        if( outputChannelCount > hostApi->deviceInfos[outputParameters->device]->maxOutputChannels )
            return paInvalidChannelCount;

        if( outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo;
    }
    else
    {
        outputChannelCount = 0;
    }

    /* PipeWire handles sample rate conversion, so accept any reasonable rate */
    if( sampleRate <= 0 || sampleRate > 768000 )
        return paInvalidSampleRate;

    /* Suppress unused variable warnings */
    (void)inputSampleFormat;
    (void)outputSampleFormat;

    return paFormatIsSupported;
}


/* ---- OpenStream ---- */

static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData )
{
    PaError result = paNoError;
    PaPipeWireHostApiRepresentation *pwHostApi = (PaPipeWireHostApiRepresentation *)hostApi;
    PaPipeWireStream *stream = NULL;
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;

    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        if( inputChannelCount > hostApi->deviceInfos[inputParameters->device]->maxInputChannels )
            return paInvalidChannelCount;

        if( inputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo;

        hostInputSampleFormat = PaUtil_SelectClosestAvailableFormat( paFloat32, inputSampleFormat );
    }
    else
    {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = paFloat32;
    }

    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        if( outputChannelCount > hostApi->deviceInfos[outputParameters->device]->maxOutputChannels )
            return paInvalidChannelCount;

        if( outputParameters->hostApiSpecificStreamInfo )
            return paIncompatibleHostApiSpecificStreamInfo;

        hostOutputSampleFormat = PaUtil_SelectClosestAvailableFormat( paFloat32, outputSampleFormat );
    }
    else
    {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = paFloat32;
    }

    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag;

    stream = (PaPipeWireStream *)PaUtil_AllocateZeroInitializedMemory( sizeof(PaPipeWireStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    stream->hostApi = pwHostApi;
    stream->callbackMode = ( streamCallback != NULL );
    stream->isActive = 0;
    stream->isStopped = 1;
    stream->callbackAbort = 0;
    stream->inputChannelCount = inputChannelCount;
    stream->outputChannelCount = outputChannelCount;
    stream->sampleRate = sampleRate;

    /* Calculate frame sizes (float32 interleaved) */
    stream->inputFrameSize = inputChannelCount * sizeof(float);
    stream->outputFrameSize = outputChannelCount * sizeof(float);

    /* Store target node IDs */
    stream->inputNodeId = SPA_ID_INVALID;
    stream->outputNodeId = SPA_ID_INVALID;

    if( inputParameters )
    {
        int devIdx = inputParameters->device;
        if( devIdx >= 0 && devIdx < pwHostApi->enumDeviceCount )
            stream->inputNodeId = pwHostApi->enumDevices[devIdx].nodeId;
    }
    if( outputParameters )
    {
        int devIdx = outputParameters->device;
        if( devIdx >= 0 && devIdx < pwHostApi->enumDeviceCount )
            stream->outputNodeId = pwHostApi->enumDevices[devIdx].nodeId;
    }

    if( streamCallback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
            &pwHostApi->callbackStreamInterface, streamCallback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
            &pwHostApi->blockingStreamInterface, streamCallback, userData );
    }

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    result = PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
        inputChannelCount, inputSampleFormat, hostInputSampleFormat,
        outputChannelCount, outputSampleFormat, hostOutputSampleFormat,
        sampleRate, streamFlags, framesPerBuffer,
        0, paUtilUnknownHostBufferSize,
        streamCallback, userData );
    if( result != paNoError )
        goto error;

    stream->streamRepresentation.streamInfo.inputLatency =
        (PaTime)PaUtil_GetBufferProcessorInputLatencyFrames( &stream->bufferProcessor ) / sampleRate
        + PA_PIPEWIRE_DEFAULT_LOW_LATENCY;
    stream->streamRepresentation.streamInfo.outputLatency =
        (PaTime)PaUtil_GetBufferProcessorOutputLatencyFrames( &stream->bufferProcessor ) / sampleRate
        + PA_PIPEWIRE_DEFAULT_LOW_LATENCY;
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    /* Set up ring buffers.
       Input ring is needed for both blocking mode and full-duplex callback mode
       (to decouple PipeWire's independent input/output stream timing).
       Output ring is only needed for blocking mode. */
    {
        int needInputRing = ( inputChannelCount > 0 ) &&
            ( !streamCallback || outputChannelCount > 0 );  /* blocking OR full-duplex */
        int needOutputRing = ( outputChannelCount > 0 ) && !streamCallback;  /* blocking only */

        if( needInputRing || needOutputRing )
        {
            unsigned long ringFrames = RoundUpPow2(
                (unsigned long)(sampleRate * PA_PIPEWIRE_DEFAULT_HIGH_LATENCY * PA_PIPEWIRE_RING_FRAMES_FACTOR) );
            if( ringFrames < 4096 )
                ringFrames = 4096;

            if( needOutputRing )
            {
                stream->outputRingData = PaUtil_AllocateZeroInitializedMemory(
                    ringFrames * stream->outputFrameSize );
                if( !stream->outputRingData )
                {
                    result = paInsufficientMemory;
                    goto error;
                }
                PaUtil_InitializeRingBuffer( &stream->outputRing,
                    stream->outputFrameSize, ringFrames, stream->outputRingData );
            }

            if( needInputRing )
            {
                stream->inputRingData = PaUtil_AllocateZeroInitializedMemory(
                    ringFrames * stream->inputFrameSize );
                if( !stream->inputRingData )
                {
                    result = paInsufficientMemory;
                    goto error;
                }
                PaUtil_InitializeRingBuffer( &stream->inputRing,
                    stream->inputFrameSize, ringFrames, stream->inputRingData );
            }
        }
    }

    *s = (PaStream *)stream;
    return result;

error:
    if( stream )
    {
        if( stream->outputRingData )
            PaUtil_FreeMemory( stream->outputRingData );
        if( stream->inputRingData )
            PaUtil_FreeMemory( stream->inputRingData );
        PaUtil_FreeMemory( stream );
    }
    return result;
}


/* ---- CloseStream ---- */

static PaError CloseStream( PaStream* s )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;

    pw_thread_loop_lock( stream->hostApi->loop );

    if( stream->outputStream )
    {
        pw_stream_destroy( stream->outputStream );
        stream->outputStream = NULL;
    }
    if( stream->inputStream )
    {
        pw_stream_destroy( stream->inputStream );
        stream->inputStream = NULL;
    }

    pw_thread_loop_unlock( stream->hostApi->loop );

    PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
    PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );

    if( stream->outputRingData )
        PaUtil_FreeMemory( stream->outputRingData );
    if( stream->inputRingData )
        PaUtil_FreeMemory( stream->inputRingData );

    PaUtil_FreeMemory( stream );

    return paNoError;
}


/* ---- StartStream ---- */

static PaError StartStream( PaStream *s )
{
    PaError result = paNoError;
    PaPipeWireStream *stream = (PaPipeWireStream *)s;

    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );

    stream->isActive = 1;
    stream->isStopped = 0;
    stream->callbackAbort = 0;

    /* Flush ring buffers */
    PaUtil_FlushRingBuffer( &stream->outputRing );
    PaUtil_FlushRingBuffer( &stream->inputRing );

    pw_thread_loop_lock( stream->hostApi->loop );

    if( stream->outputChannelCount > 0 )
    {
        result = CreateAndConnectStream( stream, &stream->outputStream,
            &stream->outputListener, &outputStreamEvents,
            SPA_DIRECTION_OUTPUT, stream->outputChannelCount,
            stream->sampleRate, stream->outputNodeId );
        if( result != paNoError )
        {
            pw_thread_loop_unlock( stream->hostApi->loop );
            goto error;
        }
    }

    if( stream->inputChannelCount > 0 )
    {
        result = CreateAndConnectStream( stream, &stream->inputStream,
            &stream->inputListener, &inputStreamEvents,
            SPA_DIRECTION_INPUT, stream->inputChannelCount,
            stream->sampleRate, stream->inputNodeId );
        if( result != paNoError )
        {
            pw_thread_loop_unlock( stream->hostApi->loop );
            goto error;
        }
    }

    pw_thread_loop_unlock( stream->hostApi->loop );

    return paNoError;

error:
    stream->isActive = 0;
    stream->isStopped = 1;

    pw_thread_loop_lock( stream->hostApi->loop );
    if( stream->outputStream )
    {
        pw_stream_destroy( stream->outputStream );
        stream->outputStream = NULL;
    }
    if( stream->inputStream )
    {
        pw_stream_destroy( stream->inputStream );
        stream->inputStream = NULL;
    }
    pw_thread_loop_unlock( stream->hostApi->loop );

    return result;
}


/* ---- StopStream ---- */

static PaError StopStream( PaStream *s )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;

    stream->isActive = 0;

    pw_thread_loop_lock( stream->hostApi->loop );

    if( stream->outputStream )
    {
        pw_stream_disconnect( stream->outputStream );
        pw_stream_destroy( stream->outputStream );
        stream->outputStream = NULL;
    }
    if( stream->inputStream )
    {
        pw_stream_disconnect( stream->inputStream );
        pw_stream_destroy( stream->inputStream );
        stream->inputStream = NULL;
    }

    pw_thread_loop_unlock( stream->hostApi->loop );

    stream->isStopped = 1;

    if( stream->streamRepresentation.streamFinishedCallback )
        stream->streamRepresentation.streamFinishedCallback(
            stream->streamRepresentation.userData );

    return paNoError;
}


/* ---- AbortStream ---- */

static PaError AbortStream( PaStream *s )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;

    stream->callbackAbort = 1;
    return StopStream( s );
}


/* ---- IsStreamStopped ---- */

static PaError IsStreamStopped( PaStream *s )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;
    return stream->isStopped;
}


/* ---- IsStreamActive ---- */

static PaError IsStreamActive( PaStream *s )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;
    return stream->isActive;
}


/* ---- GetStreamTime ---- */

static PaTime GetStreamTime( PaStream *s )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;
    struct pw_time pwTime;

    if( stream->outputStream )
    {
        if( pw_stream_get_time_n( stream->outputStream, &pwTime, sizeof(pwTime) ) == 0 )
            return (PaTime)pwTime.now / 1000000000.0;
    }
    else if( stream->inputStream )
    {
        if( pw_stream_get_time_n( stream->inputStream, &pwTime, sizeof(pwTime) ) == 0 )
            return (PaTime)pwTime.now / 1000000000.0;
    }

    return 0;
}


/* ---- GetStreamCpuLoad ---- */

static double GetStreamCpuLoad( PaStream* s )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;
    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}


/* ---- Blocking mode ---- */

static PaError ReadStream( PaStream* s,
                           void *buffer,
                           unsigned long frames )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;
    unsigned long framesRead = 0;
    char *dst = (char *)buffer;

    pw_thread_loop_lock( stream->hostApi->loop );

    while( framesRead < frames )
    {
        ring_buffer_size_t available = PaUtil_GetRingBufferReadAvailable( &stream->inputRing );

        if( available > 0 )
        {
            ring_buffer_size_t toRead = (ring_buffer_size_t)(frames - framesRead);
            if( toRead > available )
                toRead = available;

            PaUtil_ReadRingBuffer( &stream->inputRing,
                dst + framesRead * stream->inputFrameSize, toRead );
            framesRead += toRead;
        }
        else
        {
            if( !stream->isActive )
            {
                pw_thread_loop_unlock( stream->hostApi->loop );
                return paStreamIsStopped;
            }
            pw_thread_loop_wait( stream->hostApi->loop );
        }
    }

    pw_thread_loop_unlock( stream->hostApi->loop );

    return paNoError;
}


static PaError WriteStream( PaStream* s,
                            const void *buffer,
                            unsigned long frames )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;
    unsigned long framesWritten = 0;
    const char *src = (const char *)buffer;

    pw_thread_loop_lock( stream->hostApi->loop );

    while( framesWritten < frames )
    {
        ring_buffer_size_t space = PaUtil_GetRingBufferWriteAvailable( &stream->outputRing );

        if( space > 0 )
        {
            ring_buffer_size_t toWrite = (ring_buffer_size_t)(frames - framesWritten);
            if( toWrite > space )
                toWrite = space;

            PaUtil_WriteRingBuffer( &stream->outputRing,
                src + framesWritten * stream->outputFrameSize, toWrite );
            framesWritten += toWrite;
        }
        else
        {
            if( !stream->isActive )
            {
                pw_thread_loop_unlock( stream->hostApi->loop );
                return paStreamIsStopped;
            }
            pw_thread_loop_wait( stream->hostApi->loop );
        }
    }

    pw_thread_loop_unlock( stream->hostApi->loop );

    return paNoError;
}


static signed long GetStreamReadAvailable( PaStream* s )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;
    return (signed long)PaUtil_GetRingBufferReadAvailable( &stream->inputRing );
}


static signed long GetStreamWriteAvailable( PaStream* s )
{
    PaPipeWireStream *stream = (PaPipeWireStream *)s;
    return (signed long)PaUtil_GetRingBufferWriteAvailable( &stream->outputRing );
}
