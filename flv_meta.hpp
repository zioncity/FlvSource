#pragma once
#include "keyframes.hpp"
#include "flv.hpp"
#include <vector>
struct flv_meta {
  flv::audio_codec     audiocodecid;  // only aac and mp3 are supported
  flv::video_codec     videocodecid;  // only avc is supported, avc mapped to media_subtype_h264
  uint32_t              audiodatarate           = 0; //bits per second
  uint32_t              audiodelay              = 0; // seconds
  uint16_t              audiosamplesize         = 0; // 8bits /16bits
  uint64_t              audiosize               = 0; // bytes
  uint64_t              duration;                    // seconds
  uint64_t              filesize;                    // total file size bytes
  uint64_t              datasize;                    // bytes
  uint32_t              height ;                     // pixels
  uint32_t              width;                       // pixels
  uint32_t              videodatarate           = 0; // bits per second
  uint32_t              audiosamplerate         = 0; //bits per second
  uint32_t              framerate               = 0; // frames per second
  uint32_t              last_timestamp          = 0; // milliseconds
  uint32_t              last_keyframe_timestamp = 0;
  uint8_t               can_seek_to_end         = 0; // boolean
  uint8_t               stereo                  = 1; // 1: stereo
  uint8_t               has_video               = 0;
  uint8_t               has_audio               = 0;
  uint8_t               has_metadata            = 0;

  ::keyframes keyframes;
};
