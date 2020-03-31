#ifndef __GATHER_HPP__
#define __GATHER_HPP__

#include <stdint.h>

/**
  * @brief  标识
  */
enum __flags {
	PH_TRANSMIT = 0,//透传
	PH_QUERY,//查询
	PH_REJECT,//强制下线
	
	RE_OK,//成功
	RE_ONLINE,//在线
	RE_OFFLINE,//不在线
	RE_FAILD,//失败
};

/**
  * @brief  包头
  */
typedef struct __packet_header {
	uint64_t id;
	char name[32];
	uint8_t flag;
} packet_header;

#endif
