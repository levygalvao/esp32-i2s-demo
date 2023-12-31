#include <stdio.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"

#include "protocol_examples_common.h"

#define SERVER_IP_ADDR CONFIG_SERVER_IP_ADDR // IP do servidor UDP
#define SERVER_PORT CONFIG_SERVER_PORT // porta do servidor UDP

#define DMA_BUF_NUM 16 // quantidade de buffers do DMA
#define DMA_BUF_SIZE 1024 // tamanho [em amostras] dos buffers do DMA
#define BUF_SIZE 4096 // tamanho [em bytes] dos buffers de entrada/transmissao

char in_buffer[BUF_SIZE]; // buffer para entrada de dados
char tx_buffer[BUF_SIZE]; // buffer para envio de dados

i2s_chan_handle_t rx_handle; // handler para canais i2s

QueueHandle_t xQueueData; // fila de dados e eventos para transferir leituras do microfone entre tarefas
TaskHandle_t xTaskReadDataHandle; // handler para task de leitura de dados do microfone
TaskHandle_t xTaskTransmitDataHandle; // handler para task de envio de dados

// tags
static const char *I2S_TAG = "I2S";
static const char *UDP_TAG = "UDP";
static const char *MAIN_TAG = "MAIN";

static void vTaskReadData(void * pvParameters)
{
    size_t bytes_to_read = BUF_SIZE; // quantidades de bytes para ler
    size_t bytes_read; // quantidade de bytes lidos

    // iniciar o canal i2s
    i2s_channel_enable(rx_handle);

    while (1) {
        // esperar para que o buffer de entrada (in_buffer) seja totalmente preenchido
        if (i2s_channel_read(rx_handle, (void*) in_buffer, bytes_to_read, &bytes_read, portMAX_DELAY) == ESP_OK) {
            xQueueSend(xQueueData, &in_buffer, portMAX_DELAY); // enfileirar dados lidos para a tarefa de envio
        } else {
            ESP_LOGE(I2S_TAG, "Erro durante a leitura: errno %d", errno);
            break;
        }
        vTaskDelay(1);
    }

    // no caso de erro, deve-se parar o canal i2s
    i2s_channel_disable(rx_handle);
    // liberar os recursos alocados
    i2s_del_channel(rx_handle);

    vTaskDelete(NULL);
}

static void vTaskTransmitData(void * pvParameters)
{
    // configurando endereco do servidor
    struct sockaddr_in dest_addr = {
        .sin_addr.s_addr = inet_addr(SERVER_IP_ADDR),
        .sin_family = AF_INET,
        .sin_port = htons(SERVER_PORT),
    };

    while (1) {

        // criar socket UDP
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(UDP_TAG, "Falha ao criar socket: errno %d", errno);
            break;
        }

        ESP_LOGI(UDP_TAG, "Socket criado. Destino dos pacotes %s:%d", SERVER_IP_ADDR, SERVER_PORT);
        
        while (1) {
            if(
                (xQueueData!=NULL)                                 &&
                (xQueueReceive(xQueueData, &tx_buffer, 0)==pdTRUE) 
            ) // esperar que dados sejam lidos
            {
                // enviar buffer
                int err = sendto(
                    sock, 
                    tx_buffer, 
                    BUF_SIZE, 
                    0, 
                    (struct sockaddr *)&dest_addr, 
                    sizeof(dest_addr)
                );

                // avaliar se envio falhou
                if (err < 0) {
                    ESP_LOGE(UDP_TAG, "Erro durante o envio: errno %d", errno);
                    break;
                }
            }
            vTaskDelay(1);
        }

        if (sock != -1) {
            ESP_LOGE(UDP_TAG, "Desativando socket e reiniciando...");
            shutdown(sock, 0);
            close(sock);
        }
    }    
    vTaskDelete(NULL);
}

void i2s_std()
{
    // configuracao do canal i2s 
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_AUTO, // escolha do canal automatica (0 ou 1)
        .role = I2S_ROLE_MASTER, // o esp32 atua como o controlador
        .dma_desc_num = DMA_BUF_NUM, // quantidade de buffers do DMA
        .dma_frame_num = DMA_BUF_SIZE, // tamanho dos buffers do DMA
        .auto_clear = false, // limpar automaticamente o buffer TX (desnecessario)
    };
    
    // alocar o canal i2s para receber dados
    i2s_new_channel(&chan_cfg, NULL, &rx_handle);

    // configuracoes de clock, slot e gpio para o i2s no modo normal (standard)
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050), // 22050 amostras por segundo
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO), // 32bits por amostras e audio mono
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // master clock (opcional)
            .bclk = GPIO_NUM_5, // bit clock
            .ws = GPIO_NUM_18, // word slot
            .dout = I2S_GPIO_UNUSED, // data out (necessario apenas para envio de dados)
            .din = GPIO_NUM_19, // data in
            .invert_flags = { // nao inverter bits
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    // inicializar o canal i2s no modo padrao
    i2s_channel_init_std_mode(rx_handle, &std_cfg);
}

void i2s_pdm()
{
    // configuracao do canal i2s 
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0, // escolha do canal i2s 0 (no esp32 apenas esse canal suporta o modo pdm)
        .role = I2S_ROLE_MASTER, // o esp32 atua como o controlador
        .dma_desc_num = DMA_BUF_NUM, // quantidade de buffers do DMA
        .dma_frame_num = DMA_BUF_SIZE, // tamanho dos buffers do DMA
        .auto_clear = false, // limpar automaticamente o buffer TX (desnecessario)
    };

    // alocar o canal i2s para receber dados
    i2s_new_channel(&chan_cfg, NULL, &rx_handle);

    // configuracoes de clock, slot e gpio para o i2s no modo pdm
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(44100), // 44100 amostras por segundo
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), // 16bits por amostras e audio mono
        .gpio_cfg = {
            .clk = GPIO_NUM_18, // clock
            .din = GPIO_NUM_19, // data in
            .invert_flags = { // nao inverter bits
                .clk_inv = false,
            },
        },
    };

    // inicializar o canal i2s no modo pdm
    i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg);
}

void app_main(void)
{
    // decidir qual o tipo de aquisicao sera feita
    #ifdef CONFIG_USE_I2S_ACQUISITION
        i2s_std(); // aquisicao de audio em formato i2s
    #elif CONFIG_USE_PDM_ACQUISITION 
        i2s_pdm(); // aquisicao de audio em formato pdm
    #endif

    ESP_ERROR_CHECK(nvs_flash_init()); // inicializar NVS
    ESP_ERROR_CHECK(esp_netif_init()); // inicializar lwIP
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // criar event loop para eventos Wi-Fi
    ESP_ERROR_CHECK(example_connect()); // funcao do protocol_examples_common.h para auxiliar na conexao Wi-Fi

    xQueueData = xQueueCreate(DMA_BUF_NUM, BUF_SIZE*sizeof(char)); 
    if(xQueueData == NULL){ // testar se a criacao da fila falhou
        ESP_LOGE(MAIN_TAG, "Falha em criar fila de dados");
        while(1);
    }

    BaseType_t xReturnedTask[2];

    xReturnedTask[0] = xTaskCreatePinnedToCore(
        vTaskReadData, 
        "taskREAD", 
        configMINIMAL_STACK_SIZE+2048, 
        NULL, 
        configMAX_PRIORITIES-1, 
        &xTaskReadDataHandle, 
        APP_CPU_NUM
    );

    xReturnedTask[1] = xTaskCreatePinnedToCore(
        vTaskTransmitData, 
        "taskTX", 
        configMINIMAL_STACK_SIZE+2048, 
        NULL, 
        configMAX_PRIORITIES-1, 
        &xTaskTransmitDataHandle, 
        PRO_CPU_NUM
    );

    // testar se a criacao das tarefas falhou
    if(xReturnedTask[0] == pdFAIL || xReturnedTask[1] == pdFAIL){ 
        ESP_LOGE(MAIN_TAG, "Falha em criar tarefas");
        while(1);
    }
}