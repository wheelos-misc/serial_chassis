/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 * Modified by WheelOS, 2025. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/serial/common/serial_stream.h"

#include "cyber/common/log.h"

namespace apollo {
namespace serial {

constexpr static int kMaxEmptyReadCount = 10;

SerialStream::SerialStream(const char* device_name, speed_t baud_rate,
                           uint32_t timeout_usec)
    : device_name_(device_name),
      baud_rate_(baud_rate),
      bytesize_(8),
      parity_(0),
      stopbits_(1),
      flowcontrol_(0),
      timeout_usec_(timeout_usec),
      fd_(-1),
      errno_(0),
      is_open_(false) {
  if (device_name_.empty()) {
    status_ = Status::ERROR;
  }
}

SerialStream::~SerialStream() { this->close(); }

void SerialStream::open(void) {
  int fd = 0;
  fd = ::open(device_name_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd == -1) {
    switch (errno) {
      case EINTR:
        // Recurse because this is a recoverable error.
        return open();

      case ENFILE:
      case EMFILE:
      default:
        AERROR << "Open device " << device_name_
               << " failed, error: " << strerror(errno);
        return;
    }
  }

  if (!configure_port(fd)) {
    ::close(fd);
    return;
  }

  fd_ = fd;
  is_open_ = true;
}

bool SerialStream::configure_port(int fd) {
  if (fd < 0) {
    return false;
  }

  struct termios options;  // The options for the file descriptor
  if (tcgetattr(fd, &options) == -1) {
    AERROR << "tcgetattr failed.";
    return false;
  }

  // set up raw mode / no echo / binary
  options.c_cflag |= (tcflag_t)(CLOCAL | CREAD);
  options.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL |
                                  ISIG | IEXTEN);  // |ECHOPRT

  options.c_oflag &= (tcflag_t) ~(OPOST);
  options.c_iflag &= (tcflag_t) ~(INLCR | IGNCR | ICRNL | IGNBRK);

#ifdef IUCLC
  options.c_iflag &= (tcflag_t)~IUCLC;
#endif

#ifdef PARMRK
  options.c_iflag &= (tcflag_t)~PARMRK;
#endif

#ifdef BSD_SOURCE_  // depend glibc
  ::cfsetspeed(&options, baud_rate_);
#else
  ::cfsetispeed(&options, baud_rate_);
  ::cfsetospeed(&options, baud_rate_);
#endif

  // setup char len
  options.c_cflag &= (tcflag_t)~CSIZE;

  // eight bits
  options.c_cflag |= CS8;

  // setup stopbits:stopbits_one
  options.c_cflag &= (tcflag_t) ~(CSTOPB);

  // setup parity: parity_none
  options.c_iflag &= (tcflag_t) ~(INPCK | ISTRIP);
  options.c_cflag &= (tcflag_t) ~(PARENB | PARODD);

// setup flow control : flowcontrol_none
// xonxoff
#ifdef IXANY
  options.c_iflag &= (tcflag_t) ~(IXON | IXOFF | IXANY);
#else
  options.c_iflag &= (tcflag_t) ~(IXON | IXOFF);
#endif

// rtscts
#ifdef CRTSCTS
  options.c_cflag &= static_cast<uint64_t>(~(CRTSCTS));
#elif defined CNEW_RTSCTS
  options.c_cflag &= static_cast<uint64_t>(~(CNEW_RTSCTS));
#else
#error "OS Support seems wrong."
#endif

  // http://www.unixwiz.net/techtips/termios-vmin-vtime.html
  // this basically sets the read call up to be a polling read,
  // but we are using select to ensure there is data available
  // to read before each call, so we should never needlessly poll
  options.c_cc[VMIN] = 0;
  options.c_cc[VTIME] = 0;

  // activate settings
  ::tcsetattr(fd, TCSANOW, &options);

  // Update byte_time_ based on the new settings.
  uint32_t bit_time_us = static_cast<uint32_t>(1e6 / 115200);
  byte_time_us_ = bit_time_us * (1 + bytesize_ + parity_ + stopbits_);
  return true;
}

bool SerialStream::Connect() {
  if (!is_open_) {
    this->open();
    if (!is_open_) {
      status_ = Status::ERROR;
      errno_ = errno;
      return false;
    }
  }

  if (status_ == Status::CONNECTED) {
    return true;
  }

  status_ = Status::CONNECTED;
  return true;
}

void SerialStream::close(void) {
  if (is_open_) {
    ::close(fd_);
    fd_ = -1;
    is_open_ = false;
    status_ = Status::DISCONNECTED;
  }
}

bool SerialStream::Disconnect() {
  if (!is_open_) {
    // not open
    return false;
  }

  this->close();
  return true;
}

void SerialStream::check_remove() {
  char data = 0;
  ssize_t nsent = ::write(fd_, &data, 0);
  if (nsent < 0) {
    AERROR << "Serial stream detect write failed, error: " << strerror(errno);
    switch (errno) {
      case EBADF:
      case EIO:
        status_ = Status::DISCONNECTED;
        AERROR << "Device " << device_name_ << " removed.";
        Disconnect();
        break;
    }
  }
}

size_t SerialStream::read(uint8_t* buffer, size_t max_length) {
  if (!is_open_) {
    if (!Connect()) {
      return 0;
    }
    AINFO << "Connect " << device_name_ << " success.";
  }

  static int empty_read_count = 0;
  ssize_t bytes_read = 0;
  ssize_t bytes_current_read = 0;

  wait_readable(10000);  // wait 10ms

  while (max_length > 0) {
    bytes_current_read = ::read(fd_, buffer, max_length);
    if (bytes_current_read < 0) {
      switch (errno) {
        case EAGAIN:
        case EINVAL:
          bytes_current_read = 0;
          break;

        case EBADF:
        case EIO:
          AERROR << "Serial stream read data failed, error: "
                 << strerror(errno);
          Disconnect();
          if (Connect()) {
            AINFO << "Reconnect " << device_name_ << " success.";
            bytes_current_read = 0;
            break;  // has recoverable
          }

        default:
          AERROR << "Serial stream read data failed, error: " << strerror(errno)
                 << ", errno: " << errno;
          status_ = Status::ERROR;
          errno_ = errno;
          return bytes_read;
      }
    }

    if (bytes_current_read == 0) {
      if (bytes_read == 0) {
        ++empty_read_count;
        if (empty_read_count > kMaxEmptyReadCount) {
          check_remove();
        }
      }
      return bytes_read;
    }
    // Reset count when read data success
    empty_read_count = 0;

    max_length -= bytes_current_read;
    buffer += bytes_current_read;
    bytes_read += bytes_current_read;
  }

  return bytes_read;
}

size_t SerialStream::write(const uint8_t* data, size_t length) {
  if (!is_open_) {
    if (!Connect()) {
      return 0;
    }
    AINFO << "Connect " << device_name_ << " success.";
  }

  size_t total_nsent = 0;
  size_t delay_times = 0;

  while ((length > 0) && (delay_times < 5)) {
    ssize_t nsent = ::write(fd_, data, length);
    if (nsent < 0) {
      AERROR << "Serial stream write data failed, error: " << strerror(errno);
      switch (errno) {
        case EAGAIN:
        case EINVAL:
          nsent = 0;
          break;

        case EBADF:
        case EIO:
          Disconnect();
          if (Connect()) {
            AINFO << "Reconnect " << device_name_ << "success.";
            nsent = 0;
            break;  // has recoverable
          }

        default:
          status_ = Status::ERROR;
          errno_ = errno;
          return total_nsent;
      }
    }

    if (nsent == 0) {
      if (!wait_writable(byte_time_us_)) {
        break;
      }
      ++delay_times;
      continue;
    }

    total_nsent += nsent;
    length -= nsent;
    data += nsent;
  }

  return total_nsent;
}

bool SerialStream::wait_readable(uint32_t timeout_us) {
  // Setup a select call to block for serial data or a timeout
  timespec timeout_ts;
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(fd_, &readfds);

  timeout_ts.tv_sec = timeout_us / 1000000;
  timeout_ts.tv_nsec = (timeout_us % 1000000) * 1000;
  int r = pselect(fd_ + 1, &readfds, NULL, NULL, &timeout_ts, NULL);
  if (r <= 0) {
    return false;
  }

  // This shouldn't happen, if r > 0 our fd has to be in the list!
  if (!FD_ISSET(fd_, &readfds)) {
    return false;
  }
  // Data available to read.
  return true;
}

bool SerialStream::wait_writable(uint32_t timeout_us) {
  // Setup a select call to block for serial data or a timeout
  timespec timeout_ts;
  fd_set writefds;
  FD_ZERO(&writefds);
  FD_SET(fd_, &writefds);

  timeout_ts.tv_sec = timeout_us / 1000000;
  timeout_ts.tv_nsec = (timeout_us % 1000000) * 1000;
  int r = pselect(fd_ + 1, NULL, &writefds, NULL, &timeout_ts, NULL);
  if (r <= 0) {
    return false;
  }

  // This shouldn't happen, if r > 0 our fd has to be in the list!
  if (!FD_ISSET(fd_, &writefds)) {
    return false;
  }
  // Data available to write.
  return true;
}

}  // namespace serial
}  // namespace apollo
