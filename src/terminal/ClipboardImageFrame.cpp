#include "ClipboardImageFrame.hpp"

#include <cctype>

namespace et {
namespace {

const string kClipboardImageFrameMagic("\0ET_CLIPBOARD_IMAGE_V1\0", 23);

bool isSafeExtension(const string& extension) {
  if (extension.empty() || extension.size() > 8) {
    return false;
  }
  return all_of(extension.begin(), extension.end(), [](unsigned char c) {
    return isalnum(c) != 0;
  });
}

#ifndef WIN32
bool writeAllToFd(int fd, const string& bytes) {
  size_t pos = 0;
  while (pos < bytes.size()) {
    ssize_t written = write(fd, bytes.data() + pos, bytes.size() - pos);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    pos += static_cast<size_t>(written);
  }
  return true;
}
#endif

}  // namespace

string encodeClipboardImageFrame(const ClipboardImagePayload& payload) {
  string frame = kClipboardImageFrameMagic;
  frame += payload.extension;
  frame += '\n';
  frame += to_string(payload.bytes.size());
  frame += '\n';
  frame += payload.bytes;
  return frame;
}

bool decodeClipboardImageFrame(const string& frame,
                               ClipboardImagePayload* payload,
                               string* error) {
  if (frame.rfind(kClipboardImageFrameMagic, 0) != 0) {
    return false;
  }

  const size_t extensionStart = kClipboardImageFrameMagic.size();
  const size_t extensionEnd = frame.find('\n', extensionStart);
  if (extensionEnd == string::npos) {
    if (error) *error = "missing extension delimiter";
    return true;
  }

  string extension = frame.substr(extensionStart, extensionEnd - extensionStart);
  if (!isSafeExtension(extension)) {
    if (error) *error = "unsafe image extension";
    return true;
  }

  const size_t sizeStart = extensionEnd + 1;
  const size_t sizeEnd = frame.find('\n', sizeStart);
  if (sizeEnd == string::npos) {
    if (error) *error = "missing size delimiter";
    return true;
  }

  string sizeString = frame.substr(sizeStart, sizeEnd - sizeStart);
  if (sizeString.empty() ||
      !all_of(sizeString.begin(), sizeString.end(), [](unsigned char c) {
        return isdigit(c) != 0;
      })) {
    if (error) *error = "invalid image size";
    return true;
  }

  size_t imageSize = 0;
  try {
    imageSize = stoull(sizeString);
  } catch (const exception&) {
    if (error) *error = "invalid image size";
    return true;
  }

  if (imageSize == 0 || imageSize > kMaxClipboardImageBytes) {
    if (error) *error = "image size outside allowed range";
    return true;
  }

  const size_t dataStart = sizeEnd + 1;
  if (frame.size() - dataStart != imageSize) {
    if (error) *error = "image payload length mismatch";
    return true;
  }

  if (payload) {
    payload->extension = extension;
    payload->bytes = frame.substr(dataStart);
  }
  if (error) {
    error->clear();
  }
  return true;
}

ClipboardImageSaveResult saveClipboardImageFrameToTemp(const string& frame) {
  ClipboardImageSaveResult result;
  ClipboardImagePayload payload;

  string error;
  const bool isFrame = decodeClipboardImageFrame(frame, &payload, &error);
  if (!isFrame) {
    return result;
  }

  result.isFrame = true;
  if (!error.empty()) {
    result.error = error;
    return result;
  }

#ifdef WIN32
  result.error = "remote clipboard image save is not supported on Windows";
  return result;
#else
  string directory = GetTempDirectory() + "et-clipboard-images-" +
                     to_string(static_cast<long long>(getuid()));
  if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST) {
    result.error = string("could not create temp image directory: ") +
                   strerror(errno);
    return result;
  }
  chmod(directory.c_str(), 0700);

  string pathTemplate = directory + "/image-XXXXXX";
  int fd = mkstemp(&pathTemplate[0]);
  if (fd < 0) {
    result.error = string("could not create temp image file: ") +
                   strerror(errno);
    return result;
  }

  if (!writeAllToFd(fd, payload.bytes)) {
    result.error = string("could not write temp image file: ") +
                   strerror(errno);
    close(fd);
    unlink(pathTemplate.c_str());
    return result;
  }

  if (close(fd) < 0) {
    result.error = string("could not close temp image file: ") +
                   strerror(errno);
    unlink(pathTemplate.c_str());
    return result;
  }

  string finalPath = pathTemplate + "." + payload.extension;
  if (rename(pathTemplate.c_str(), finalPath.c_str()) < 0) {
    result.error = string("could not rename temp image file: ") +
                   strerror(errno);
    unlink(pathTemplate.c_str());
    return result;
  }

  result.saved = true;
  result.path = finalPath;
  return result;
#endif
}

}  // namespace et
