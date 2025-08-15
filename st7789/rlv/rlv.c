#include "rlv.h"
#include <stdlib.h>
#include <string.h>
#include "py/mphal.h"

// MicroPython文件操作函数声明
typedef struct _mp_file_t mp_file_t;
extern int mp_seek(mp_file_t *fp, int offset, int whence);
extern int mp_readinto(mp_file_t *fp, void *buf, size_t size);
#define SEEK_SET 0

#define RLV_HEADER_SIZE 6
#define MAX_LINE_BUFFER_SIZE 512

// 前置声明
static void rlv_update_fps_stats(rlv_decoder_t *decoder);

static inline void rlv_bit_reader_init(rlv_bit_reader_t *reader, const uint8_t *data, uint32_t size) {
    reader->data = data;
    reader->data_size = size;
    reader->byte_pos = 0;
    reader->bit_pos = 0;
    reader->cache = 0;
    reader->cache_valid_bits = 0;
}

static inline void rlv_bit_reader_seek(rlv_bit_reader_t *reader, uint32_t bit_offset) {
    reader->byte_pos = bit_offset / 8;
    reader->bit_pos = bit_offset % 8;
    reader->cache = 0;
    reader->cache_valid_bits = 0;
}

static inline void rlv_fill_cache(rlv_bit_reader_t *reader) {
    reader->cache = 0;
    reader->cache_valid_bits = 0;
    
    for (int i = 0; i < 4 && reader->byte_pos + i < reader->data_size; i++) {
        reader->cache = (reader->cache << 8) | reader->data[reader->byte_pos + i];
        reader->cache_valid_bits += 8;
    }
    
    if (reader->bit_pos > 0) {
        reader->cache <<= reader->bit_pos;
        reader->cache_valid_bits -= reader->bit_pos;
    }
}

static inline uint32_t rlv_read_bits(rlv_bit_reader_t *reader, uint8_t bit_count) {
    if (bit_count == 0) return 0;
    if (bit_count > 24) return 0;
    
    if (reader->cache_valid_bits < bit_count) {
        rlv_fill_cache(reader);
    }
    
    uint32_t mask = (1UL << bit_count) - 1;
    uint32_t result = (reader->cache >> (32 - bit_count)) & mask;
    
    reader->cache <<= bit_count;
    reader->cache_valid_bits -= bit_count;
    
    reader->bit_pos += bit_count;
    while (reader->bit_pos >= 8) {
        reader->bit_pos -= 8;
        reader->byte_pos++;
    }
    
    return result;
}

static uint32_t read_bits(const uint8_t *data, uint32_t bit_offset, uint8_t bit_count) {
    rlv_bit_reader_t reader;
    rlv_bit_reader_init(&reader, data, UINT32_MAX);
    rlv_bit_reader_seek(&reader, bit_offset);
    return rlv_read_bits(&reader, bit_count);
}

static int parse_header(rlv_decoder_t *decoder, const uint8_t *data) {
    if (!decoder || !data) {
        decoder->error_msg = "Invalid parameters";
        return -1;
    }
    
    decoder->header.width = data[0];
    decoder->header.height = data[1];
    decoder->header.fps = data[2];
    decoder->header.frame_count = (data[3] << 8) | data[4];
    
    uint8_t last_byte = data[5];
    decoder->header.frame_table_bits = (last_byte >> 3) & 0x1F;
    decoder->header.unit_bits = last_byte & 0x07;
    
    if (decoder->header.width == 0 || decoder->header.height == 0) {
        decoder->error_msg = "Invalid dimensions";
        return -1;
    }
    
    return 0;
}

static void rlv_frame_cache_init(rlv_frame_cache_t *cache) {
    memset(cache, 0, sizeof(rlv_frame_cache_t));
}

static uint32_t rlv_frame_cache_get(rlv_frame_cache_t *cache, uint16_t frame_index) {
    for (int i = 0; i < 4; i++) {
        if (cache->entries[i].frame_index == frame_index) {
            return cache->entries[i].offset;
        }
    }
    return UINT32_MAX;
}

static void rlv_frame_cache_put(rlv_frame_cache_t *cache, uint16_t frame_index, uint32_t offset) {
    cache->entries[cache->next_slot].frame_index = frame_index;
    cache->entries[cache->next_slot].offset = offset;
    cache->next_slot = (cache->next_slot + 1) & 3;
}

static uint32_t get_frame_start_offset(rlv_decoder_t *decoder, uint16_t frame_index) {
    if (frame_index >= decoder->header.frame_count) {
        return 0;
    }
    
    if (decoder->header.frame_table_bits == 0) {
        return 0;
    }
    
    uint32_t cached_offset = rlv_frame_cache_get(&decoder->frame_cache, frame_index);
    if (cached_offset != UINT32_MAX) {
        return cached_offset;
    }
    
    uint32_t offset = 0;
    
    if (decoder->is_file_mode) {
        // 文件模式：从缓存的帧表中读取
        if (decoder->frame_table_cache) {
            uint32_t bit_offset = frame_index * decoder->header.frame_table_bits;
            rlv_bit_reader_t reader;
            rlv_bit_reader_init(&reader, decoder->frame_table_cache, decoder->frame_table_size);
            rlv_bit_reader_seek(&reader, bit_offset);
            offset = rlv_read_bits(&reader, decoder->header.frame_table_bits);
        }
    } else {
        // 内存模式：直接从文件数据读取
        uint32_t frame_table_start = RLV_HEADER_SIZE;
        uint32_t bit_offset = frame_table_start * 8 + frame_index * decoder->header.frame_table_bits;
        offset = read_bits(decoder->file_data, bit_offset, decoder->header.frame_table_bits);
    }
    
    rlv_frame_cache_put(&decoder->frame_cache, frame_index, offset);
    
    return offset;
}

int rlv_decoder_decode_frame_to_buffer(rlv_decoder_t *decoder, uint16_t frame_index,
                                      uint16_t *frame_buffer, uint16_t fg_color, uint16_t bg_color) {
    if (!decoder || !frame_buffer || frame_index >= decoder->header.frame_count) {
        if (decoder) decoder->error_msg = "Invalid parameters";
        return -1;
    }
    
    uint32_t frame_start_unit = get_frame_start_offset(decoder, frame_index);
    uint32_t frame_table_bytes = (decoder->header.frame_count * decoder->header.frame_table_bits + 7) / 8;
    uint32_t data_start = RLV_HEADER_SIZE + frame_table_bytes;
    
    if (decoder->header.unit_bits == 0) {
        uint32_t frame_size = decoder->header.width * decoder->header.height;
        uint32_t bit_offset = data_start * 8 + frame_index * frame_size;
        
        rlv_bit_reader_init(&decoder->bit_reader, decoder->file_data, decoder->file_size);
        rlv_bit_reader_seek(&decoder->bit_reader, bit_offset);
        
        for (uint32_t i = 0; i < frame_size; i++) {
            uint8_t pixel = rlv_read_bits(&decoder->bit_reader, 1);
            frame_buffer[i] = pixel ? fg_color : bg_color;
        }
    } else {
        uint8_t total_unit_bits = decoder->header.unit_bits + 1;
        uint32_t bit_offset = data_start * 8 + frame_start_unit * total_unit_bits;
        
        rlv_bit_reader_init(&decoder->bit_reader, decoder->file_data, decoder->file_size);
        rlv_bit_reader_seek(&decoder->bit_reader, bit_offset);
        
        uint32_t pixels_decoded = 0;
        uint32_t frame_size = decoder->header.width * decoder->header.height;
        
        while (pixels_decoded < frame_size) {
            uint32_t unit_value = rlv_read_bits(&decoder->bit_reader, total_unit_bits);
            uint8_t color = (unit_value >> decoder->header.unit_bits) & 1;
            uint32_t length = (unit_value & ((1 << decoder->header.unit_bits) - 1)) + 1;
            
            uint16_t pixel_color = color ? fg_color : bg_color;
            uint32_t pixels_to_fill = (length + pixels_decoded > frame_size) ?
                                     (frame_size - pixels_decoded) : length;
            
            for (uint32_t i = 0; i < pixels_to_fill; i++) {
                frame_buffer[pixels_decoded + i] = pixel_color;
            }
            pixels_decoded += pixels_to_fill;
        }
    }
    
    return 0;
}

// 智能数据读取函数，使用缓存优化
static int rlv_read_frame_data(rlv_decoder_t *decoder, uint32_t offset, uint8_t *buffer, uint32_t size) {
    if (decoder->is_file_mode) {
        // 检查是否可以从缓存中读取
        if (decoder->data_cache_offset != UINT32_MAX &&
            offset >= decoder->data_cache_offset &&
            offset + size <= decoder->data_cache_offset + decoder->data_cache_size) {
            // 缓存命中
            memcpy(buffer, decoder->data_cache + (offset - decoder->data_cache_offset), size);
            return 0;
        }
        
        // 缓存未命中，需要重新加载
        uint32_t cache_start = offset;
        uint32_t cache_size = (size > decoder->data_cache_capacity) ? size : decoder->data_cache_capacity;
        
        if (rlv_read_data(decoder, cache_start, decoder->data_cache, cache_size) != 0) {
            return -1;
        }
        
        decoder->data_cache_offset = cache_start;
        decoder->data_cache_size = cache_size;
        
        // 从缓存中复制数据
        memcpy(buffer, decoder->data_cache, size);
        return 0;
    } else {
        // 内存模式：直接读取
        return rlv_read_data(decoder, offset, buffer, size);
    }
}

int rlv_decoder_decode_frame_line_by_line(rlv_decoder_t *decoder, uint16_t frame_index,
                                         rlv_line_buffer_callback_t line_callback,
                                         uint16_t fg_color, uint16_t bg_color) {
    if (!decoder || !line_callback || frame_index >= decoder->header.frame_count) {
        if (decoder) decoder->error_msg = "Invalid parameters";
        return -1;
    }
    
    // 更新FPS统计
    rlv_update_fps_stats(decoder);
    
    uint16_t *line_buffer = (uint16_t *)decoder->line_buffer;
    uint16_t max_width = decoder->buffer_size / 2;
    
    if (decoder->header.width > max_width) {
        decoder->error_msg = "Frame width exceeds line buffer size";
        return -1;
    }
    
    uint32_t frame_start_unit = get_frame_start_offset(decoder, frame_index);
    uint32_t frame_table_bytes = decoder->is_file_mode ? decoder->frame_table_size :
                                (decoder->header.frame_count * decoder->header.frame_table_bits + 7) / 8;
    uint32_t data_start = RLV_HEADER_SIZE + frame_table_bytes;
    
    if (decoder->header.unit_bits == 0) {
        // 未压缩模式：每个像素1位
        uint32_t frame_bits = decoder->header.width * decoder->header.height;
        uint32_t frame_bytes = (frame_bits + 7) / 8;
        uint32_t bit_offset = data_start * 8 + frame_index * frame_bits;
        uint32_t byte_offset = bit_offset / 8;
        
        // 读取整帧数据到临时缓冲区
        uint8_t *frame_data = m_malloc(frame_bytes);
        if (!frame_data) {
            decoder->error_msg = "Out of memory for frame data";
            return -1;
        }
        
        if (rlv_read_frame_data(decoder, byte_offset, frame_data, frame_bytes) != 0) {
            m_free(frame_data);
            decoder->error_msg = "Failed to read frame data";
            return -1;
        }
        
        rlv_bit_reader_init(&decoder->bit_reader, frame_data, frame_bytes);
        rlv_bit_reader_seek(&decoder->bit_reader, bit_offset % 8);
        
        for (uint16_t y = 0; y < decoder->header.height; y++) {
            for (uint16_t x = 0; x < decoder->header.width; x++) {
                uint8_t pixel = rlv_read_bits(&decoder->bit_reader, 1);
                line_buffer[x] = pixel ? fg_color : bg_color;
            }
            line_callback(decoder, y, line_buffer, decoder->header.width);
        }
        
        m_free(frame_data);
    } else {
        // RLE压缩模式：需要流式解码
        uint8_t total_unit_bits = decoder->header.unit_bits + 1;
        uint32_t bit_offset = data_start * 8 + frame_start_unit * total_unit_bits;
        uint32_t byte_offset = bit_offset / 8;
        
        // 估算需要的数据量（保守估计）
        uint32_t estimated_bytes = (decoder->header.width * decoder->header.height * total_unit_bits + 7) / 8;
        if (estimated_bytes > decoder->data_cache_capacity) {
            estimated_bytes = decoder->data_cache_capacity;
        }
        
        // 读取数据到缓存
        if (rlv_read_frame_data(decoder, byte_offset, decoder->data_cache, estimated_bytes) != 0) {
            decoder->error_msg = "Failed to read frame data";
            return -1;
        }
        
        rlv_bit_reader_init(&decoder->bit_reader, decoder->data_cache, estimated_bytes);
        rlv_bit_reader_seek(&decoder->bit_reader, bit_offset % 8);
        
        uint32_t pixels_decoded = 0;
        uint32_t frame_size = decoder->header.width * decoder->header.height;
        uint16_t current_y = 0;
        uint16_t current_x = 0;
        
        while (pixels_decoded < frame_size && current_y < decoder->header.height) {
            uint32_t unit_value = rlv_read_bits(&decoder->bit_reader, total_unit_bits);
            uint8_t color = (unit_value >> decoder->header.unit_bits) & 1;
            uint32_t length = (unit_value & ((1 << decoder->header.unit_bits) - 1)) + 1;
            
            uint16_t pixel_color = color ? fg_color : bg_color;
            
            for (uint32_t i = 0; i < length && pixels_decoded < frame_size; i++) {
                line_buffer[current_x] = pixel_color;
                current_x++;
                pixels_decoded++;
                
                if (current_x >= decoder->header.width) {
                    line_callback(decoder, current_y, line_buffer, decoder->header.width);
                    current_x = 0;
                    current_y++;
                }
            }
        }
        
        if (current_x > 0 && current_y < decoder->header.height) {
            line_callback(decoder, current_y, line_buffer, current_x);
        }
    }
    
    return 0;
}

rlv_decoder_t *rlv_decoder_new(void) {
    return rlv_decoder_new_with_cache_size(RLV_DEFAULT_DATA_CACHE_SIZE);
}

rlv_decoder_t *rlv_decoder_new_with_cache_size(uint32_t data_cache_size) {
    rlv_decoder_t *decoder = m_malloc(sizeof(rlv_decoder_t));
    if (!decoder) {
        return NULL;
    }
    
    memset(decoder, 0, sizeof(rlv_decoder_t));
    decoder->buffer_size = MAX_LINE_BUFFER_SIZE;
    decoder->line_buffer = m_malloc(decoder->buffer_size);
    if (!decoder->line_buffer) {
        m_free(decoder);
        return NULL;
    }
    
    // 分配数据缓存
    decoder->data_cache_capacity = data_cache_size;
    decoder->data_cache = m_malloc(decoder->data_cache_capacity);
    if (!decoder->data_cache) {
        m_free(decoder->line_buffer);
        m_free(decoder);
        return NULL;
    }
    decoder->data_cache_size = 0;
    decoder->data_cache_offset = UINT32_MAX; // 标记为无效
    
    // 初始化FPS统计
    decoder->fps_print_enabled = false;
    decoder->fps_last_time = 0;
    decoder->fps_frame_count = 0;
    
    rlv_frame_cache_init(&decoder->frame_cache);
    
    return decoder;
}

void rlv_decoder_destroy(rlv_decoder_t *decoder) {
    if (!decoder) return;
    
    if (decoder->line_buffer) {
        m_free(decoder->line_buffer);
    }
    if (decoder->frame_table_cache) {
        m_free(decoder->frame_table_cache);
    }
    if (decoder->data_cache) {
        m_free(decoder->data_cache);
    }
    m_free(decoder);
}

int rlv_decoder_init(rlv_decoder_t *decoder, const uint8_t *data, uint32_t size) {
    if (!decoder || !data || size < RLV_HEADER_SIZE) {
        if (decoder) decoder->error_msg = "Invalid parameters";
        return -1;
    }
    
    decoder->file_data = (uint8_t *)data;
    decoder->file_size = size;
    decoder->file_handle = NULL;
    decoder->is_file_mode = false;
    decoder->current_frame = 0;
    decoder->error_msg = NULL;
    
    rlv_frame_cache_init(&decoder->frame_cache);
    
    if (parse_header(decoder, data) != 0) {
        return -1;
    }
    
    return 0;
}

// 新的文件模式初始化函数
int rlv_decoder_init_file(rlv_decoder_t *decoder, void *file_handle) {
    if (!decoder || !file_handle) {
        if (decoder) decoder->error_msg = "Invalid parameters";
        return -1;
    }
    
    decoder->file_handle = file_handle;
    decoder->file_data = NULL;
    decoder->file_size = 0;
    decoder->is_file_mode = true;
    decoder->current_frame = 0;
    decoder->error_msg = NULL;
    decoder->data_cache_offset = UINT32_MAX;
    
    rlv_frame_cache_init(&decoder->frame_cache);
    
    // 读取头部
    uint8_t header[RLV_HEADER_SIZE];
    if (rlv_read_data(decoder, 0, header, RLV_HEADER_SIZE) != 0) {
        decoder->error_msg = "Failed to read header";
        return -1;
    }
    
    if (parse_header(decoder, header) != 0) {
        return -1;
    }
    
    // 计算并分配帧表缓存
    decoder->frame_table_size = (decoder->header.frame_count * decoder->header.frame_table_bits + 7) / 8;
    if (decoder->frame_table_size > 0) {
        decoder->frame_table_cache = m_malloc(decoder->frame_table_size);
        if (!decoder->frame_table_cache) {
            decoder->error_msg = "Out of memory for frame table";
            return -1;
        }
        
        // 读取帧表
        if (rlv_read_data(decoder, RLV_HEADER_SIZE, decoder->frame_table_cache, decoder->frame_table_size) != 0) {
            decoder->error_msg = "Failed to read frame table";
            return -1;
        }
    }
    
    return 0;
}

// 统一的数据读取函数
int rlv_read_data(rlv_decoder_t *decoder, uint32_t offset, uint8_t *buffer, uint32_t size) {
    if (!decoder || !buffer || size == 0) {
        return -1;
    }
    
    if (decoder->is_file_mode) {
        // 文件模式：使用MicroPython文件API
        mp_file_t *fp = (mp_file_t *)decoder->file_handle;
        mp_seek(fp, offset, SEEK_SET);
        return (mp_readinto(fp, buffer, size) == size) ? 0 : -1;
    } else {
        // 内存模式：直接复制
        if (offset + size > decoder->file_size) {
            return -1;
        }
        memcpy(buffer, decoder->file_data + offset, size);
        return 0;
    }
}

void rlv_decoder_set_user_data(rlv_decoder_t *decoder, void *user_data) {
    if (decoder) {
        decoder->user_data = user_data;
    }
}

void *rlv_decoder_get_user_data(rlv_decoder_t *decoder) {
    return decoder ? decoder->user_data : NULL;
}

const char *rlv_decoder_get_error(rlv_decoder_t *decoder) {
    return decoder ? decoder->error_msg : "Invalid decoder";
}

// 运行时配置函数
void rlv_decoder_set_data_cache_size(rlv_decoder_t *decoder, uint32_t cache_size) {
    if (!decoder || cache_size == 0) {
        return;
    }
    
    // 如果新的缓存大小不同，重新分配内存
    if (cache_size != decoder->data_cache_capacity) {
        uint8_t *new_cache = m_malloc(cache_size);
        if (new_cache) {
            if (decoder->data_cache) {
                m_free(decoder->data_cache);
            }
            decoder->data_cache = new_cache;
            decoder->data_cache_capacity = cache_size;
            decoder->data_cache_size = 0;
            decoder->data_cache_offset = UINT32_MAX; // 标记为无效
        }
    }
}

uint32_t rlv_decoder_get_data_cache_size(rlv_decoder_t *decoder) {
    return decoder ? decoder->data_cache_capacity : 0;
}

void rlv_decoder_set_fps_print(rlv_decoder_t *decoder, bool enabled) {
    if (decoder) {
        decoder->fps_print_enabled = enabled;
        if (enabled) {
            decoder->fps_last_time = get_ticks_ms();
            decoder->fps_frame_count = 0;
        }
    }
}

bool rlv_decoder_get_fps_print(rlv_decoder_t *decoder) {
    return decoder ? decoder->fps_print_enabled : false;
}

// FPS统计函数
static void rlv_update_fps_stats(rlv_decoder_t *decoder) {
    if (!decoder || !decoder->fps_print_enabled) {
        return;
    }
    
    decoder->fps_frame_count++;
    uint32_t current_time = get_ticks_ms();
    uint32_t elapsed = current_time - decoder->fps_last_time;
    
    // 每秒打印一次FPS
    if (elapsed >= 1000) {
        float fps = (float)decoder->fps_frame_count * 1000.0f / elapsed;
        printf("RLV FPS: %.2f (frames: %lu, time: %lums)\n",
               fps, (unsigned long)decoder->fps_frame_count, (unsigned long)elapsed);
        
        decoder->fps_last_time = current_time;
        decoder->fps_frame_count = 0;
    }
}

uint32_t get_ticks_ms(void) {
    return mp_hal_ticks_ms();
}

void delay_ms(uint32_t ms) {
    mp_hal_delay_ms(ms);
}