/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <iostream>
#include <memory>
#include <unordered_set>
#include <cstdio>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "camera.hh"
#include "exception.hh"

using namespace std;

unordered_set<uint32_t> SUPPORTED_FORMATS {
  { V4L2_PIX_FMT_NV12 }
};

Camera::Camera( const uint16_t width, const uint16_t height,
                const uint32_t pixel_format, const string device )
  : width_( width ), height_( height ),
    camera_fd_( SystemCall( "open camera", open( device.c_str(), O_RDWR ) ) ),
    mmap_region_(), buffer_info_(), type_()
{
  v4l2_capability cap;
  SystemCall( "ioctl", ioctl( camera_fd_.fd_num(), VIDIOC_QUERYCAP, &cap ) );

  if ( not ( cap.capabilities & V4L2_CAP_VIDEO_CAPTURE ) ) {
    throw runtime_error( "this device does not handle video capture" );
  }

  if ( not SUPPORTED_FORMATS.count( pixel_format ) ) {
    throw runtime_error( "this pixel format is not implemented" );
  }

  /* setting the output format and size */
  v4l2_format format;
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.pixelformat = pixel_format;
  format.fmt.pix.width = width;
  format.fmt.pix.height = height;

  SystemCall( "setting format", ioctl( camera_fd_.fd_num(), VIDIOC_S_FMT, &format ) );

  if ( format.fmt.pix.pixelformat != pixel_format or
       format.fmt.pix.width != width_ or
       format.fmt.pix.height != height_ ) {
    throw runtime_error( "couldn't configure the camera with the given format" );
  }

  /* tell the v4l2 about our buffers */
  v4l2_requestbuffers buf_request;
  buf_request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf_request.memory = V4L2_MEMORY_MMAP;
  buf_request.count = 1;

  SystemCall( "buffer request", ioctl( camera_fd_.fd_num(), VIDIOC_REQBUFS, &buf_request ) );

  /* allocate buffers */
  memset( &buffer_info_, 0, sizeof( buffer_info_ ) );

  buffer_info_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer_info_.memory = V4L2_MEMORY_MMAP;
  buffer_info_.index = 0;

  type_ = buffer_info_.type;
  SystemCall( "allocate buffer", ioctl( camera_fd_.fd_num(), VIDIOC_QUERYBUF, &buffer_info_ ) );

  /* mmap the thing */
  mmap_region_ = make_unique<MMap_Region>( buffer_info_.length, PROT_READ | PROT_WRITE,
                                                MAP_SHARED, camera_fd_.fd_num() );

  SystemCall( "stream on", ioctl( camera_fd_.fd_num(), VIDIOC_STREAMON, &type_ ) );
}

Camera::~Camera()
{
  SystemCall( "stream off", ioctl( camera_fd_.fd_num(), VIDIOC_STREAMOFF, &type_ ) );
}

Optional<RasterHandle> Camera::get_next_frame()
{
  MutableRasterHandle raster_handle { width_, height_ };
  auto & raster = raster_handle.get();

  buffer_info_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer_info_.memory = V4L2_MEMORY_MMAP;
  buffer_info_.index = 0;

  SystemCall( "queue", ioctl( camera_fd_.fd_num(), VIDIOC_QBUF, &buffer_info_ ) );

  memcpy( &raster.Y().at( 0, 0 ), mmap_region_->addr(), width_ * height_ );

  uint8_t * src_chroma_start = mmap_region_->addr() + width_ * height_;
  uint8_t * dst_cb_start = &raster.U().at( 0, 0 );
  uint8_t * dst_cr_start = &raster.V().at( 0, 0 );

  size_t chroma_length = width_ * height_ / 4;

  for ( size_t i = 0; i < chroma_length; i++ ) {
    dst_cb_start[ i ] = src_chroma_start[ 2 * i ];
    dst_cr_start[ i ] = src_chroma_start[ 2 * i + 1 ];
  }

  SystemCall( "dequeue buffer", ioctl( camera_fd_.fd_num(), VIDIOC_DQBUF, &buffer_info_ ) );

  return RasterHandle{ move( raster_handle ) };
}
