#include "ClipboardImageFrame.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("Clipboard image frame round trips binary payload",
          "[ClipboardImageFrame]") {
  ClipboardImagePayload payload;
  payload.extension = "png";
  payload.bytes = string("abc\0def", 7);

  string frame = encodeClipboardImageFrame(payload);
  ClipboardImagePayload decoded;
  string error;

  REQUIRE(decodeClipboardImageFrame(frame, &decoded, &error));
  REQUIRE(error.empty());
  CHECK(decoded.extension == payload.extension);
  CHECK(decoded.bytes == payload.bytes);
}

TEST_CASE("Clipboard image frame ignores ordinary terminal input",
          "[ClipboardImageFrame]") {
  ClipboardImagePayload decoded;
  string error;

  CHECK_FALSE(decodeClipboardImageFrame("hello", &decoded, &error));
  CHECK(error.empty());
}

TEST_CASE("Clipboard image frame rejects malformed image size",
          "[ClipboardImageFrame]") {
  string frame = string("\0ET_CLIPBOARD_IMAGE_V1\0", 23);
  frame += "png\nnot-a-size\nabc";

  ClipboardImagePayload decoded;
  string error;

  REQUIRE(decodeClipboardImageFrame(frame, &decoded, &error));
  CHECK(error == "invalid image size");
}

TEST_CASE("Clipboard image frame rejects oversized images",
          "[ClipboardImageFrame]") {
  string frame = string("\0ET_CLIPBOARD_IMAGE_V1\0", 23);
  frame += "png\n" + to_string(kMaxClipboardImageBytes + 1) + "\n";

  ClipboardImagePayload decoded;
  string error;

  REQUIRE(decodeClipboardImageFrame(frame, &decoded, &error));
  CHECK(error == "image size outside allowed range");
}

#ifndef WIN32
TEST_CASE("Clipboard image frame saves payload to temp image file",
          "[ClipboardImageFrame]") {
  ClipboardImagePayload payload;
  payload.extension = "png";
  payload.bytes = "fakepng";

  ClipboardImageSaveResult result =
      saveClipboardImageFrameToTemp(encodeClipboardImageFrame(payload));

  REQUIRE(result.isFrame);
  REQUIRE(result.saved);
  REQUIRE(result.error.empty());
  CHECK(result.path.find(".png") == result.path.size() - 4);

  ifstream input(result.path, ios::binary);
  REQUIRE(input);
  string bytes((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
  CHECK(bytes == payload.bytes);

  remove(result.path.c_str());
}
#endif
