#pragma once
#include <cstdint>

namespace ugdr{
namespace common{

constexpr uint32_t UGDR_MAX_DEV_NAME_LEN = 64;
constexpr uint32_t UGDR_MAX_SHRING_NAME_LEN = UGDR_MAX_DEV_NAME_LEN + 64; //dev_name + "_xq_shring_xxx"
constexpr uint32_t UGDR_MAX_SEND_FDS_NUM = 2;

}
}
