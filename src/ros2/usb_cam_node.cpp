// Copyright 2014 Robert Bosch, LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the Robert Bosch, LLC nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include "usb_cam/usb_cam_node.hpp"
#include "usb_cam/utils.hpp"

const char BASE_TOPIC_NAME[] = "image_raw";

namespace usb_cam
{

UsbCamNode::UsbCamNode(const rclcpp::NodeOptions & node_options)
: Node("usb_cam", node_options),
  m_camera(new usb_cam::UsbCam()),
  m_image_msg(new sensor_msgs::msg::Image()),
  m_compressed_img_msg(nullptr),
  m_image_publisher(std::make_shared<image_transport::CameraPublisher>(
      image_transport::create_camera_publisher(this, BASE_TOPIC_NAME,
      rclcpp::QoS {100}.get_rmw_qos_profile()))),
  m_compressed_image_publisher(nullptr),
  m_compressed_cam_info_publisher(nullptr),
  m_parameters(),
  m_camera_info_msg(new sensor_msgs::msg::CameraInfo()),
  m_service_capture(
    this->create_service<std_srvs::srv::SetBool>(
      "set_capture",
      std::bind(
        &UsbCamNode::service_capture,
        this,
        std::placeholders::_1,
        std::placeholders::_2,
        std::placeholders::_3)))
{
  // declare params
  this->declare_parameter("camera_name", "default_cam");
  this->declare_parameter("camera_info_url", "");
  this->declare_parameter("framerate", 30.0);
  this->declare_parameter("frame_id", "default_cam");
  this->declare_parameter("image_height", 1920);
  this->declare_parameter("image_width", 1080);
  this->declare_parameter("io_method", "mmap");
  this->declare_parameter("pixel_format", "yuyv");
  this->declare_parameter("av_device_format", "YUV422P");
  this->declare_parameter("video_device", "/dev/video0");
  this->declare_parameter("brightness", 0);  
  this->declare_parameter("contrast", 32);    
  this->declare_parameter("saturation", 64);  
  this->declare_parameter("hue", 0); 
  this->declare_parameter("gamma", 100); 
  this->declare_parameter("gain", 0); 
  this->declare_parameter("power_line_frequency", 2);
  this->declare_parameter("white_balance_automatic", true);
  this->declare_parameter("white_balance_temperature", 4600);
  this->declare_parameter("sharpness", 3);   
  this->declare_parameter("backlight_compensation", 5);   
  this->declare_parameter("auto_exposure", 3);
  this->declare_parameter("exposure_time_absolute", 157);
  this->declare_parameter("exposure_dynamic_framerate", false);  

  get_params();
  init();
  m_parameters_callback_handle = add_on_set_parameters_callback(
    std::bind(
      &UsbCamNode::parameters_callback, this,
      std::placeholders::_1));
}

UsbCamNode::~UsbCamNode()
{
  RCLCPP_WARN(this->get_logger(), "Shutting down");
  m_image_msg.reset();
  m_compressed_img_msg.reset();
  m_camera_info_msg.reset();
  m_camera_info.reset();
  m_timer.reset();
  m_service_capture.reset();
  m_parameters_callback_handle.reset();

  delete (m_camera);
}

void UsbCamNode::service_capture(
  const std::shared_ptr<rmw_request_id_t> request_header,
  const std::shared_ptr<std_srvs::srv::SetBool::Request> request,
  std::shared_ptr<std_srvs::srv::SetBool::Response> response)
{
  (void) request_header;
  if (request->data) {
    m_camera->start_capturing();
    response->message = "Start Capturing";
  } else {
    m_camera->stop_capturing();
    response->message = "Stop Capturing";
  }
}

std::string resolve_device_path(const std::string & path)
{
  if (std::filesystem::is_symlink(path)) {
    // For some reason read_symlink only returns videox
    return "/dev/" + std::string(std::filesystem::read_symlink(path));
  }
  return path;
}

void UsbCamNode::init()
{
  while (m_parameters.frame_id == "") {
    RCLCPP_WARN_ONCE(
      this->get_logger(), "Required Parameters not set...waiting until they are set");
    get_params();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  // load the camera info
  m_camera_info.reset(
    new camera_info_manager::CameraInfoManager(
      this, m_parameters.camera_name, m_parameters.camera_info_url));
  // check for default camera info
  if (!m_camera_info->isCalibrated()) {
    m_camera_info->setCameraName(m_parameters.device_name);
    m_camera_info_msg->header.frame_id = m_parameters.frame_id;
    m_camera_info_msg->width = m_parameters.image_width;
    m_camera_info_msg->height = m_parameters.image_height;
    m_camera_info->setCameraInfo(*m_camera_info_msg);
  }

  // Check if given device name is an available v4l2 device
  auto available_devices = usb_cam::utils::available_devices();
  if (available_devices.find(m_parameters.device_name) == available_devices.end()) {
    RCLCPP_ERROR_STREAM(
      this->get_logger(),
      "Device specified is not available or is not a vaild V4L2 device: `" <<
        m_parameters.device_name << "`"
    );
    RCLCPP_INFO(this->get_logger(), "Available V4L2 devices are:");
    for (const auto & device : available_devices) {
      RCLCPP_INFO_STREAM(this->get_logger(), "    " << device.first);
      RCLCPP_INFO_STREAM(this->get_logger(), "        " << device.second.card);
    }
    rclcpp::shutdown();
    return;
  }

  // if pixel format is equal to 'mjpeg', i.e. raw mjpeg stream, initialize compressed image message
  // and publisher
  if (m_parameters.pixel_format_name == "mjpeg") {
    m_compressed_img_msg.reset(new sensor_msgs::msg::CompressedImage());
    m_compressed_img_msg->header.frame_id = m_parameters.frame_id;
    m_compressed_image_publisher =
      this->create_publisher<sensor_msgs::msg::CompressedImage>(
      std::string(BASE_TOPIC_NAME) + "/compressed", rclcpp::QoS(100));
    m_compressed_cam_info_publisher =
      this->create_publisher<sensor_msgs::msg::CameraInfo>(
      "camera_info", rclcpp::QoS(100));
  }

  m_image_msg->header.frame_id = m_parameters.frame_id;
  RCLCPP_INFO(
    this->get_logger(), "Starting '%s' (%s) at %dx%d via %s (%s) at %i FPS",
    m_parameters.camera_name.c_str(), m_parameters.device_name.c_str(),
    m_parameters.image_width, m_parameters.image_height, m_parameters.io_method_name.c_str(),
    m_parameters.pixel_format_name.c_str(), m_parameters.framerate);
  // set the IO method
  io_method_t io_method =
    usb_cam::utils::io_method_from_string(m_parameters.io_method_name);
  if (io_method == usb_cam::utils::IO_METHOD_UNKNOWN) {
    RCLCPP_ERROR_ONCE(
      this->get_logger(),
      "Unknown IO method '%s'", m_parameters.io_method_name.c_str());
    rclcpp::shutdown();
    return;
  }

  // configure the camera
  m_camera->configure(m_parameters, io_method);

  set_v4l2_params();

  // start the camera
  m_camera->start();

  // TODO(lucasw) should this check a little faster than expected frame rate?
  // TODO(lucasw) how to do small than ms, or fractional ms- std::chrono::nanoseconds?
  const int period_ms = 1000.0 / m_parameters.framerate;
  m_timer = this->create_wall_timer(
    std::chrono::milliseconds(static_cast<int64_t>(period_ms)),
    std::bind(&UsbCamNode::update, this));
  RCLCPP_INFO_STREAM(this->get_logger(), "Timer triggering every " << period_ms << " ms");
}

void UsbCamNode::get_params()
{
  auto parameters_client = std::make_shared<rclcpp::SyncParametersClient>(this);
  auto parameters = parameters_client->get_parameters(
    {
      "camera_name", "camera_info_url", "frame_id", "framerate", "image_height", "image_width",
      "io_method", "pixel_format", "av_device_format", "video_device", "brightness", "contrast",
      "saturation", "hue", "gamma", "gain", "power_line_frequency", "white_balance_temperature", 
      "white_balance_automatic", "sharpness", "backlight_compensation", "auto_exposure", 
      "exposure_time_absolute", "exposure_dynamic_framerate"
    }
  );

  assign_params(parameters);
}

void UsbCamNode::assign_params(const std::vector<rclcpp::Parameter> & parameters)
{
  for (auto & parameter : parameters) {
    if (parameter.get_name() == "camera_name") {
      RCLCPP_INFO(this->get_logger(), "camera_name value: %s", parameter.value_to_string().c_str());
      m_parameters.camera_name = parameter.value_to_string();
    } else if (parameter.get_name() == "camera_info_url") {
      m_parameters.camera_info_url = parameter.value_to_string();
    } else if (parameter.get_name() == "frame_id") {
      m_parameters.frame_id = parameter.value_to_string();
    } else if (parameter.get_name() == "framerate") {
      RCLCPP_INFO(this->get_logger(), "framerate: %f", parameter.as_double());
      m_parameters.framerate = parameter.as_double();
    } else if (parameter.get_name() == "image_height") {
      m_parameters.image_height = parameter.as_int();
    } else if (parameter.get_name() == "image_width") {
      m_parameters.image_width = parameter.as_int();
    } else if (parameter.get_name() == "io_method") {
      m_parameters.io_method_name = parameter.value_to_string();
    } else if (parameter.get_name() == "pixel_format") {
      m_parameters.pixel_format_name = parameter.value_to_string();
    } else if (parameter.get_name() == "av_device_format") {
      m_parameters.av_device_format = parameter.value_to_string();
    } else if (parameter.get_name() == "video_device") {
      m_parameters.device_name = resolve_device_path(parameter.value_to_string());
    } else if (parameter.get_name() == "brightness") {
      m_parameters.brightness = parameter.as_int();
    } else if (parameter.get_name() == "contrast") {
      m_parameters.contrast = parameter.as_int();
    } else if (parameter.get_name() == "saturation") {
      m_parameters.saturation = parameter.as_int();
    } else if (parameter.get_name() == "hue") {
      m_parameters.hue = parameter.as_int();
    } else if (parameter.get_name() == "gamma") {
      m_parameters.gamma = parameter.as_int();
    } else if (parameter.get_name() == "gain") {
      m_parameters.gain = parameter.as_int();
    } else if (parameter.get_name() == "power_line_frequency") {
      m_parameters.power_line_frequency = parameter.as_int();
    } else if (parameter.get_name() == "white_balance_automatic") {
      m_parameters.white_balance_automatic = parameter.as_bool();
    } else if (parameter.get_name() == "white_balance_temperature") {
      m_parameters.white_balance_temperature = parameter.as_int();
    } else if (parameter.get_name() == "sharpness") {
      m_parameters.sharpness = parameter.as_int();
    } else if (parameter.get_name() == "backlight_compensation") {
      m_parameters.backlight_compensation = parameter.as_int();
    } else if (parameter.get_name() == "auto_exposure") {
      m_parameters.auto_exposure = parameter.as_int();
    } else if (parameter.get_name() == "exposure_time_absolute") {
      m_parameters.exposure_time_absolute = parameter.as_int();
    } else if (parameter.get_name() == "exposure_dynamic_framerate") {
      m_parameters.exposure_dynamic_framerate = parameter.as_bool();
    } else {
      RCLCPP_WARN(this->get_logger(), "Invalid parameter name: %s", parameter.get_name().c_str());
    }
  }
}

/// @brief Send current parameters to V4L2 device
void UsbCamNode::set_v4l2_params()
{
  // set camera parameters
  if (m_parameters.brightness >= -64 || m_parameters.brightness <= 64) {
    RCLCPP_INFO(this->get_logger(), "Setting 'brightness' to %d", m_parameters.brightness);
    m_camera->set_v4l_parameter("brightness", m_parameters.brightness);
  }

  if (m_parameters.contrast >= 0 || m_parameters.contrast <= 64) {
    RCLCPP_INFO(this->get_logger(), "Setting 'contrast' to %d", m_parameters.contrast);
    m_camera->set_v4l_parameter("contrast", m_parameters.contrast);
  }

  if (m_parameters.saturation >= 0 || m_parameters.saturation <= 128) {
    RCLCPP_INFO(this->get_logger(), "Setting 'saturation' to %d", m_parameters.saturation);
    m_camera->set_v4l_parameter("saturation", m_parameters.saturation);
  }

  if (m_parameters.hue >= -40 || m_parameters.hue <= 40) {
    RCLCPP_INFO(this->get_logger(), "Setting 'hue' to %d", m_parameters.hue);
    m_camera->set_v4l_parameter("hue", m_parameters.hue);
  }

  if (m_parameters.gain >= 0 || m_parameters.gain <= 100) {
    RCLCPP_INFO(this->get_logger(), "Setting 'gain' to %d", m_parameters.gain);
    m_camera->set_v4l_parameter("gain", m_parameters.gain);
  }

  if (m_parameters.gamma >= 72 || m_parameters.gamma <= 500) {
    RCLCPP_INFO(this->get_logger(), "Setting 'gamma' to %d", m_parameters.gamma);
    m_camera->set_v4l_parameter("gamma", m_parameters.gamma);
  }

  if (m_parameters.power_line_frequency == 0) {
    RCLCPP_INFO(this->get_logger(), "Setting 'power_line_frequency' to disabled");
    m_camera->set_v4l_parameter("power_line_frequency", 0);
  } else if (m_parameters.power_line_frequency == 1) {
    RCLCPP_INFO(this->get_logger(), "Setting 'power_line_frequency' to %d Hz", 50);
    m_camera->set_v4l_parameter("power_line_frequency", 1);
  } else if (m_parameters.power_line_frequency == 2) {
    RCLCPP_INFO(this->get_logger(), "Setting 'power_line_frequency' to %d Hz", 60);
    m_camera->set_v4l_parameter("power_line_frequency", 2);
  } else {
    RCLCPP_WARN(this->get_logger(), "Settings for 'power_line_frequency' are wrong. No option for %d", 
    m_parameters.power_line_frequency);
  }

  if (m_parameters.white_balance_automatic) {
    RCLCPP_INFO(this->get_logger(), "Setting 'white_balance_automatic' to true");
    m_camera->set_v4l_parameter("white_balance_automatic", 1);
  } else {
    RCLCPP_INFO(this->get_logger(), "Setting 'white_balance_automatic' to false");
    m_camera->set_v4l_parameter("white_balance_automatic", 0);
    RCLCPP_INFO(this->get_logger(), "Setting 'white_balance_temperature' to %d", m_parameters.white_balance_temperature);
    m_camera->set_v4l_parameter("white_balance_temperature", m_parameters.white_balance_temperature);
  }

  if (m_parameters.backlight_compensation >= 0 || m_parameters.backlight_compensation <= 20) {
    RCLCPP_INFO(this->get_logger(), "Setting 'backlight_compensation' to %d", m_parameters.backlight_compensation);
    m_camera->set_v4l_parameter("backlight_compensation", m_parameters.backlight_compensation);
  }

  if (m_parameters.sharpness >= 0 || m_parameters.sharpness <= 6) {
    RCLCPP_INFO(this->get_logger(), "Setting 'sharpness' to %d", m_parameters.sharpness);
    m_camera->set_v4l_parameter("sharpness", m_parameters.sharpness);
  }

  if (m_parameters.auto_exposure == 1) {
    RCLCPP_INFO(this->get_logger(), "Setting 'auto_exposure' to Manual Mode");
    m_camera->set_v4l_parameter("auto_exposure", 1);
  } else if (m_parameters.auto_exposure == 3 ) {
    RCLCPP_INFO(this->get_logger(), "Setting 'auto_exposure' to Aperture Priority Mode");
    m_camera->set_v4l_parameter("auto_exposure", 3);
  } else {
    RCLCPP_WARN(this->get_logger(), "Settings for 'auto_exposure' are wrong. No option for %d", 
    m_parameters.auto_exposure);
  }

  if (m_parameters.exposure_time_absolute >= 1 || m_parameters.exposure_time_absolute <= 5000 ) {
    RCLCPP_INFO(this->get_logger(), "Setting 'exposure_time_absolute' to %d", m_parameters.exposure_time_absolute);
    m_camera->set_v4l_parameter("exposure_time_absolute", m_parameters.exposure_time_absolute);
  }

  if (m_parameters.exposure_dynamic_framerate) {
    RCLCPP_INFO(this->get_logger(), "Setting 'exposure_dynamic_framerate' to true");
    m_camera->set_v4l_parameter("exposure_dynamic_framerate", 1);
  } else {
    RCLCPP_INFO(this->get_logger(), "Setting 'exposure_dynamic_framerate' to false");
    m_camera->set_v4l_parameter("exposure_dynamic_framerate", 0);
  }

}

bool UsbCamNode::take_and_send_image()
{
  // Only resize if required
  if (sizeof(m_image_msg->data) != m_camera->get_image_size_in_bytes()) {
    m_image_msg->width = m_camera->get_image_width();
    m_image_msg->height = m_camera->get_image_height();
    m_image_msg->encoding = m_camera->get_pixel_format()->ros();
    m_image_msg->step = m_camera->get_image_step();
    if (m_image_msg->step == 0) {
      // Some formats don't have a linesize specified by v4l2
      // Fall back to manually calculating it step = size / height
      m_image_msg->step = m_camera->get_image_size_in_bytes() / m_image_msg->height;
    }
    m_image_msg->data.resize(m_camera->get_image_size_in_bytes());
  }

  // grab the image, pass image msg buffer to fill
  m_camera->get_image(reinterpret_cast<char *>(&m_image_msg->data[0]));

  auto stamp = m_camera->get_image_timestamp();
  m_image_msg->header.stamp.sec = stamp.tv_sec;
  m_image_msg->header.stamp.nanosec = stamp.tv_nsec;

  *m_camera_info_msg = m_camera_info->getCameraInfo();
  m_camera_info_msg->header = m_image_msg->header;
  m_image_publisher->publish(*m_image_msg, *m_camera_info_msg);
  return true;
}

bool UsbCamNode::take_and_send_image_mjpeg()
{
  // Only resize if required
  if (sizeof(m_compressed_img_msg->data) != m_camera->get_image_size_in_bytes()) {
    m_compressed_img_msg->format = "jpeg";
    m_compressed_img_msg->data.resize(m_camera->get_image_size_in_bytes());
  }

  // grab the image, pass image msg buffer to fill
  m_camera->get_image(reinterpret_cast<char *>(&m_compressed_img_msg->data[0]));

  auto stamp = m_camera->get_image_timestamp();
  m_compressed_img_msg->header.stamp.sec = stamp.tv_sec;
  m_compressed_img_msg->header.stamp.nanosec = stamp.tv_nsec;

  *m_camera_info_msg = m_camera_info->getCameraInfo();
  m_camera_info_msg->header = m_compressed_img_msg->header;

  m_compressed_image_publisher->publish(*m_compressed_img_msg);
  m_compressed_cam_info_publisher->publish(*m_camera_info_msg);
  return true;
}

rcl_interfaces::msg::SetParametersResult UsbCamNode::parameters_callback(
  const std::vector<rclcpp::Parameter> & parameters)
{
  RCLCPP_DEBUG(this->get_logger(), "Setting parameters for %s", m_parameters.camera_name.c_str());
  m_timer->reset();
  assign_params(parameters);
  set_v4l2_params();
  rcl_interfaces::msg::SetParametersResult result;
  result.successful = true;
  result.reason = "success";
  return result;
}

void UsbCamNode::update()
{
  if (m_camera->is_capturing()) {
    // If the camera exposure longer higher than the framerate period
    // then that caps the framerate.
    // auto t0 = now();
    bool isSuccessful = (m_parameters.pixel_format_name == "mjpeg") ?
      take_and_send_image_mjpeg() :
      take_and_send_image();
    if (!isSuccessful) {
      RCLCPP_WARN_ONCE(this->get_logger(), "USB camera did not respond in time.");
    }
  }
}
}  // namespace usb_cam


#include "rclcpp_components/register_node_macro.hpp"
RCLCPP_COMPONENTS_REGISTER_NODE(usb_cam::UsbCamNode)
