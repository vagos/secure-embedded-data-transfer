/**
 * ring_buffer.h
 *
 */

#ifndef RING_BUFFER_H_
#define RING_BUFFER_H_


#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>



#define  	RING_MAX_BUFSIZE  	128U
    
typedef struct
{
	uint32_t 	w_index;                         //ring buffer wrie index 
	uint32_t 	r_index;                         //ring buffer read index 
	uint32_t    size;							 //一条消息的尺寸(字节)
	uint32_t    max_num;						 //最大存储几条消息
	uint8_t     *buffer;                     	 //data ring buffer 
}RING_BUFFER_T;
typedef struct 
{
	uint8_t mic_data[2];
	uint8_t mic_length;
}MQTT_MSG_T;

    
//环形缓冲区可以读完,但是不可写尽[写的时候留一位]

#define RINF_BUF_IS_FULL(ring)  (ring->r_index==((ring->w_index+1)%ring->max_num))
#define RING_BUF_IS_EMPTY(ring) (ring->r_index==ring->w_index)

extern RING_BUFFER_T* ring_buffer_create(uint32_t max_num, uint32_t size);
extern void ring_buffer_clear(RING_BUFFER_T* ringCache);
extern int ring_buffer_push(RING_BUFFER_T* ringCache, uint8_t data[], int nBytes);
extern int ring_buffer_pop(RING_BUFFER_T* ringCache, uint8_t data[], int nBytes);





#ifdef __cplusplus
}
#endif


#endif /* RING_BUFFER_H_ */

