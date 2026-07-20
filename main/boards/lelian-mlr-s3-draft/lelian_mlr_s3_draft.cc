#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>

#include <algorithm>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

#define TAG "LelianMlrS3Draft"

class LelianMlrS3DraftBoard : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Display* display_ = nullptr;

    static bool IsValidGpio(gpio_num_t pin) {
        return pin >= GPIO_NUM_0 && pin < GPIO_NUM_NC;
    }

    static void AddPin(cJSON* root, const char* name, gpio_num_t pin) {
        cJSON_AddNumberToObject(root, name, static_cast<int>(pin));
    }

    bool CanInitializeLcd() const {
#if LELIAN_LCD_PIN_MAP_READY
        return IsValidGpio(DISPLAY_MOSI_PIN) && IsValidGpio(DISPLAY_CLK_PIN) &&
               IsValidGpio(DISPLAY_DC_PIN) && IsValidGpio(DISPLAY_CS_PIN);
#else
        return false;
#endif
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        if (!CanInitializeLcd()) {
            ESP_LOGW(TAG, "LCD pin map is not ready, using NoDisplay");
            display_ = new NoDisplay();
            return;
        }

        InitializeSpi();

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;

#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            codec->SetOutputVolume(std::min(codec->output_volume() + 10, 100));
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            codec->SetOutputVolume(std::max(codec->output_volume() - 10, 0));
        });
    }

    void RegisterDraftTools() {
        static LampController lamp(LAMP_GPIO);

        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.board.lelian_pin_map.get_status",
            "Get the LeLian MLR S3 draft board pin map status. Use this to check which hardware routes still need confirmation.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "board", "lelian-mlr-s3-draft");
                cJSON_AddBoolToObject(root, "lcd_pin_map_ready", LELIAN_LCD_PIN_MAP_READY);
                cJSON_AddBoolToObject(root, "ml307_pin_map_ready", LELIAN_ML307_PIN_MAP_READY);
                cJSON_AddBoolToObject(root, "audio_pin_map_ready", LELIAN_AUDIO_PIN_MAP_READY);
                AddPin(root, "display_mosi_gpio", DISPLAY_MOSI_PIN);
                AddPin(root, "display_clk_gpio", DISPLAY_CLK_PIN);
                AddPin(root, "display_dc_gpio", DISPLAY_DC_PIN);
                AddPin(root, "display_cs_gpio", DISPLAY_CS_PIN);
                AddPin(root, "display_rst_gpio", DISPLAY_RST_PIN);
                AddPin(root, "ml307_tx_gpio", ML307_TX_PIN);
                AddPin(root, "ml307_rx_gpio", ML307_RX_PIN);
                AddPin(root, "mic_ws_gpio", AUDIO_I2S_MIC_GPIO_WS);
                AddPin(root, "mic_sck_gpio", AUDIO_I2S_MIC_GPIO_SCK);
                AddPin(root, "mic_din_gpio", AUDIO_I2S_MIC_GPIO_DIN);
                AddPin(root, "spk_dout_gpio", AUDIO_I2S_SPK_GPIO_DOUT);
                AddPin(root, "spk_bclk_gpio", AUDIO_I2S_SPK_GPIO_BCLK);
                AddPin(root, "spk_lrck_gpio", AUDIO_I2S_SPK_GPIO_LRCK);
                cJSON_AddStringToObject(root, "note", "LCD and ML307 GPIO numbers are not visible in the board photo.");
                return root;
            });
    }

public:
    LelianMlrS3DraftBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeLcdDisplay();
        InitializeButtons();
        RegisterDraftTools();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC && GetBacklight() != nullptr) {
            GetBacklight()->RestoreBrightness();
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_ != nullptr ? display_ : Board::GetDisplay();
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }
};

DECLARE_BOARD(LelianMlrS3DraftBoard);
