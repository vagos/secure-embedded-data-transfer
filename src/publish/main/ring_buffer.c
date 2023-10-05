/**
 * ring_buffer.c
 *
 */

#include <stdio.h>
#include <string.h>
#include "ring_buffer.h"


/**
-----------------------------------------------
@des:  		创建环形缓冲区
@param[in]  max_num 缓冲区最大可以存储的帧数
@param[in]  size    单帧尺寸(字节)
@return   	缓冲区指针	    
------------------------------------------------
*/

extern RING_BUFFER_T* ring_buffer_create(uint32_t max_num, uint32_t size)
{
	if((max_num*size) > RING_MAX_BUFSIZE)
	{
		printf("create ring buffer fail[%d]!\r\n",RING_MAX_BUFSIZE);
		return NULL;
	}
    //创建环形缓冲区本身
	RING_BUFFER_T *ringCache = malloc(sizeof(RING_BUFFER_T));
	if(ringCache==NULL)
		return NULL;
	//创建环形缓冲区的数据存储区
	ringCache->buffer = malloc(max_num * size);
	if(ringCache->buffer==NULL)
	{
		free(ringCache);
		return NULL;
	}
	ringCache->max_num = max_num;
	ringCache->size = size;
	ringCache->r_index = 0;
	ringCache->w_index = 0;
	return ringCache;
}

/**
-----------------------------------------------
@des:  		清空环形缓冲区
@param[in]  ringCache 环形缓冲区
@return   	none	    
------------------------------------------------
*/

void ring_buffer_clear(RING_BUFFER_T* ringCache)
{
	ringCache->r_index = 0;
	ringCache->w_index = 0;
}

/**
-----------------------------------------------
@des:  		环形缓冲区入栈
@param[in]  ringCache 环形缓冲区
@param[in]  data    数据缓存
@param[in]  nBytes  缓存尺寸(字节)
@return   	入栈的帧数	    
------------------------------------------------
*/

int ring_buffer_push(RING_BUFFER_T* ringCache, uint8_t *data, int nBytes)
{
	int i=0;
	if(ringCache==NULL || data == NULL)
		return -1;
    int n_frame = nBytes/ringCache->size;            //得到数据共有多少帧
    for(i=0; i<n_frame; i++)
    {
        //环形缓冲区可以读完,但是不可写尽[写的时候留一位]
		if(((ringCache->w_index+1)%ringCache->max_num) == ringCache->r_index)
		{
			break;
		}
	    else
		{
			memcpy((ringCache->buffer + ringCache->w_index * ringCache->size), (data+i*ringCache->size), ringCache->size);
			ringCache->w_index = (ringCache->w_index+1)%ringCache->max_num;
		}
    }
	return i;
}

/**
-----------------------------------------------
@des:  		环形缓冲区出栈
@param[in]  ringCache 环形缓冲区
@param[out] data    数据缓存
@param[in]  nBytes  缓存尺寸(字节)
@return   	出栈的帧数    
------------------------------------------------
*/

int ring_buffer_pop(RING_BUFFER_T* ringCache, uint8_t data[], int nBytes)
{
    int i=0;
    if(ringCache==NULL || data==NULL)
        return -1;
    int n_frame = nBytes / ringCache->size;           //得到数据共有多少帧
    for(i=0; i<n_frame; i++)
    {
        //环形缓冲区可以读完,但是不可写尽[写的时候留一位]
        if(ringCache->r_index==ringCache->w_index) 
        {
            break;
        }
        else
        {
             memcpy((data + i * ringCache->size), (ringCache->buffer + ringCache->r_index * ringCache->size), ringCache->size);
             ringCache->r_index = (ringCache->r_index+1)%ringCache->max_num;
        }
    }
    return i;
}

