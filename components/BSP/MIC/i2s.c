#include "driver/i2s.h"
#include "i2s.h"
// INMP441 麦克风引脚定义
#define INMP441_WS_IO   1   // WS (LRCK)
#define INMP441_SCK_IO  2   // SCK (BCLK)
#define INMP441_SD_IO   42   // SD (DIN)

#define bufferLen 512

//事件处理函数
void Init_i2s(){
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // L/R 接 GND，使用左声道
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = bufferLen,
        .use_apll = false          // 分配中断标志
    };
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_pin_config_t pin_config = {
        .bck_io_num = INMP441_SCK_IO,               // BCLK引脚号
        .ws_io_num = INMP441_WS_IO,                 // LRCK引脚号
        .data_out_num = -1,                         // DATA引脚号 (仅录音，不输出)
        .data_in_num = INMP441_SD_IO,               // DATA_IN引脚号
    };
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_start(I2S_NUM_0);
    
}

