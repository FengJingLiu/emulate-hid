/* SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/usbh.h"
#include "sdkconfig.h"
#include "soc/gpio_num.h"
#include "tinyusb_hid.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"


#define TAG "HID Example"

static const int RX_BUF_SIZE = 1024;
#define TXD_PIN (GPIO_NUM_43)
#define RXD_PIN (GPIO_NUM_44)

ESP_EVENT_DECLARE_BASE(POINTS_PARSER_EVENTS);

ESP_EVENT_DEFINE_BASE(POINTS_PARSER_EVENTS);

enum
{
    EVENT_UART_DATA
};

void init_uart() {
  const uart_config_t uart_config = {
      .baud_rate = 115200,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_APB,
  };

  // 配置 UART 驱动
  uart_driver_install(UART_NUM_0, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
  uart_param_config(UART_NUM_0, &uart_config);
  uart_set_pin(UART_NUM_0, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE,
               UART_PIN_NO_CHANGE);
}
//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

static void tusb_device_task(void *arg) {
  while (1) {
    tud_task();
  }
}

// Invoked when device is mounted
void tud_mount_cb(void) { ESP_LOGI(TAG, "USB Mount"); }

// Invoked when device is unmounted
void tud_umount_cb(void) { ESP_LOGI(TAG, "USB Un-Mount"); }

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
  (void)remote_wakeup_en;
  ESP_LOGI(TAG, "USB Suspend");
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) { ESP_LOGI(TAG, "USB Resume"); }

//--------------------------------------------------------------------+
// USB PHY config
//--------------------------------------------------------------------+
static void usb_phy_init(void) {
  usb_phy_handle_t phy_hdl;
  // Configure USB PHY
  usb_phy_config_t phy_conf = {
      .controller = USB_PHY_CTRL_OTG,
      .otg_mode = USB_OTG_MODE_DEVICE,
      .target = USB_PHY_TARGET_INT,
  };
  usb_new_phy(&phy_conf, &phy_hdl);
}

typedef struct {
  int16_t point_cnt;
  int8_t *point_array;
  uint8_t click;
  uint8_t need_init; // 需要先移动到 0， 0
} mouse_move_t;

void parser_uart_mouse_move(uint8_t *data, size_t data_size,
                            mouse_move_t *position) {
  // 前两位保存 point 数量,第三位是否点击 第四位是否初始移动到 0,0
  uint16_t point_cnt = (uint16_t)data[0] | (uint16_t)(data[1] << 8);
  ESP_LOGI(TAG, "point_cnt 1 x:%d", point_cnt);

  position->point_array = (int8_t*)(data + 4);
  if ((uint8_t)data[2] == 1) {
    position->click = 1;
  } else {
    position->click = 0;
  }
  if ((uint8_t)data[3] == 1) {
    position->need_init = 1;
  } else {
    position->need_init = 0;
  }
  position->point_cnt = point_cnt;
  ESP_LOGI(TAG, "position point_cnt=%d click=%d init=%d", position->point_cnt,
           position->click, position->need_init);
}

void move_mouse_large_distance(int32_t x, int32_t y, int32_t vertical,
                               int32_t horizontal) {
  int32_t x_remaining = x;
  int32_t y_remaining = y;
  int32_t vertical_remaining = vertical;
  int32_t horizontal_remaining = horizontal;

  while (x_remaining != 0 || y_remaining != 0 || vertical_remaining != 0 ||
         horizontal_remaining != 0) {
    // 计算本次移动的值，限制在 -127 到 127 之间
    int8_t x_move = (x_remaining > 127)    ? 127
                    : (x_remaining < -127) ? -127
                                           : (int8_t)x_remaining;
    int8_t y_move = (y_remaining > 127)    ? 127
                    : (y_remaining < -127) ? -127
                                           : (int8_t)y_remaining;
    int8_t vertical_move = (vertical_remaining > 127) ? 127
                           : (vertical_remaining < -127)
                               ? -127
                               : (int8_t)vertical_remaining;
    int8_t horizontal_move = (horizontal_remaining > 127) ? 127
                             : (horizontal_remaining < -127)
                                 ? -127
                                 : (int8_t)horizontal_remaining;

    // 发送鼠标移动报告
    tinyusb_hid_mouse_move_report(x_move, y_move, vertical_move,
                                  horizontal_move);

    // 更新剩余的移动距离
    x_remaining -= x_move;
    y_remaining -= y_move;
    vertical_remaining -= vertical_move;
    horizontal_remaining -= horizontal_move;
  }
}

void move_mouse(mouse_move_t *position) {
  if (position->need_init == 1) {
    move_mouse_large_distance(-4000, -5000, 0, 0);
  }
  for (int i = 0; i < position->point_cnt * 2; i = i + 2) {
    tinyusb_hid_mouse_move_report(position->point_array[i],
                                  position->point_array[i + 1], 0, 0);
    // ESP_LOGI(TAG, "Move to x:%d y:%d", position->point_array[i],
    //          position->point_array[i + 1]);
  }
  // move_mouse_large_distance(position->x, position->y, 0, 0);
  if (position->click == 1) {
    ESP_LOGI(TAG, "Click");
    tinyusb_hid_mouse_button_report(MOUSE_BUTTON_LEFT);
    tinyusb_hid_mouse_button_report(0);
  }
}

typedef struct{
    char* data;
    int data_len;
} event_data_t;

static void rx_task(void *arg)
{
    static const char *RX_TASK_TAG = "RX_TASK";
    esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);
    uint8_t* data = (uint8_t*) malloc(RX_BUF_SIZE + 1);
    while (1) {
        const int rxBytes = uart_read_bytes(UART_NUM_0, data, RX_BUF_SIZE, 50 / portTICK_PERIOD_MS);
        if (rxBytes > 0) {
            data[rxBytes] = 0;
            ESP_LOGI(RX_TASK_TAG, "Read %d point:%p", rxBytes, data);
            char* data_cp = (char*) malloc(rxBytes);
            memcpy(data_cp, data, rxBytes);
            event_data_t event_data = {.data = data_cp, rxBytes};
            esp_event_post(POINTS_PARSER_EVENTS, EVENT_UART_DATA, &event_data, sizeof(event_data_t), portMAX_DELAY);
            // ESP_LOG_BUFFER_HEXDUMP(RX_TASK_TAG, data, rxBytes, ESP_LOG_INFO);
        }
    }
    free(data);
}



static void uart_data_event_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  static bool start_flag = 1;
  static int16_t tot_size = 0;
  static char *point_data;
  static int point_data_index = 0;
  
  event_data_t* data = (event_data_t*)event_data;
  ESP_LOGI("RX_TASK","uart_data_event_handler point : %p", data->data);
  ESP_LOGI("RX_TASK","uart_data_event_handler data :%d ", data->data[0]);
  ESP_LOGI("RX_TASK","uart_data_event_handler data_size :%d ", data->data_len);
  if (data != NULL) {
      if (start_flag) {
        uint16_t point_cnt = (uint16_t)(data->data[0]) | ((uint16_t)data->data[1] << 8);
        ESP_LOGI("RX_TASK","point_cnt is :%d data[0] : %d, data[1] : %d", point_cnt, data->data[0], data->data[1]);
        tot_size = 2 + 2 + (point_cnt * 2);
        point_data = (char *)malloc(tot_size);
        memcpy(point_data, data->data, data->data_len);
        start_flag = 0;
        point_data_index += data->data_len;
        ESP_LOGI("RX_TASK",
                 "start_flag is true, tot_size %d point_data_index %d",
                 tot_size, point_data_index);
      } else {
        memcpy(point_data + point_data_index, data->data, data->data_len);
        point_data_index += data->data_len;
        ESP_LOGI("RX_TASK","mid data point_data_index: %d", point_data_index);
      }
      if (point_data_index >= tot_size) {
        start_flag = 1;
        mouse_move_t move_position;
        parser_uart_mouse_move((uint8_t *)point_data, point_data_index, &move_position);
        ESP_LOGI(TAG, "Mouse point_cnt=%d click=%d init=%d",
                 move_position.point_cnt, move_position.click,
                 move_position.need_init);
        move_mouse(&move_position);
        point_data_index = 0;
        free(point_data);
      }
    }
    free(data->data);
}

void app_main(void) {
  esp_event_loop_create_default();
  init_uart();
  // switch esp usb phy to usb-otg
  usb_phy_init();
  tud_init(BOARD_TUD_RHPORT);
  xTaskCreate(tusb_device_task, "TinyUSB", 4096, NULL, 5, NULL);
  tinyusb_hid_init();
  xTaskCreate(rx_task, "uart_rx_task", 1024 * 4, NULL, configMAX_PRIORITIES - 1, NULL);

  ESP_LOGI(TAG, "Wait Mount through USB interface");
  ESP_ERROR_CHECK(esp_event_handler_register(POINTS_PARSER_EVENTS, ESP_EVENT_ANY_ID, uart_data_event_handler, NULL));
}
