#ifndef __RLV_H__
#define __RLV_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RLV_MAX_LINE_WIDTH 240
#define RLV_FRAME_CACHE_SIZE 4
#define RLV_BIT_CACHE_SIZE 32

// 默认数据缓存大小，可以在运行时修改
#define RLV_DEFAULT_DATA_CACHE_SIZE 1024

typedef struct {
    uint8_t width;
    uint8_t height;
    uint8_t fps;
    uint16_t frame_count;
    uint8_t frame_table_bits;
    uint8_t unit_bits;
} rlv_header_t;

typedef struct {
    const uint8_t *data;
    uint32_t data_size;
    uint32_t byte_pos;
    uint8_t bit_pos;
    uint32_t cache;
    uint8_t cache_valid_bits;
} rlv_bit_reader_t;

typedef struct {
    uint16_t frame_index;
    uint32_t offset;
} rlv_frame_cache_entry_t;

typedef struct {
    rlv_frame_cache_entry_t entries[4];
    uint8_t next_slot;
} rlv_frame_cache_t;

typedef struct _rlv_decoder_t rlv_decoder_t;

typedef void (*rlv_line_buffer_callback_t)(rlv_decoder_t *decoder, uint16_t y, uint16_t *line_buffer, uint16_t width);

struct _rlv_decoder_t {
    rlv_header_t header;
    
    // 文件访问
    void *file_handle;              // 文件句柄 (mp_file_t*)
    uint8_t *file_data;             // 内存数据（如果是字节数组）
    uint32_t file_size;
    bool is_file_mode;              // true=文件模式，false=内存模式
    
    // 缓存
    uint8_t *frame_table_cache;     // 帧表缓存
    uint32_t frame_table_size;      // 帧表大小
    uint8_t *data_cache;            // 数据缓存
    uint32_t data_cache_offset;     // 缓存的数据偏移
    uint32_t data_cache_size;       // 缓存的数据大小
    uint32_t data_cache_capacity;   // 数据缓存容量（可配置）
    
    // FPS统计
    bool fps_print_enabled;         // 是否启用FPS打印
    uint32_t fps_last_time;         // 上次FPS统计时间
    uint32_t fps_frame_count;       // 帧计数器
    
    uint16_t current_frame;
    uint8_t *line_buffer;
    uint16_t buffer_size;
    rlv_bit_reader_t bit_reader;
    rlv_frame_cache_t frame_cache;
    void *user_data;
    const char *error_msg;
};

rlv_decoder_t *rlv_decoder_new(void);
rlv_decoder_t *rlv_decoder_new_with_cache_size(uint32_t data_cache_size);
void rlv_decoder_destroy(rlv_decoder_t *decoder);

// 内存模式初始化（原有接口）
int rlv_decoder_init(rlv_decoder_t *decoder, const uint8_t *data, uint32_t size);

// 文件模式初始化（新接口）
int rlv_decoder_init_file(rlv_decoder_t *decoder, void *file_handle);

int rlv_decoder_decode_frame_to_buffer(rlv_decoder_t *decoder, uint16_t frame_index,
                                      uint16_t *frame_buffer, uint16_t fg_color, uint16_t bg_color);
int rlv_decoder_decode_frame_line_by_line(rlv_decoder_t *decoder, uint16_t frame_index,
                                         rlv_line_buffer_callback_t line_callback,
                                         uint16_t fg_color, uint16_t bg_color);

// 运行时配置函数
void rlv_decoder_set_data_cache_size(rlv_decoder_t *decoder, uint32_t cache_size);
uint32_t rlv_decoder_get_data_cache_size(rlv_decoder_t *decoder);
void rlv_decoder_set_fps_print(rlv_decoder_t *decoder, bool enabled);
bool rlv_decoder_get_fps_print(rlv_decoder_t *decoder);

void rlv_decoder_set_user_data(rlv_decoder_t *decoder, void *user_data);
void *rlv_decoder_get_user_data(rlv_decoder_t *decoder);
const char *rlv_decoder_get_error(rlv_decoder_t *decoder);

// 内部辅助函数
int rlv_read_data(rlv_decoder_t *decoder, uint32_t offset, uint8_t *buffer, uint32_t size);
uint32_t rlv_get_frame_offset(rlv_decoder_t *decoder, uint16_t frame_index);

uint32_t get_ticks_ms(void);
void delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* __RLV_H__ */