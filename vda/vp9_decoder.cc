// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vp9_decoder.h"

#include <memory>

#include "base/bind.h"
#include "base/logging.h"

namespace media {

VP9Decoder::VP9Accelerator::VP9Accelerator() {}

VP9Decoder::VP9Accelerator::~VP9Accelerator() {}

VP9Decoder::VP9Decoder(VP9Accelerator* accelerator)
    : state_(kNeedStreamMetadata),
      accelerator_(accelerator),
      parser_(accelerator->IsFrameContextRequired()) {
  ref_frames_.resize(kVp9NumRefFrames);
}

VP9Decoder::~VP9Decoder() {}

void VP9Decoder::SetStream(const uint8_t* ptr, size_t size) {
  DCHECK(ptr);
  DCHECK(size);

  DVLOG(4) << "New input stream at: " << (void*)ptr << " size: " << size;
  parser_.SetStream(ptr, size);
}

bool VP9Decoder::Flush() {
  DVLOG(2) << "Decoder flush";
  Reset();
  return true;
}

void VP9Decoder::Reset() {
  curr_frame_hdr_ = nullptr;
  for (auto& ref_frame : ref_frames_)
    ref_frame = nullptr;

  parser_.Reset();

  if (state_ == kDecoding)
    state_ = kAfterReset;
}

VP9Decoder::DecodeResult VP9Decoder::Decode() {
  while (1) {
    // Read a new frame header if one is not awaiting decoding already.
    if (!curr_frame_hdr_) {
      std::unique_ptr<Vp9FrameHeader> hdr(new Vp9FrameHeader());
      Vp9Parser::Result res = parser_.ParseNextFrame(hdr.get());
      switch (res) {
        case Vp9Parser::kOk:
          curr_frame_hdr_ = std::move(hdr);
          break;

        case Vp9Parser::kEOStream:
          return kRanOutOfStreamData;

        case Vp9Parser::kInvalidStream:
          DVLOG(1) << "Error parsing stream";
          SetError();
          return kDecodeError;

        case Vp9Parser::kAwaitingRefresh:
          DVLOG(4) << "Awaiting context update";
          return kNeedContextUpdate;
      }
    }

    if (state_ != kDecoding) {
      // Not kDecoding, so we need a resume point (a keyframe), as we are after
      // reset or at the beginning of the stream. Drop anything that is not
      // a keyframe in such case, and continue looking for a keyframe.
      if (curr_frame_hdr_->IsKeyframe()) {
        state_ = kDecoding;
      } else {
        curr_frame_hdr_.reset();
        continue;
      }
    }

    if (curr_frame_hdr_->show_existing_frame) {
      // This frame header only instructs us to display one of the
      // previously-decoded frames, but has no frame data otherwise. Display
      // and continue decoding subsequent frames.
      size_t frame_to_show = curr_frame_hdr_->frame_to_show_map_idx;
      if (frame_to_show >= ref_frames_.size() || !ref_frames_[frame_to_show]) {
        DVLOG(1) << "Request to show an invalid frame";
        SetError();
        return kDecodeError;
      }

      if (!accelerator_->OutputPicture(ref_frames_[frame_to_show])) {
        SetError();
        return kDecodeError;
      }

      curr_frame_hdr_.reset();
      continue;
    }

    Size new_pic_size(curr_frame_hdr_->frame_width,
                           curr_frame_hdr_->frame_height);
    DCHECK(!new_pic_size.IsEmpty());

    if (new_pic_size != pic_size_) {
      DVLOG(1) << "New resolution: " << new_pic_size.ToString();

      if (!curr_frame_hdr_->IsKeyframe()) {
        // TODO(posciak): This is doable, but requires a few modifications to
        // VDA implementations to allow multiple picture buffer sets in flight.
        DVLOG(1) << "Resolution change currently supported for keyframes only";
        SetError();
        return kDecodeError;
      }

      // TODO(posciak): This requires us to be on a keyframe (see above) and is
      // required, because VDA clients expect all surfaces to be returned before
      // they can cycle surface sets after receiving kAllocateNewSurfaces.
      // This is only an implementation detail of VDAs and can be improved.
      for (auto& ref_frame : ref_frames_)
        ref_frame = nullptr;

      pic_size_ = new_pic_size;
      return kAllocateNewSurfaces;
    }

    scoped_refptr<VP9Picture> pic = accelerator_->CreateVP9Picture();
    if (!pic)
      return kRanOutOfSurfaces;

    pic->frame_hdr.reset(curr_frame_hdr_.release());

    if (!DecodeAndOutputPicture(pic)) {
      SetError();
      return kDecodeError;
    }
  }
}

void VP9Decoder::RefreshReferenceFrames(const scoped_refptr<VP9Picture>& pic) {
  for (size_t i = 0; i < kVp9NumRefFrames; ++i) {
    DCHECK(!pic->frame_hdr->IsKeyframe() || pic->frame_hdr->RefreshFlag(i));
    if (pic->frame_hdr->RefreshFlag(i))
      ref_frames_[i] = pic;
  }
}

void VP9Decoder::UpdateFrameContext(
    const scoped_refptr<VP9Picture>& pic,
    const base::Callback<void(const Vp9FrameContext&)>& context_refresh_cb) {
  DCHECK(!context_refresh_cb.is_null());
  Vp9FrameContext frame_ctx;
  memset(&frame_ctx, 0, sizeof(frame_ctx));

  if (!accelerator_->GetFrameContext(pic, &frame_ctx)) {
    SetError();
    return;
  }

  context_refresh_cb.Run(frame_ctx);
}

bool VP9Decoder::DecodeAndOutputPicture(scoped_refptr<VP9Picture> pic) {
  DCHECK(!pic_size_.IsEmpty());
  DCHECK(pic->frame_hdr);

  base::Closure done_cb;
  const auto& context_refresh_cb =
      parser_.GetContextRefreshCb(pic->frame_hdr->frame_context_idx);
  if (!context_refresh_cb.is_null())
    done_cb = base::Bind(&VP9Decoder::UpdateFrameContext,
                         base::Unretained(this), pic, context_refresh_cb);

  const Vp9Parser::Context& context = parser_.context();
  if (!accelerator_->SubmitDecode(pic, context.segmentation(),
                                  context.loop_filter(), ref_frames_, done_cb))
    return false;

  if (pic->frame_hdr->show_frame) {
    if (!accelerator_->OutputPicture(pic))
      return false;
  }

  RefreshReferenceFrames(pic);
  return true;
}

void VP9Decoder::SetError() {
  Reset();
  state_ = kError;
}

Size VP9Decoder::GetPicSize() const {
  return pic_size_;
}

size_t VP9Decoder::GetRequiredNumOfPictures() const {
  // kMaxVideoFrames to keep higher level media pipeline populated, +2 for the
  // pictures being parsed and decoded currently.
  // TODO(johnylin): see if we could get rid of kMaxVideoFrames.
  const size_t kMaxVideoFrames = 4;
  return kMaxVideoFrames + kVp9NumRefFrames + 2;
}

}  // namespace media
