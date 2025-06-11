#ifndef USB_CAM__FORMATS__H264_HPP_
#define USB_CAM__FORMATS__H264_HPP_

extern "C" {
#define __STDC_CONSTANT_MACROS
#include "libavutil/imgutils.h"
#include "libavformat/avformat.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "linux/videodev2.h"
#include "libswscale/swscale.h"
}

#include "usb_cam/formats/pixel_format_base.hpp"
#include "usb_cam/formats/utils.hpp"
#include "usb_cam/formats/av_pixel_format_helper.hpp"

namespace usb_cam
{
namespace formats
{

class H2642RGB : public pixel_format_base
{
public:
  explicit H2642RGB(const format_arguments_t & args = format_arguments_t())
  : pixel_format_base(
      "h2642rgb",
      V4L2_PIX_FMT_H264,
      usb_cam::constants::RGB8,
      3,
      8,
      true),
    m_avcodec(avcodec_find_decoder(AVCodecID::AV_CODEC_ID_H264)),
    m_avparser(av_parser_init(AVCodecID::AV_CODEC_ID_H264)),
    m_avframe_device(av_frame_alloc()),
    m_avframe_rgb(av_frame_alloc()),
    m_avoptions(NULL),
    m_averror_str(reinterpret_cast<char *>(malloc(AV_ERROR_MAX_STRING_SIZE)))
  {
    if (!m_avcodec) {
      throw std::runtime_error("Could not find H264 decoder");
    }

    if (!m_avparser) {
      throw std::runtime_error("Could not find H264 parser");
    }

    m_avcodec_context = avcodec_alloc_context3(m_avcodec);

    m_avframe_device->width = args.width;
    m_avframe_device->height = args.height;
    m_avframe_device->format = formats::get_av_pixel_format_from_string(args.av_device_format_str);

    m_avframe_rgb->width = args.width;
    m_avframe_rgb->height = args.height;
    m_avframe_rgb->format = AV_PIX_FMT_RGB24;

    m_sws_context = sws_getContext(
      args.width, args.height, (AVPixelFormat)m_avframe_device->format,
      args.width, args.height, (AVPixelFormat)m_avframe_rgb->format, SWS_FAST_BILINEAR,
      NULL, NULL, NULL);

    av_log_set_level(AV_LOG_FATAL);
    av_log_set_flags(AV_LOG_SKIP_REPEATED);

    m_avcodec_context->width = args.width;
    m_avcodec_context->height = args.height;
    m_avcodec_context->pix_fmt = (AVPixelFormat)m_avframe_device->format;
    m_avcodec_context->codec_type = AVMEDIA_TYPE_VIDEO;

    m_avframe_device_size = static_cast<size_t>(
      av_image_get_buffer_size(
        (AVPixelFormat)m_avframe_device->format,
        m_avframe_device->width,
        m_avframe_device->height,
        m_align));
    m_avframe_rgb_size = static_cast<size_t>(
      av_image_get_buffer_size(
        (AVPixelFormat)m_avframe_rgb->format,
        m_avframe_rgb->width,
        m_avframe_rgb->height,
        m_align));

    if (avcodec_open2(m_avcodec_context, m_avcodec, &m_avoptions) < 0) {
      throw std::runtime_error("Could not open H264 decoder");
      return;
    }

    m_result = av_frame_get_buffer(m_avframe_device, m_align);
    if (m_result != 0) {
      print_av_error_string(m_result);
    }
    m_result = av_frame_get_buffer(m_avframe_rgb, m_align);
    if (m_result != 0) {
      print_av_error_string(m_result);
    }
  }

  ~H2642RGB()
  {
    if (m_averror_str) {
      free(m_averror_str);
    }
    if (m_avoptions) {
      free(m_avoptions);
    }
    if (m_avcodec_context) {
      avcodec_free_context(&m_avcodec_context);
    }
    if (m_avframe_device) {
      av_frame_free(&m_avframe_device);
    }
    if (m_avframe_rgb) {
      av_frame_free(&m_avframe_rgb);
    }
    if (m_avparser) {
      av_parser_close(m_avparser);
    }
    if (m_sws_context) {
      sws_freeContext(m_sws_context);
    }
  }

  void convert(const char * & src, char * & dest, const int & bytes_used) override
  {
    m_result = 0;
    memset(dest, 0, m_avframe_device_size);

    auto avpacket = av_packet_alloc();
    av_new_packet(avpacket, bytes_used);
    memcpy(avpacket->data, src, bytes_used);

    m_result = avcodec_send_packet(m_avcodec_context, avpacket);

    av_packet_free(&avpacket);

    if (m_result != 0) {
      std::cerr << "Failed to send AVPacket to decode: ";
      print_av_error_string(m_result);
      return;
    }

    m_result = avcodec_receive_frame(m_avcodec_context, m_avframe_device);

    if (m_result == AVERROR(EAGAIN) || m_result == AVERROR_EOF) {
      return;
    } else if (m_result < 0) {
      std::cerr << "Failed to receive decoded frame from codec: ";
      print_av_error_string(m_result);
      return;
    }

    sws_scale(
      m_sws_context, m_avframe_device->data,
      m_avframe_device->linesize, 0, m_avframe_device->height,
      m_avframe_rgb->data, m_avframe_rgb->linesize);

    av_image_copy_to_buffer(
      const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(dest)),
      m_avframe_rgb_size, m_avframe_rgb->data,
      m_avframe_rgb->linesize, (AVPixelFormat)m_avframe_rgb->format,
      m_avframe_rgb->width, m_avframe_rgb->height, m_align);
  }

private:
  void print_av_error_string(int & err_code)
  {
    av_make_error_string(m_averror_str, AV_ERROR_MAX_STRING_SIZE, err_code);
    std::cerr << m_averror_str << std::endl;
  }

  const AVCodec * m_avcodec;
  AVCodecContext * m_avcodec_context;
  AVCodecParserContext * m_avparser;
  AVFrame * m_avframe_device;
  AVFrame * m_avframe_rgb;
  AVDictionary * m_avoptions;
  SwsContext * m_sws_context;
  size_t m_avframe_device_size;
  size_t m_avframe_rgb_size;
  char * m_averror_str;
  int m_result = 0;

  const int m_align = 32;
};

}  // namespace formats
}  // namespace usb_cam

#endif  // USB_CAM__FORMATS__H264_HPP_