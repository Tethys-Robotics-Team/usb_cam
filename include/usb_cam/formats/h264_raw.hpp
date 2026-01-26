#ifndef USB_CAM__FORMATS__H264_RAW_HPP_
#define USB_CAM__FORMATS__H264_RAW_HPP_

extern "C" {
#include "linux/videodev2.h"
}

#include "usb_cam/formats/pixel_format_base.hpp"

namespace usb_cam
{
namespace formats
{

/// @brief Raw H264 format that does not decode, just passes through the compressed data
class H264 : public pixel_format_base
{
public:
  explicit H264(const format_arguments_t & args [[maybe_unused]] = format_arguments_t())
  : pixel_format_base(
      "h264",
      V4L2_PIX_FMT_H264,
      "h264",  // encoding type
      1,       // channels (not really applicable for compressed)
      8,       // bits per pixel (not really applicable for compressed)
      false)   // requires_conversion = false since we're passing through
  {
  }

  /// @brief No conversion needed, just copy the data
  void convert(const char * & src, char * & dest, const int & bytes_used) override
  {
    memcpy(dest, src, bytes_used);
  }
};

}  // namespace formats
}  // namespace usb_cam

#endif  // USB_CAM__FORMATS__H264_RAW_HPP_
