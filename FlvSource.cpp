#include "FlvSource.h"
#include <wmcodecdsp.h>
#include "MFState.hpp"
#include "avcc.hpp"
#include "prop_variant.hpp"
#include "flvstream.h"
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")      // Media Foundation GUIDs
#pragma comment(lib, "strmiids")    // DirectShow GUIDs
#pragma comment(lib, "Ws2_32")      // htonl, etc
#pragma comment(lib, "shlwapi")
#pragma comment(lib,"wmcodecdspuuid")


HRESULT CreateVideoMediaType(const flv_file_header& , IMFMediaType **ppType);
HRESULT CreateAudioMediaType(const flv_file_header& , IMFMediaType **ppType);
HRESULT GetStreamMajorType(IMFStreamDescriptor *pSD, GUID *pguidMajorType);
HRESULT NewMFMediaBuffer(const uint8_t*data, uint32_t length, IMFMediaBuffer **rtn);
HRESULT NewNaluBuffer(uint8_t nallength, packet const&nalu, IMFMediaBuffer **rtn);
IMFMediaStreamExtPtr to_stream_ext(IMFMediaStreamPtr &);

struct scope_lock {
  FlvSource* pthis;
  explicit scope_lock(FlvSource* pt) : pthis(pt) { pthis->Lock(); }
  void unlock() { pthis->Unlock(); pthis = nullptr; }
  ~scope_lock() { if(pthis)pthis->Unlock(); }
};
//-------------------------------------------------------------------
// IMFMediaEventGenerator methods
//
// All of the IMFMediaEventGenerator methods do the following:
// 1. Check for shutdown status.
// 2. Call the event queue helper object.
//-------------------------------------------------------------------

HRESULT FlvSource::BeginGetEvent(IMFAsyncCallback* pCallback,IUnknown* punkState)
{
    HRESULT hr = S_OK;

    scope_lock l(this);

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
      hr = event_queue->BeginGetEvent(pCallback, punkState);
    }

    return hr;
}

HRESULT FlvSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    HRESULT hr = S_OK;

    scope_lock l(this);

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
      hr = event_queue->EndGetEvent(pResult, ppEvent);
    }

    return hr;
}

HRESULT FlvSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    IMFMediaEventQueuePtr pQueue;

    scope_lock l(this);

    // Check shutdown
    HRESULT hr = CheckShutdown();

    // Cache a local pointer to the queue.
    if (SUCCEEDED(hr))
    {
      pQueue = event_queue;
    }

    l.unlock();

    // Use the local pointer to call GetEvent.
    if (SUCCEEDED(hr))
    {
        hr = pQueue->GetEvent(dwFlags, ppEvent);
    }

    return hr;
}

HRESULT FlvSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    HRESULT hr = S_OK;

    scope_lock l(this);

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
      hr = event_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
    }


    return hr;
}

//-------------------------------------------------------------------
// CreatePresentationDescriptor
// Returns a shallow copy of the source's presentation descriptor.
//-------------------------------------------------------------------

HRESULT FlvSource::CreatePresentationDescriptor(    IMFPresentationDescriptor** pppd    )
{
  if (pppd == NULL)
    {
        return E_POINTER;
    }

    scope_lock l(this);

    // Fail if the source is shut down.
    HRESULT hr = CheckShutdown();

    // Fail if the source was not initialized yet.
    if (SUCCEEDED(hr))
    {
        hr = IsInitialized();
    }
    if (ok(hr)) {
    // Do we have a valid presentation descriptor?
      if (presentation_descriptor)
        hr = presentation_descriptor->Clone(pppd);
      else hr = MF_E_NOT_INITIALIZED;
    }
    return hr;
}


//-------------------------------------------------------------------
// GetCharacteristics
// Returns capabilities flags.
//-------------------------------------------------------------------

HRESULT FlvSource::GetCharacteristics(DWORD* pdwCharacteristics)
{
    if (pdwCharacteristics == NULL)
    {
        return E_POINTER;
    }

    HRESULT hr = S_OK;

    scope_lock l(this);

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
      *pdwCharacteristics = MFMEDIASOURCE_CAN_PAUSE |
        MFMEDIASOURCE_CAN_SEEK |
        MFMEDIASOURCE_HAS_SLOW_SEEK |
        MFMEDIASOURCE_CAN_SKIPFORWARD |
        MFMEDIASOURCE_CAN_SKIPBACKWARD;
    }

    return hr;
}


//-------------------------------------------------------------------
// Pause
// Pauses the source.
//-------------------------------------------------------------------

HRESULT FlvSource::Pause() {
  scope_lock l(this);

  HRESULT hr = S_OK;

  // Fail if the source is shut down.
  hr = CheckShutdown();

  // Queue the operation.
  if (SUCCEEDED(hr)) {
    hr = AsyncPause();// QueueAsyncOperation(SourceOp::OP_PAUSE);
  }

  return hr;
}

//-------------------------------------------------------------------
// Shutdown
// Shuts down the source and releases all resources.
//-------------------------------------------------------------------

HRESULT FlvSource::Shutdown() {
  scope_lock l(this);

  HRESULT hr = S_OK;

  hr = CheckShutdown();
  if (fail(hr))
    return hr;
  // Shut down the stream objects.
  if (audio_stream) {
    auto as = to_stream_ext(audio_stream);// static_cast<FlvStream*>(audio_stream.Get());
    as->Shutdown();
  }
  if (video_stream) {
    auto vs = to_stream_ext(video_stream);// static_cast<FlvStream*>(video_stream.Get());
    vs->Shutdown();
  }
  // Shut down the event queue.
  if (event_queue) {
    (void)event_queue->Shutdown();
  }

  // Release objects.
  event_queue = nullptr;
  presentation_descriptor = nullptr;
  begin_open_caller_result = nullptr;
  byte_stream = nullptr;

  // Set the state.
  m_state = SourceState::STATE_SHUTDOWN;

  return hr;
}

//-------------------------------------------------------------------
// Start
// Starts or seeks the media source.
//-------------------------------------------------------------------

HRESULT FlvSource::Start(
  IMFPresentationDescriptor* pPresentationDescriptor,
  const GUID* pguidTimeFormat,
  const PROPVARIANT* pvarStartPos
  ) {
    // Check parameters.

    // Start position and presentation descriptor cannot be NULL.
    if (pvarStartPos == NULL || pPresentationDescriptor == NULL) {
      return E_INVALIDARG;
    }

    // Check the time format.
    if ((pguidTimeFormat != NULL) && (*pguidTimeFormat != GUID_NULL)) {
      // Unrecognized time format GUID.
      return MF_E_UNSUPPORTED_TIME_FORMAT;
    }

    // only accept nano timestamp
    // Check the data type of the start position.
    if ((pvarStartPos->vt != VT_I8) && (pvarStartPos->vt != VT_EMPTY)) {
      return MF_E_UNSUPPORTED_TIME_FORMAT;
    }

    scope_lock l(this);

    // Fail if the source is shut down.
    auto hr = CheckShutdown();
    // Fail if the source was not initialized yet.
    if (ok(hr)) hr = IsInitialized();
    // Perform a sanity check on the caller's presentation descriptor.
    if (ok(hr)) hr = ValidatePresentationDescriptor(pPresentationDescriptor);

    // The operation looks OK. Complete the operation asynchronously.
    if (ok(hr)) hr = AsyncStart(pPresentationDescriptor, pvarStartPos);
    return hr;
}

HRESULT FlvSource::AsyncStart(IMFPresentationDescriptor* pd, PROPVARIANT const*startpos){
  IMFPresentationDescriptorPtr spd(pd);
  _prop_variant_t spos = startpos;
  return AsyncDo(MFAsyncCallback::New([spd, spos, this](IMFAsyncResult*result)->HRESULT{
    scope_lock l(this);
    auto hr = this->DoStart(spd.Get(), &spos);
    result->SetStatus(hr);
    return S_OK;
  }).Get(), static_cast<IMFMediaSource*>(this));  // state add this's ref
}

//-------------------------------------------------------------------
// Stop
// Stops the media source.
//-------------------------------------------------------------------

HRESULT FlvSource::Stop() {
  scope_lock l(this);

  // Fail if the source is shut down.
  HRESULT hr = CheckShutdown();

  // Fail if the source was not initialized yet.
  if (ok(hr))    hr = IsInitialized();

  // Queue the operation.
  if (ok(hr))    hr = AsyncStop();// QueueAsyncOperation(SourceOp::OP_STOP);

  return hr;
}


//-------------------------------------------------------------------
// BeginOpen
// Begins reading the byte stream to initialize the source.
// Called by the byte-stream handler when it creates the source.
//
// This method is asynchronous. When the operation completes,
// the callback is invoked and the byte-stream handler calls
// EndOpen.
//
// pStream: Pointer to the byte stream for the flv stream.
// pCB: Pointer to the byte-stream handler's callback.
// pState: State object for the async callback. (Can be NULL.)
//
// Note: The source reads enough data to find one packet header
// for each audio or video stream. This enables the source to
// create a presentation descriptor that describes the format of
// each stream. The source queues the packets that it reads during
// BeginOpen.
//-------------------------------------------------------------------

HRESULT FlvSource::BeginOpen(IMFByteStream *pStream, IMFAsyncCallback *pCB, IUnknown *pState)
{
    if (pStream == NULL || pCB == NULL)
    {
        return E_POINTER;
    }

    if (m_state != SourceState::STATE_INVALID)
    {
        return MF_E_INVALIDREQUEST;
    }


    scope_lock l(this);

    // Cache the byte-stream pointer.
    byte_stream = pStream;
    ZeroMemory(&status, sizeof(status));  // reset all status flags
    // todo: do other initializations here

    // Validate the capabilities of the byte stream.
    // The byte stream must be readable and seekable.
    DWORD dwCaps = 0;
    HRESULT hr = pStream->GetCapabilities(&dwCaps);

    if (SUCCEEDED(hr)) {
      if ((dwCaps & MFBYTESTREAM_IS_SEEKABLE) == 0) {
        hr = MF_E_BYTESTREAM_NOT_SEEKABLE;
      } else if ((dwCaps & MFBYTESTREAM_IS_READABLE) == 0) {
        hr = E_FAIL;
      }
    }

    // Create an async result object. We'll use it later to invoke the callback.
    if (SUCCEEDED(hr)) {
      hr = MFCreateAsyncResult(NULL, pCB, pState, &begin_open_caller_result);
    }

    // Start reading data from the stream.
    if (SUCCEEDED(hr)) {
      hr = ReadFlvHeader();
    }

    // At this point, we now guarantee to invoke the callback.
    if (SUCCEEDED(hr)) {
      m_state = SourceState::STATE_OPENING;
    } else {
      byte_stream = nullptr;  // reset byte_stream
    }

    return hr;
}

HRESULT FlvSource::ReadFlvHeader() {
  auto hr = parser.begin_flv_header(byte_stream, &on_flv_header, nullptr);
  return hr;
}

HRESULT FlvSource::OnFlvHeader(IMFAsyncResult *result){
  flv_header v;
  auto hr = parser.end_flv_header(result, &v);
  scope_lock l(this);
  if (FAILED(hr)) { 
    StreamingError(hr);
  } else {
    header.status.file_header_ready = 1;
    ReadFlvTagHeader();
  }
  return S_OK;
}
HRESULT FlvSource::ReadFlvTagHeader() {
  auto hr = parser.begin_tag_header(1, &on_tag_header, nullptr);
  return hr;
}
HRESULT FlvSource::OnFlvTagHeader(IMFAsyncResult *result){
  tag_header tagh;
  auto hr = parser.end_tag_header(result, &tagh);
  scope_lock l(this);
  if (FAILED(hr)) {
    StreamingError(hr);
    return S_OK;
  }
  if (tagh.type == flv::tag_type::script_data && !status.on_meta_data_ready){
    header.status.has_script_data = 1;
    ReadMetaData(tagh.data_size);
  }
  else if (tagh.type == flv::tag_type::video ){
    header.status.has_video = 1;
    if (!header.first_media_tag_offset)
      header.first_media_tag_offset = tagh.data_offset - flv::flv_tag_header_length;
    if(status.first_video_tag_ready)
      SeekToNextTag(tagh);
    else ReadVideoHeader(tagh);
  }
  else if (tagh.type == flv::tag_type::audio){
    header.status.has_audio = 1;
    if (!header.first_media_tag_offset)
      header.first_media_tag_offset = tagh.data_offset - flv::flv_tag_header_length;
    if (status.first_audio_tag_ready)
      SeekToNextTag(tagh);
    else ReadAudioHeader(tagh);
  }
  else if (tagh.type == flv::tag_type::eof){  // first round scan done
    header.status.scan_once = 1;
    StreamingError(MF_E_INVALID_FILE_FORMAT);
  }
  else {
    SeekToNextTag(tagh);// ignore unknown tags
  }
  return S_OK;
}

HRESULT FlvSource::SeekToNextTag(tag_header const&tagh) {
  //return // parser.begin_seek(tagh.data_size, 1, &on_seek_to_tag_begin, nullptr);
  QWORD pos = 0;
  return byte_stream->Seek(MFBYTESTREAM_SEEK_ORIGIN::msoCurrent, tagh.data_size, MFBYTESTREAM_SEEK_FLAG_CANCEL_PENDING_IO, &pos);
}

HRESULT FlvSource::ReadMetaData(uint32_t meta_size) {
  return parser.begin_on_meta_data(meta_size, &on_meta_data, nullptr);
}

HRESULT FlvSource::OnMetaData(IMFAsyncResult *result){
  auto hr = parser.end_on_meta_data(result, &header);
  scope_lock l(this);
  if (FAILED(hr)){
    StreamingError(hr);
    return S_OK;
  }
  status.on_meta_data_ready = 1;
  header.status.meta_ready = 1;
  if (header.audiocodecid != flv::audio_codec::aac && header.videocodecid != flv::video_codec::avc)
    FinishInitialize();
  else  ReadFlvTagHeader();
  return S_OK;
}
//-------------------------------------------------------------------
// EndOpen
// Completes the BeginOpen operation.
// Called by the byte-stream handler when it creates the source.
//-------------------------------------------------------------------

HRESULT FlvSource::EndOpen(IMFAsyncResult *pResult) {
  scope_lock l(this);

  HRESULT hr = pResult->GetStatus();

  if (FAILED(hr)) {
    // The source is not designed to recover after failing to open.
    // Switch to shut-down state.
    Shutdown();
  }
  return hr;
}

#pragma warning( push )
#pragma warning( disable : 4355 )  // 'this' used in base member initializer list

FlvSource::FlvSource() :
    on_flv_header(this, &FlvSource::OnFlvHeader),
    on_tag_header(this, &FlvSource::OnFlvTagHeader),
    on_meta_data(this, &FlvSource::OnMetaData),
    on_demux_sample_header(this, &FlvSource::OnSampleHeader),
    on_audio_header(this, &FlvSource::OnAudioHeader),
    on_aac_packet_type(this, &FlvSource::OnAacPacketType),
    on_audio_data(this, &FlvSource::OnAudioData),
    on_avc_packet_type(this, &FlvSource::OnAvcPacketType),
    on_video_data(this, &FlvSource::OnVideoData),
    on_video_header(this, &FlvSource::OnVideoHeader)
{
  ZeroMemory(&status, sizeof(status));

  InitializeCriticalSection(&crit_sec);
}
#pragma warning( pop )

HRESULT FlvSource::RuntimeClassInitialize(){
  return MFCreateEventQueue(&event_queue);
}
FlvSource::~FlvSource()
{
  if (m_state != SourceState::STATE_SHUTDOWN)
    {
        Shutdown();
    }

  DeleteCriticalSection(&crit_sec);
}


//-------------------------------------------------------------------
// CompleteOpen
//
// Completes the asynchronous BeginOpen operation.
//
// hrStatus: Status of the BeginOpen operation.
//-------------------------------------------------------------------

HRESULT FlvSource::CompleteOpen(HRESULT hrStatus)
{
    HRESULT hr = S_OK;
    assert(begin_open_caller_result);
    hr = begin_open_caller_result->SetStatus(hrStatus);
    hr = MFInvokeCallback(begin_open_caller_result.Get());

    begin_open_caller_result = nullptr;
    return hr;
}


//-------------------------------------------------------------------
// IsInitialized:
// Returns S_OK if the source is correctly initialized with an
// MPEG-1 byte stream. Otherwise, returns MF_E_NOT_INITIALIZED.
//-------------------------------------------------------------------

HRESULT FlvSource::IsInitialized() const
{// needn't lock and unlock
  if (m_state == SourceState::STATE_OPENING || m_state == SourceState::STATE_INVALID)
    {
        return MF_E_NOT_INITIALIZED;
    }
    else
    {
        return S_OK;
    }
}

template<typename I>
void release(I**i){
  if (*i)
    (*i)->Release();
  *i = nullptr;
}
//-------------------------------------------------------------------
// InitPresentationDescriptor
//
// Creates the source's presentation descriptor, if possible.
//
// During the BeginOpen operation, the source reads packets looking
// for headers for each stream. This enables the source to create the
// presentation descriptor, which describes the stream formats.
//
// This method tests whether the source has seen enough packets
// to create the PD. If so, it invokes the callback to complete
// the BeginOpen operation.
//-------------------------------------------------------------------

HRESULT FlvSource::InitPresentationDescriptor()
{
    HRESULT hr = S_OK;

    assert(!presentation_descriptor);
    assert(m_state == SourceState::STATE_OPENING);
    DWORD cStreams = 0;
    if (video_stream)
      ++cStreams;
    if (audio_stream)
      ++cStreams;
    // Ready to create the presentation descriptor.

    // Create an array of IMFStreamDescriptor pointers.
    IMFStreamDescriptor **ppSD =
            new (std::nothrow) IMFStreamDescriptor*[cStreams];

    ZeroMemory(ppSD, cStreams * sizeof(IMFStreamDescriptor*));

    cStreams = 0;
    if (video_stream)
      video_stream->GetStreamDescriptor(&ppSD[cStreams++]);
    if (audio_stream)
      audio_stream->GetStreamDescriptor(&ppSD[cStreams++]);

    // Create the presentation descriptor.
    hr = MFCreatePresentationDescriptor(cStreams, ppSD,      &presentation_descriptor);
    if (ok(hr))
      hr = presentation_descriptor->SetUINT64(MF_PD_DURATION, header.duration * 10000000ull);  // seconds to 100 nano
    if (ok(hr))
      hr = presentation_descriptor->SetUINT32(MF_PD_AUDIO_ENCODING_BITRATE, header.audiodatarate);
    if (ok(hr))
      hr = presentation_descriptor->SetUINT32(MF_PD_VIDEO_ENCODING_BITRATE, header.videodatarate);
    if (ok(hr))
      hr = presentation_descriptor->SetUINT64(MF_PD_TOTAL_FILE_SIZE, header.filesize);

    if (FAILED(hr))
    {
        goto done;
    }

    // Select the first video stream (if any).
    for (DWORD i = 0; i < cStreams; i++)
    {
      hr = presentation_descriptor->SelectStream(i);
    }

    // Switch state from "opening" to stopped.
    m_state = SourceState::STATE_STOPPED;

    // Invoke the async callback to complete the BeginOpen operation.
    hr = CompleteOpen(S_OK);

done:
    // clean up:
    if (ppSD)
    {
        for (DWORD i = 0; i < cStreams; i++)
        {
          release(&ppSD[i]);
        }
        delete [] ppSD;
    }
    return hr;
}


//-------------------------------------------------------------------
// QueueAsyncOperation
// Queue an asynchronous operation.
//
// OpType: Type of operation to queue.
//
// Note: If the SourceOp object requires additional information, call
// OpQueue<SourceOp>::QueueOperation, which takes a SourceOp pointer.
//-------------------------------------------------------------------

/*
HRESULT FlvSource::QueueAsyncOperation(SourceOp::Operation OpType)
{
    HRESULT hr = S_OK;
    SourceOp *pOp = NULL;

    hr = SourceOp::CreateOp(OpType, &pOp);

    if (SUCCEEDED(hr))
    {
        hr = QueueOperation(pOp);
    }

    SafeRelease(&pOp);
    return hr;
}
*/
//-------------------------------------------------------------------
// BeginAsyncOp
//
// Starts an asynchronous operation. Called by the source at the
// begining of any asynchronous operation.
//-------------------------------------------------------------------

void FlvSource::enter_op()
{
  status.processing_op = 1;
}

//-------------------------------------------------------------------
// CompleteAsyncOp
//
// Completes an asynchronous operation. Called by the source at the
// end of any asynchronous operation.
//-------------------------------------------------------------------

void FlvSource::leave_op()
{
    assert(status.processing_op);
    status.processing_op = 0;
}

//-------------------------------------------------------------------
// ValidateOperation
//
// Checks whether the source can perform the operation indicated
// by pOp at this time.
//
// If the source cannot perform the operation now, the method
// returns MF_E_NOTACCEPTING.
//
// NOTE:
// Implements the pure-virtual OpQueue::ValidateOperation method.
//-------------------------------------------------------------------

HRESULT FlvSource::ValidateOperation()
{
  return (status.processing_op) ? MF_E_NOTACCEPTING : S_OK;
}



//-------------------------------------------------------------------
// DoStart
// Perform an async start operation (IMFMediaSource::Start)
//
// pOp: Contains the start parameters.
//
// Note: This sample currently does not implement seeking, and the
// Start() method fails if the caller requests a seek.
//-------------------------------------------------------------------

HRESULT FlvSource::DoStart(IMFPresentationDescriptor*pd, PROPVARIANT const*startpos)
{
  auto hr = ValidateOperation();
  assert(ok(hr));  // overlapped operations arenot permitted
  enter_op();

  //startpos->vt == vt_empty) current pos
  bool isseek = false;
  bool restart = false;
  keyframe k;
  if (startpos->vt == VT_I8){
    k = header.keyframes.seek(startpos->hVal.QuadPart);
    pending_seek_file_position = k.position - flv::flv_previous_tag_size_field_length;  // - previous_tag_size
    status.pending_seek = 1;
    if (m_state != SourceState::STATE_STOPPED)
      isseek = true;
  } else if (startpos->vt == VT_EMPTY) {
    if (m_state == SourceState::STATE_STOPPED) {
      pending_seek_file_position = header.first_media_tag_offset - flv::flv_previous_tag_size_field_length;
      status.pending_seek = 1;
      k.position = header.first_media_tag_offset;
      k.time = 0;
    } else {
      k = current_keyframe;
      restart = true;
    }      
  }
  _prop_variant_t actual_pos(k.time);
    // Select/deselect streams, based on what the caller set in the PD.
    // This method also sends the MENewStream/MEUpdatedStream events.
    hr = SelectStreams(pd, k.time, isseek);

    if (ok(hr) && isseek) {
      hr = QueueEvent(MESourceSeeked, GUID_NULL, hr, &actual_pos);
    }else if (SUCCEEDED(hr))
    {
      m_state = SourceState::STATE_STARTED;
      IMFMediaEventPtr evt;
      hr = MFCreateMediaEvent(MESourceStarted, GUID_NULL, hr, &actual_pos, &evt);
      if (ok(hr)) {
        hr = evt->SetUINT64(MF_EVENT_SOURCE_ACTUAL_START, k.time);
      }
      if (ok(hr)) hr = event_queue->QueueEvent(evt.Get());

        // Queue the "started" event. The event data is the start position.
      /*hr = event_queue->QueueEventParamVar(
            MESourceStarted,
            GUID_NULL,
            S_OK,
            startpos
            );
      */
    }
    if (ok(hr) && audio_stream) hr = to_stream_ext(audio_stream)->Start(k.time, isseek);
    if (ok(hr) && video_stream) hr = to_stream_ext(video_stream)->Start(k.time, isseek);

    if (FAILED(hr))
    {
        // Failure. Send the error code to the application.

        // Note: It's possible that QueueEvent itself failed, in which case it
        // is likely to fail again. But there is no good way to recover in
        // that case.

      (void)event_queue->QueueEventParamVar(
            MESourceStarted, GUID_NULL, hr, NULL);
    }

    leave_op();

    return hr;
}

//-------------------------------------------------------------------
// SelectStreams
// Called during START operations to select and deselect streams.
// This method also sends the MENewStream/MEUpdatedStream events.
//-------------------------------------------------------------------
HRESULT     FlvSource::SelectStreams(IMFPresentationDescriptor *pPD, uint64_t nanosec, bool isseek){
  HRESULT hr = S_OK;

  // Reset the pending EOS count.
  pending_eos = 0;
  DWORD stream_count = 0;
  hr = pPD->GetStreamDescriptorCount(&stream_count);

  // Loop throught the stream descriptors to find which streams are active.
  for (DWORD i = 0; i < stream_count; ++i)
  {
    IMFStreamDescriptorPtr pSD;
    BOOL    selected = FALSE;
    DWORD stream_id = 0 - 1ul;
    hr = pPD->GetStreamDescriptorByIndex(i, &selected, &pSD);
    if (ok(hr)) hr = pSD->GetStreamIdentifier(&stream_id);
    IMFMediaStreamPtr stream;
    if (ok(hr)) {
      if (stream_id == 1){
        stream = audio_stream;
      }
      else if (stream_id == 0){
        stream = video_stream;
      }
      else hr = E_INVALIDARG;
    }
    if (fail(hr))
    {
      return hr;
    }
    auto flv_stream = to_stream_ext(stream);// static_cast<FlvStream*>(stream.Get());
    // Was the stream active already?
    auto was_selected = flv_stream->IsActived() == S_OK;
    // Activate or deactivate the stream.
    hr = flv_stream->Active(selected);

    if (ok(hr) && selected)
    {
      ++pending_eos;

      // If the stream was previously selected, send an "updated stream"
      // event. Otherwise, send a "new stream" event.
      MediaEventType met = was_selected ? MEUpdatedStream : MENewStream;
      if(ok(hr)) hr = event_queue->QueueEventParamUnk(met, GUID_NULL, hr, stream.Get());

      // Start the stream. The stream will send the appropriate event.
//      if(ok(hr)) hr = flv_stream->Start(nanosec, isseek);
    }
    else if (ok(hr) && was_selected) {
      (void)flv_stream->Shutdown();  // ignore result
    }
  }
  return hr;
}

HRESULT FlvSource::AsyncStop(){
  return AsyncDo(MFAsyncCallback::New([this](IMFAsyncResult*result)->HRESULT{
    scope_lock l(this);
    auto hr = this->DoStop();
    result->SetStatus(hr);
    return S_OK;
  }).Get(), static_cast<IMFMediaSource*>(this));
}
//-------------------------------------------------------------------
// DoStop
// Perform an async stop operation (IMFMediaSource::Stop)
//-------------------------------------------------------------------

HRESULT FlvSource::DoStop()
{
  enter_op();
  HRESULT hr = S_OK;
  // Stop the active streams.
  if (audio_stream) hr = to_stream_ext(audio_stream)->Stop();
  if (video_stream) hr = to_stream_ext(video_stream)->Stop();

  // Increment the counter that tracks "stale" read requests.
  ++restart_counter; // This counter is allowed to overflow.

  m_state = SourceState::STATE_STOPPED;

  // Send the "stopped" event. This might include a failure code.
  (void)event_queue->QueueEventParamVar(MESourceStopped, GUID_NULL, hr, NULL);

  leave_op();

  return hr;
}

HRESULT FlvSource::AsyncPause(){
  return AsyncDo(MFAsyncCallback::New([this](IMFAsyncResult*result)->HRESULT{
    scope_lock l(this);
    auto hr = this->DoPause();
    result->SetStatus(hr);
    return S_OK;
  }).Get(), static_cast<IMFMediaSource*>(this));
}

//-------------------------------------------------------------------
// DoPause
// Perform an async pause operation (IMFMediaSource::Pause)
//-------------------------------------------------------------------

HRESULT FlvSource::DoPause()
{
  HRESULT hr = S_OK;

  enter_op();

  // Pause is only allowed while running.
  if (m_state != SourceState::STATE_STARTED) {
    hr = MF_E_INVALID_STATE_TRANSITION;
  }

  // Pause the active streams.
  if (SUCCEEDED(hr)) {
    if(audio_stream) to_stream_ext(audio_stream)->Pause();
    if(video_stream) to_stream_ext(video_stream)->Pause();
  }

  m_state = SourceState::STATE_PAUSED;


  // Send the "paused" event. This might include a failure code.
  (void)event_queue->QueueEventParamVar(MESourcePaused, GUID_NULL, hr, NULL);

  leave_op();

  return hr;
}

HRESULT FlvSource::AsyncRequestData(){
  auto hr = AsyncDo(MFAsyncCallback::New([this](IMFAsyncResult*result)->HRESULT{
    scope_lock l(this);
    auto hr = this->DoRequestData();
    result->SetStatus(hr);
    return S_OK;
  }).Get(), static_cast<IMFMediaSource*>(this));
  return hr;
}
//-------------------------------------------------------------------
// StreamRequestSample
// Called by streams when they need more data.
//
// Note: This is an async operation. The stream requests more data
// by queueing an OP_REQUEST_DATA operation.
//-------------------------------------------------------------------

HRESULT FlvSource::DoRequestData()
{
  enter_op();
  DemuxSample();
  leave_op();
  return S_OK;
}

void FlvSource::DemuxSample(){
  if (!NeedDemux())
    return;
  if (status.pending_seek){
    status.pending_seek = 0;
    byte_stream->SetCurrentPosition(pending_seek_file_position);
  }
  status.pending_request = 1;
  ReadSampleHeader();
}
HRESULT FlvSource::ReadSampleHeader(){
  auto hr= parser.begin_tag_header(1, &on_demux_sample_header, nullptr);
  if (fail(hr)){
    Shutdown();
  }
  return hr;
}
HRESULT FlvSource::OnSampleHeader(IMFAsyncResult *result){
  tag_header tagh;
  auto hr = parser.end_tag_header(result, &tagh);
  if (tagh.type == flv::tag_type::eof){
    hr = EndOfFile();
  }
  else if (tagh.type == flv::tag_type::audio){
    hr = ReadAudioHeader(tagh);
  }
  else if (tagh.type == flv::tag_type::video){
    hr = ReadVideoHeader(tagh);
  }
  else if (fail(hr)){
    hr = Shutdown();
  }
  else {
    hr = SeekToNextTag(tagh);
  }
  return hr;
}

HRESULT FlvSource::ReadVideoHeader(tag_header const&h){
  auto hr = parser.begin_video_header(&on_video_header, NewMFState(h).Get());
  if (fail(hr))
    Shutdown();
  return hr;
}

HRESULT FlvSource::OnVideoHeader(IMFAsyncResult*result){
  video_header vh;
  auto hr = parser.end_video_header(result, &vh);
  video_packet_header vsh(FromAsyncResultState<tag_header>(result), vh);
  if (ok(hr)){
    if (vsh.codec_id == flv::video_codec::avc) {
      ReadAvcPacketType(vsh);
    }
    else{
//      byte_stream->GetCurrentPosition(&vsh.payload_offset);
      ReadVideoData(vsh);
    }
  }
  if (fail(hr))
    Shutdown();
  return hr;
}

HRESULT FlvSource::ReadAudioHeader(tag_header const&h){
  auto hr = parser.begin_audio_header(&on_audio_header, NewMFState(h).Get());
  if (fail(hr))
    Shutdown();
  return hr;
}
HRESULT FlvSource::OnAudioHeader(IMFAsyncResult* result){
  audio_header ah;
  auto hr = parser.end_audio_header(result, &ah);
  tag_header const&th = FromAsyncResultState<tag_header>(result);
  audio_packet_header ash(th, ah);
  if (ok(hr)){
    if (ah.codec_id == flv::audio_codec::aac)
      ReadAacPacketType(ash); // ignore return value
    else {
//      byte_stream->GetCurrentPosition(&ash.payload_offset);
      ReadAudioData(ash);
    }
  }
  if (fail(hr)){
    Shutdown();
  }
  return hr;
}

HRESULT FlvSource::ReadAacPacketType(audio_packet_header const&ash){
  auto hr = parser.begin_aac_packet_type(&on_aac_packet_type, NewMFState(ash).Get());
  if (fail(hr))
    Shutdown();
  return hr;
}
HRESULT FlvSource::OnAacPacketType(IMFAsyncResult*result){
  auto &ash = FromAsyncResultState<audio_packet_header>(result);
  auto hr = parser.end_aac_packet_type(result, &ash.aac_packet_type);
//  if (ok(hr))
//    hr = byte_stream->GetCurrentPosition(&ash.payload_offset);
  if (ok(hr))
    ReadAudioData(ash);

  if (fail(hr))
    Shutdown();
  return hr;
}

HRESULT FlvSource::ReadAvcPacketType(video_packet_header const&vsh){
  auto hr = parser.begin_avc_header(&on_avc_packet_type, NewMFState(vsh).Get());
  if (fail(hr))
    Shutdown();
  return hr;
}
HRESULT FlvSource::OnAvcPacketType(IMFAsyncResult*result){
  avc_header v;
  auto hr = parser.end_avc_header(result, &v);
  auto &vsh = FromAsyncResultState<video_packet_header>(result);
  vsh.avc_packet_type = v.avc_packet_type;
  vsh.composition_time = v.composite_time;
//  byte_stream->GetCurrentPosition(&vsh.payload_offset);
  if (ok(hr))
   //  OnVideoHeaderReady(vsh);
    ReadVideoData(vsh);

  if (fail(hr))
    Shutdown();
  return hr;
}
HRESULT FlvSource::ReadAudioData(audio_packet_header const& ash){
  auto hr = parser.begin_audio_data(ash.payload_length(), &on_audio_data, NewMFState<audio_packet_header>(ash).Get());
  if (fail(hr))
    Shutdown();
  return hr;
}
HRESULT FlvSource::DeliverAudioPacket(audio_packet_header const&ash){
  IMFMediaBufferPtr mbuf;
  auto hr = NewMFMediaBuffer(ash.payload._, ash.payload.length, &mbuf);

  IMFSamplePtr sample;
  if (ok(hr))
    hr = MFCreateSample(&sample);
  if(ok(hr)) hr = sample->AddBuffer(mbuf.Get());
  if (ok(hr)) hr = sample->SetSampleTime(ash.nano_timestamp);
  if (ok(hr)){
    auto astream = to_stream_ext(audio_stream);// static_cast<FlvStream*>(audio_stream.Get());
    hr = astream->DeliverPayload(sample.Get());
  }
  status.pending_request = 0;
  DemuxSample();
  return hr;
}
HRESULT FlvSource::OnAudioData(IMFAsyncResult *result){
  auto &ash = FromAsyncResultState<audio_packet_header>(result);
//  packet pack;
  auto  hr = parser.end_audio_data(result, &ash.payload);
  if (ok(hr) && status.first_audio_tag_ready){
    hr = DeliverAudioPacket(ash);
  }
  else if(ok(hr)){
    status.first_audio_tag_ready = 1;
    header.audio = ash;
    CheckFirstPacketsReady();
  }
  if (fail(hr))
    Shutdown();
  return hr;
}
HRESULT FlvSource::CheckFirstPacketsReady(){
  auto ar = !header.has_audio || header.audiocodecid != flv::audio_codec::aac || status.first_audio_tag_ready;
  auto vr = !header.has_video || header.videocodecid != flv::video_codec::avc || status.first_video_tag_ready;
  HRESULT hr = S_OK;
  if (ar && vr){
    hr = FinishInitialize();
  }
  else {
    hr = ReadFlvTagHeader();
  }
  return hr;
}
HRESULT FlvSource::ReadVideoData(video_packet_header const& vsh){
  auto hr = parser.begin_video_data(vsh.payload_length(), &on_video_data, NewMFState<video_packet_header>(vsh).Get());
  if (fail(hr))
    Shutdown();
  return hr;
}
HRESULT FlvSource::DeliverAvcPacket(video_packet_header const&vsh){
  IMFSamplePtr sample;
  auto  hr = MFCreateSample(&sample);
  if (!status.code_private_data_sent){
    status.code_private_data_sent = 1;
    IMFMediaBufferPtr cpdbuf;
    auto cpd = header.avcc.code_private_data();
    hr = NewMFMediaBuffer(cpd._, cpd.length, &cpdbuf);
    if (ok(hr))
      hr = sample->AddBuffer(cpdbuf.Get());
  }

  if (vsh.avc_packet_type == flv::avc_packet_type::avc_nalu){
    auto nal = header.avcc.nal;
    flv::nalu_reader reader(vsh.payload._, vsh.payload.length);
    for (auto nalu = reader.nalu(); nalu.length; nalu = reader.nalu()){
      IMFMediaBufferPtr mbuf;
      hr = NewNaluBuffer(nal, nalu, &mbuf);//MFCreateMemoryBuffer(vsh.payload.length, &mbuf);
      if (ok(hr)) hr = sample->AddBuffer(mbuf.Get());
    }
    assert(reader.pointer == reader.length);
  }

  if (ok(hr)) hr = sample->SetSampleTime(vsh.nano_timestamp);
  if (ok(hr)) hr = sample->SetUINT32(MFSampleExtension_CleanPoint, vsh.frame_type == flv::frame_type::key_frame ? 1 : 0);
  // should set sample duration

  if (ok(hr)){
    auto astream = to_stream_ext(video_stream);//static_cast<FlvStream*>(video_stream.Get());
    hr = astream->DeliverPayload(sample.Get());
  }
  status.pending_request = 0;
  DemuxSample();
  return hr;
}
HRESULT FlvSource::DeliverNAvcPacket(video_packet_header const&vsh){
  IMFSamplePtr sample;
  auto  hr = MFCreateSample(&sample);
  IMFMediaBufferPtr mbuf;
  hr = NewMFMediaBuffer(vsh.payload._, vsh.payload.length, &mbuf);
  if (ok(hr)) hr = sample->AddBuffer(mbuf.Get());

  if (ok(hr)) hr = sample->SetSampleTime(vsh.nano_timestamp + vsh.composition_time * 10000);
  if (ok(hr)) hr = sample->SetUINT32(MFSampleExtension_CleanPoint, vsh.frame_type == flv::frame_type::key_frame ? 1 : 0);
  // should set sample duration

  if (ok(hr)){
    auto astream = to_stream_ext(video_stream);//static_cast<FlvStream*>(video_stream.Get());
    hr = astream->DeliverPayload(sample.Get());
  }
  status.pending_request = 0;
  DemuxSample();
  return hr;
}


HRESULT FlvSource::DeliverVideoPacket(video_packet_header const& vsh){
  auto isk = vsh.frame_type == flv::frame_type::key_frame || vsh.frame_type == flv::frame_type::generated_key_frame;
  if (isk)
    current_keyframe = keyframe{ vsh.data_offset - flv::flv_tag_header_length, vsh.nano_timestamp  + vsh.composition_time * 10000};
  if (vsh.codec_id == flv::video_codec::avc)
    return DeliverAvcPacket(vsh);
  else
    return DeliverNAvcPacket(vsh);

}
HRESULT FlvSource::OnVideoData(IMFAsyncResult *result){
  auto &ash = FromAsyncResultState<video_packet_header>(result);
  auto  hr = parser.end_video_data(result, &ash.payload);
  if (ok(hr) && status.first_video_tag_ready){
    hr = DeliverVideoPacket(ash);
  }
  else if (ok(hr)){
    status.first_video_tag_ready = 1;
    header.video = ash;
    header.avcc = flv::avcc_reader(ash.payload._, ash.payload.length).avcc();
    CheckFirstPacketsReady();
  }

  if (fail(hr))
    Shutdown();
  return hr;
}

HRESULT FlvSource::EndOfFile(){
  auto vstream = to_stream_ext(video_stream);// static_cast<FlvStream*>(video_stream.Get());
  auto astream = to_stream_ext(audio_stream);// static_cast<FlvStream*>(audio_stream.Get());
  if (vstream)
    vstream->EndOfStream();
  if (astream)
    astream->EndOfStream();
  return S_OK;
}
bool FlvSource::NeedDemux() {
  if (fail(CheckShutdown())){
    return false;
  }
  if (status.pending_request)
    return false;
  auto vstream = to_stream_ext(video_stream);//static_cast<FlvStream*>(video_stream.Get());
  auto astream = to_stream_ext(audio_stream);//static_cast<FlvStream*>(audio_stream.Get());
  if (vstream && vstream->NeedsData() == S_OK)
    return true;
  if (astream && astream->NeedsData() == S_OK)
    return true;
  return false;
}

HRESULT FlvSource::AsyncEndOfStream(){
  auto hr = AsyncDo(MFAsyncCallback::New([this](IMFAsyncResult*result)->HRESULT{
    scope_lock l(this);
    auto hr = this->DoEndOfStream();
    result->SetStatus(hr);
    return S_OK;
  }).Get(), static_cast<IMFMediaSource*>(this));
  return hr;
}
//-------------------------------------------------------------------
// OnEndOfStream
// Called by each stream when it sends the last sample in the stream.
//
// Note: When the media source reaches the end of the MPEG-1 stream,
// it calls EndOfStream on each stream object. The streams might have
// data still in their queues. As each stream empties its queue, it
// notifies the source through an async OP_END_OF_STREAM operation.
//
// When every stream notifies the source, the source can send the
// "end-of-presentation" event.
//-------------------------------------------------------------------

HRESULT FlvSource::DoEndOfStream()
{
    HRESULT hr = S_OK;

    enter_op();

    // Decrement the count of end-of-stream notifications.
      --pending_eos;
      if (pending_eos == 0)
        {
            // No more streams. Send the end-of-presentation event.
          hr = event_queue->QueueEventParamVar(MEEndOfPresentation, GUID_NULL, S_OK, NULL);
        }


    if (SUCCEEDED(hr))
    {
      leave_op();
    }

    return hr;
}

HRESULT FlvSource::FinishInitialize(){
  HRESULT hr = S_OK;
//  if (header.first_media_tag_offset) {
//    hr = byte_stream->SetCurrentPosition(header.first_media_tag_offset - flv::flv_previous_tag_size_field_length);
  //}
  CreateAudioStream();
  CreateVideoStream();
  hr = InitPresentationDescriptor();

  return hr;
}

HRESULT FlvSource::CreateStream(DWORD index, IMFMediaType*media_type, IMFMediaStream**v) {
  ComPtr<IMFStreamDescriptor> pSD;
  ComPtr<IMFMediaTypeHandler> pHandler;

  auto  hr = MFCreateStreamDescriptor(index, 1, &media_type, &pSD);

  // Set the default media type on the stream handler.
  if (SUCCEEDED(hr))
  {
    hr = pSD->GetMediaTypeHandler(&pHandler);
  }
  if (SUCCEEDED(hr))
  {
    hr = pHandler->SetCurrentMediaType(media_type);
  }

  // Create the new stream.
  if (SUCCEEDED(hr))
  {
    //*v = new (std::nothrow) FlvStream(this, pSD, hr);
    hr = MakeAndInitialize<FlvStream>(v, this, pSD.Get());
  }

  // Add the stream to the array.

  return hr;
}
//-------------------------------------------------------------------
// CreateStream:
// Creates a media stream, based on a packet header.
//-------------------------------------------------------------------

HRESULT FlvSource::CreateAudioStream()
{
  IMFMediaTypePtr media_type;
  auto hr = CreateAudioMediaType(header, &media_type);
  if (ok(hr))  // audio stream_index  is 1
    hr = CreateStream(1, media_type.Get(), &audio_stream);
  return hr;
}

HRESULT FlvSource::CreateVideoStream(){
  IMFMediaTypePtr media_type;
  auto hr = CreateVideoMediaType(header, &media_type);
  if (ok(hr))
    hr = CreateStream(0, media_type.Get(), &video_stream);
  return hr;
}

//-------------------------------------------------------------------
// ValidatePresentationDescriptor:
// Validates the presentation descriptor that the caller specifies
// in IMFMediaSource::Start().
//
// Note: This method performs a basic sanity check on the PD. It is
// not intended to be a thorough validation.
//-------------------------------------------------------------------

HRESULT FlvSource::ValidatePresentationDescriptor(IMFPresentationDescriptor *pPD)
{
  pPD;
    HRESULT hr = S_OK;
//    DWORD cStreams = 0;

//    IMFStreamDescriptor *pSD = NULL;

    // The caller's PD must have the same number of streams as ours.
    //hr = pPD->GetStreamDescriptorCount(&cStreams);

    // The caller must select at least one stream.
    return hr;
}


//-------------------------------------------------------------------
// StreamingError:
// Handles an error that occurs duing an asynchronous operation.
//
// hr: Error code of the operation that failed.
//-------------------------------------------------------------------

void FlvSource::StreamingError(HRESULT hr)
{
  if (m_state == SourceState::STATE_OPENING)
    {
        // An error happened during BeginOpen.
        // Invoke the callback with the status code.

        CompleteOpen(hr);
    }
  else if (m_state != SourceState::STATE_SHUTDOWN)
    {
        // An error occurred during streaming. Send the MEError event
        // to notify the pipeline.

        QueueEvent(MEError, GUID_NULL, hr, NULL);
    }
}


/*  Static functions */


//-------------------------------------------------------------------
// CreateVideoMediaType:
// Create a media type from an MPEG-1 video sequence header.
//-------------------------------------------------------------------

HRESULT CreateVideoMediaType(const flv_file_header& header, IMFMediaType **ppType)
{
  if (header.videocodecid != flv::video_codec::avc){
    *ppType = nullptr;
    return MF_E_UNSUPPORTED_FORMAT;
  }
    HRESULT hr = S_OK;
    IMFMediaType *pType = NULL;
    hr = MFCreateMediaType(&pType);

    if (ok(hr)) hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (ok(hr)) hr = pType->SetGUID(MF_MT_SUBTYPE, MEDIASUBTYPE_H264);
//    if (ok(hr)) hr = pType->SetGUID(MF_MT_SUBTYPE, MEDIASUBTYPE_AVC1);  NOT SUPPORTED

    if (ok(hr)) hr = MFSetAttributeSize(pType, MF_MT_FRAME_SIZE,  header.width, header.height   );
    if (ok(hr)) hr = MFSetAttributeRatio(pType,MF_MT_FRAME_RATE, header.framerate ,1);
    if (ok(hr)) hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE_RANGE_MAX, header.framerate, 1);
    if (ok(hr)) hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE_RANGE_MIN, header.framerate / 2, 1);
    if (ok(hr)) hr = MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (ok(hr)) hr = pType->SetUINT32(MF_MT_AVG_BITRATE, header.videodatarate);

    // header.video.payload
    auto cpd = header.avcc.sequence_header();

    if (ok(hr)) hr = pType->SetUINT32(MF_MT_MPEG2_FLAGS, header.avcc.nal);  // from avcC
    if (ok(hr)) hr = pType->SetUINT32(MF_MT_MPEG2_PROFILE, header.avcc.profile); // eAVEncH264VProfile, from avcC
    if (ok(hr)) hr = pType->SetUINT32(MF_MT_MPEG2_LEVEL, header.avcc.level);
//    if (ok(hr)) hr = pType->SetBlob(MF_MT_USER_DATA, cpd._, cpd.length); // with no effect
    if (ok(hr)) hr = pType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, cpd._, cpd.length);
// CodecPrivateData issue of Smooth Streaming
// H264: exactly same as stated in the spec.The field is in NAL byte stream : 0x00 0x00 0x00 0x01 SPS 0x00 0x00 0x00 0x01 PPS.No problem

//    if (ok(hr)) hr = pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (ok(hr))
    {
        *ppType = pType;
        (*ppType)->AddRef();
    }

    release(&pType);
    return hr;
}

HRESULT CreateAudioMediaType(const flv_file_header& header, IMFMediaType **ppType)
{
  auto cid = header.audiocodecid;
  if (cid != flv::audio_codec::aac && cid != flv::audio_codec::mp3 && cid != flv::audio_codec::mp38k){
    *ppType = nullptr;
    return MF_E_UNSUPPORTED_FORMAT;
  }
  // MEDIASUBTYPE_MPEG_HEAAC not work
    IMFMediaType *pType = NULL;
    auto hr = MFCreateMediaType(&pType);
    if (ok(hr)) hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (ok(hr) && header.audiocodecid == flv::audio_codec::aac) hr = pType->SetGUID(MF_MT_SUBTYPE, MEDIASUBTYPE_RAW_AAC1);
    if (ok(hr) && header.audiocodecid == flv::audio_codec::mp3 || header.audiocodecid == flv::audio_codec::mp38k)
      hr = pType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
    if (ok(hr) && header.audiosamplerate) hr = pType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, header.audiosamplerate);
    if (ok(hr)) hr = pType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, header.stereo + 1);
    if (ok(hr)) hr = pType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
    if (ok(hr)) hr = pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, header.audiosamplesize);
    if (ok(hr)) hr = pType->SetUINT32(MF_MT_AVG_BITRATE, header.audiodatarate);
    if (ok(hr)) hr = pType->SetBlob(MF_MT_USER_DATA, header.audio.payload._, header.audio.payload.length);

    if (ok(hr))
    {
        *ppType = pType;
        (*ppType)->AddRef();
    }

    release(&pType);
    return hr;
}

// Get the major media type from a stream descriptor.
HRESULT GetStreamMajorType(IMFStreamDescriptor *pSD, GUID *pguidMajorType)
{
    if (pSD == NULL) { return E_POINTER; }
    if (pguidMajorType == NULL) { return E_POINTER; }

    HRESULT hr = S_OK;
    IMFMediaTypeHandlerPtr pHandler = NULL;

    hr = pSD->GetMediaTypeHandler(&pHandler);
    if (SUCCEEDED(hr))
    {
        hr = pHandler->GetMajorType(pguidMajorType);
    }
    return hr;
}


HRESULT FlvSource::AsyncDo(IMFAsyncCallback* invoke, IUnknown* state){
  auto hr = MFPutWorkItem( MFASYNC_CALLBACK_QUEUE_STANDARD,  invoke,  state);
  return hr;
}

HRESULT NewMFMediaBuffer(const uint8_t*data, uint32_t length, IMFMediaBuffer **rtn){
  IMFMediaBufferPtr v;
  auto hr = MFCreateMemoryBuffer(length, &v);
  uint8_t* buffer = nullptr;
  uint32_t max_length = 0;
  if (ok(hr))
    hr = v->Lock(&buffer, (DWORD*)&max_length, nullptr);
  if (buffer){
    memcpy_s(buffer, max_length, data, length);
    v->SetCurrentLength(length);
  }
  if (ok(hr))
    hr = v->Unlock();
  if (ok(hr)){
    *rtn = v.Get();
    (*rtn)->AddRef();
  }
  return hr;
}
HRESULT NewNaluBuffer(uint8_t nallength, packet const&nalu, IMFMediaBuffer **rtn){
  IMFMediaBufferPtr v;
  auto hr = MFCreateMemoryBuffer(nallength + nalu.length, &v);
  uint8_t* buffer = nullptr;
  uint32_t max_length = 0;
  if (ok(hr))
    hr = v->Lock(&buffer, (DWORD*)&max_length, nullptr);
  uint32_t startcode = 0x00000001;

  if (buffer){
    bigendian::binary_writer writer(buffer, max_length);
    if (nallength < 4)
      writer.ui24(startcode);
    else writer.ui32(startcode);
    writer.packet(nalu);
    v->SetCurrentLength(max_length);
  }
  if (ok(hr))
    hr = v->Unlock();
  if (ok(hr)){
    *rtn = v.Get();
    (*rtn)->AddRef();
  }
  return hr;
}

IMFMediaStreamExtPtr to_stream_ext(IMFMediaStreamPtr &stream) {
  IMFMediaStreamExtPtr v;
  (void)stream.As(&v);
  return v;
}
_prop_variant_t::~_prop_variant_t(){
  PropVariantClear(this);
}
_prop_variant_t::_prop_variant_t(PROPVARIANT const*p){
  PropVariantCopy(this, p);
}
_prop_variant_t::_prop_variant_t(_prop_variant_t const&rhs){
  PropVariantCopy(this, &rhs);
}
_prop_variant_t::_prop_variant_t(uint64_t v) {
  PropVariantInit(this);
  vt = VT_I8;
  hVal.QuadPart = v;
}