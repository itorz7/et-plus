#ifndef __ET_CLIPBOARD_IMAGE_FRAME_HPP__
#define __ET_CLIPBOARD_IMAGE_FRAME_HPP__

#include "Headers.hpp"

namespace et {

constexpr const char* kClipboardImagePasteCapability =
    "clipboard-image-paste";
inline constexpr size_t kMaxClipboardImageBytes = 25 * 1024 * 1024;

struct ClipboardImagePayload {
  string extension;
  string bytes;
};

struct ClipboardImageSaveResult {
  bool isFrame = false;
  bool saved = false;
  string path;
  string error;
};

string encodeClipboardImageFrame(const ClipboardImagePayload& payload);
bool decodeClipboardImageFrame(const string& frame,
                               ClipboardImagePayload* payload,
                               string* error);
ClipboardImageSaveResult saveClipboardImageFrameToTemp(const string& frame);

}  // namespace et

#endif  // __ET_CLIPBOARD_IMAGE_FRAME_HPP__
