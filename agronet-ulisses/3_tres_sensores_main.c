#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/queue.h"
//======================================
//Para trabalhar com sistema de arquivos
#include "esp_spiffs.h"
#include "esp_err.h"
#include <string.h>
//======================================

#define LED_PIN 2
// #define BOTAO_PIN 0

#define TEMP_PIN_1 34
#define TEMP_PIN_2 35
#define TEMP_PIN_3 32

#define BUFFER_SIZE 200
#define BUFFER_SIZE2 500
#define AMOSTRAGEM_US 50
#define AMOSTRAS 5000

esp_timer_handle_t timer1Handle;
QueueHandle_t signalQueue; //amostragem parte 1: fila para contagem de 1 e 0
QueueHandle_t sequenceQueue; //amostragem parte 2: fila para agrupamento de 1 e 0

//Variáveis
volatile int sampleIndex=0;
int c0 = 0, c1 = 0, signal, tempPin=TEMP_PIN_1, ctrTempPin=0;
float dc=0.0;
volatile bool start = true;

typedef struct {
    int value;    // 0 or 1
    int count;    // Count of 0s or 1s
} SignalData_t;

//================================================================================================
esp_err_t init_spiffs() {
    ESP_LOGI("SPIFFS", "Inicializando o sistema de arquivos SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    // Registra o sistema de arquivos no VFS
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE("SPIFFS", "Falha ao montar ou formatar o sistema de arquivos");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE("SPIFFS", "Partição SPIFFS não encontrada");
        } else {
            ESP_LOGE("SPIFFS", "Erro desconhecido ao inicializar SPIFFS: %s", esp_err_to_name(ret));
        }
    } else {
        size_t total = 0, used = 0;
        if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
            ESP_LOGI("SPIFFS", "Sistema de arquivos montado, total: %d, usado: %d", total, used);
        }
    }
    return ret;
}

void write_to_file(const char *filename, const char *data) {
    FILE* f = fopen(filename, "a");
    if (f == NULL) {
        ESP_LOGE("write_to_file", "Falha ao abrir o arquivo %s para escrita", filename);
        return;
    }
    
    fprintf(f, "%s\n", data); // Escreve a linha no arquivo e adiciona uma quebra de linha
    fclose(f);
    ESP_LOGI("write_to_file", "Dados gravados no arquivo: %s", data);
}

void read_from_file(const char *filename) {
    ESP_LOGI("read_from_file", "[%lld] Inicio",esp_timer_get_time());
    FILE* f = fopen(filename, "r");
    if (f == NULL) {
        ESP_LOGE("read_from_file", "Falha ao abrir o arquivo %s para leitura", filename);
        return;
    }

    int i = 0;
    char line[128]; // Ajuste o tamanho conforme necessário para seus dados
    while (fgets(line, sizeof(line), f) != NULL) {
        // Exibe cada linha lida
        ESP_LOGI("read_from_file", "Linha [%i] lida: %s", i, line);
        i++; // Incrementa o contador para a próxima linha
    }

    fclose(f);
    ESP_LOGI("read_from_file", "[%lld] Fim",esp_timer_get_time());
}

void get_file_info(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        // O arquivo não existe, então cria um novo arquivo
        f = fopen(filename, "w");
        if (f == NULL) {
            ESP_LOGE("get_file_info", "Não foi possível criar o arquivo %s", filename);
            return;
        }
        ESP_LOGI("get_file_info", "Arquivo %s criado", filename);
    } else {
        // O arquivo existe, lê as informações do arquivo
        int line_count = 0;
        char buffer[128];  // Buffer para ler cada linha

        while (fgets(buffer, sizeof(buffer), f) != NULL) {
            line_count++;
        }
        ESP_LOGI("get_file_info", "O arquivo %s contém %d linhas", filename, line_count);
    }
    fclose(f);
}

void delete_file(const char *filename) {
    if (remove(filename) == 0) {
        ESP_LOGI("delete_file", "Arquivo '%s' excluído com sucesso.", filename);
    } else {
        ESP_LOGE("delete_file", "Falha ao excluir o arquivo '%s'.", filename);
    }
}


//===============================================================================================

void timerTrigger1(void *arg) {
    if (sampleIndex<AMOSTRAS) {
        // signal=gpio_get_level(TEMP_PIN_1);
        signal=gpio_get_level(tempPin);
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        if (xQueueSendFromISR(signalQueue, (void *)&signal, &xHigherPriorityTaskWoken) != pdPASS) {
            ESP_LOGW("timerTrigger1", "Falha ao adicionar à fila! Overflow?");
        }
        sampleIndex++;
    } else {
        // ESP_LOGI("timerTrigger1", "Terminei de amostrar o sinal! - c0=%i - c1=%i",c0,c1);
        sampleIndex=0;
        dc=((float)c1/(c1+c0))*100;
        c0=0;
        c1=0;
        start = true;
        gpio_set_level(LED_PIN, 0);
        esp_timer_stop(timer1Handle);
    }
}

//Processamento do sinal
void signalProcessingTask(void *pvParameters) {
    int receivedSignal;
    int currentSignal = -1; //Inicia com valor inválido
    int currentCount = 0;

    while (1) {
        if (xQueueReceive(signalQueue, &receivedSignal, portMAX_DELAY)) {
            if (currentSignal == -1) { //Primeira interação
                currentSignal = receivedSignal;
            }
            //Verifica se o sinal mudou
            if (currentSignal != receivedSignal) {
                SignalData_t data = {currentSignal, currentCount};
                xQueueSend(sequenceQueue, &data, portMAX_DELAY); //Envia estrutura de dados para a segunda fila
                // Reset
                currentSignal = receivedSignal;
                currentCount = 0;
            }
            //Incrementa contadores
            currentCount++;
            if(receivedSignal == 0) {
                c0++;
            } else {
                c1++;
            }
        }
    }
}

void printQueueContents() {
    SignalData_t data;
    ESP_LOGI("Main", "Início da fila:");
    while (xQueueReceive(sequenceQueue, &data, pdMS_TO_TICKS(20))) { // 0 timeout, para não bloquear
        // ESP_LOGI("Main", "[Value: %d, Count: %d]", data.value, data.count);
    }
    ESP_LOGI("Main", "Fim da fila");
}

void clearQueue(QueueHandle_t queue) {
    void *tempData;
    while (xQueueReceive(queue, &tempData, 0) == pdTRUE) {
        // Faz nada, apenas retira o item da fila
    }
}

void calcFrequency(){
    SignalData_t currentData, nextData;
    uint64_t totalPeriodUs = 0;  // total de microssegundos
    int numPeriods = 0;          // número de períodos completos
    float temp=0.0; //Temperatura
    char log_buffer[256]; //gravar em arquivo

    // Se não houver dados na fila, retorne
    if(uxQueueMessagesWaiting(sequenceQueue) < 3) {  // Precisa de pelo menos 3 itens para ter "itens do meio"
        // ESP_LOGE("calcFrequency", "Fila com menos de 3 itens.");
        sprintf(log_buffer, "[%lld] Fila com menos de 3 itens.",esp_timer_get_time());
        ESP_LOGE("calcFrequency", "%s",log_buffer);
        write_to_file("/spiffs/temp.txt", log_buffer);
        return;
    }

    // Ignore o primeiro elemento
    xQueueReceive(sequenceQueue, &currentData, portMAX_DELAY);

    while(uxQueueMessagesWaiting(sequenceQueue) > 1) {
        xQueueReceive(sequenceQueue, &nextData, portMAX_DELAY);
        // Calcular período usando a contagem de 'currentData' e 'nextData'
        // Isso assume que sua sequência é alternada entre 0s e 1s.
        uint64_t periodUs = (currentData.count + nextData.count) * AMOSTRAGEM_US;
        totalPeriodUs += periodUs;
        numPeriods++;
        // Prossiga para o próximo par
        currentData = nextData;
    }
    // Ignore o último item (já que ele foi parcialmente usado no último período calculado)
    xQueueReceive(sequenceQueue, &nextData, portMAX_DELAY);
    if(numPeriods > 0) {
        uint64_t avgPeriodUs = totalPeriodUs / numPeriods;
        float frequencyHz = 1000000.0 / avgPeriodUs;  // convertendo período em frequência
        int dados=0;
        if(frequencyHz < 115){//Dados 1 - RMSE=1.15
            temp =  frequencyHz*0.30-19.9; 
            dados=1;

        }else if(frequencyHz >= 115 && frequencyHz <= 180){//Dados 2 - RMSE=0.30
            temp = frequencyHz*0.10+0.55; 
            dados=2;
        }else if(frequencyHz >180 && frequencyHz <=260){//Dados 3 - RMSE=0.29
            temp = frequencyHz*0.09+4.231; 
            dados=3;
        }else if(frequencyHz >260 && frequencyHz <=440){//Dados 4 - RMSE=0.28
            temp = frequencyHz*0.07+10.28;  
            dados=4;
        }else if(frequencyHz >440 && frequencyHz <=655){//Dados 5 - RMSE=0.14
            temp = frequencyHz*0.04+20.21;  
            dados=5;
        }else{//Dados 6 - RMSE=0.34
            temp =  frequencyHz*0.03+26.33;
            dados=6;
        }

        // ESP_LOGI("calcFrequency", "Frequência calculada[%i]: %.2f Hz - DC: %.2fp.p - Dados: %i - Temp: %.2f", tempPin,frequencyHz,dc,dados,temp);
        sprintf(log_buffer, "[%lld] Frequência calculada[%i]: %.2f Hz - DC: %.2fp.p - Dados: %i - Temp: %.2f",esp_timer_get_time(),tempPin, frequencyHz, dc, dados, temp);
        ESP_LOGI("calcFrequency", "%s",log_buffer);
        write_to_file("/spiffs/temp.txt", log_buffer);
    } else {
        // ESP_LOGE("calcFrequency", "Não foi possível calcular a frequência.");
        sprintf(log_buffer, "[%lld] Não foi possível calcular a frequência[%i].",esp_timer_get_time(),tempPin);
        ESP_LOGE("calcFrequency", "%s", log_buffer);
        write_to_file("/spiffs/temp.txt", log_buffer);
    }
}

int getPinTemp(){
    if(ctrTempPin == 0){
        ctrTempPin++;
        return TEMP_PIN_1;
    } else if(ctrTempPin == 1){
        ctrTempPin++;
        return TEMP_PIN_2;
    } else {
        ctrTempPin=0;
        return TEMP_PIN_3;
    }
}

// //INTERRUPÇÃO PARA DELETAR E REINICIAR ESP
// static volatile bool deleteAndRestartRequested = false;

// static void IRAM_ATTR button_isr_handler(void* arg) {
//     deleteAndRestartRequested = true;
// }

void app_main(void) {
    // Configuração do led interno
    esp_rom_gpio_pad_select_gpio(LED_PIN); 
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    // Configuração do pino que recebe o sinal do sensor
    esp_rom_gpio_pad_select_gpio(TEMP_PIN_1); 
    gpio_set_direction(TEMP_PIN_1, GPIO_MODE_INPUT);

    esp_rom_gpio_pad_select_gpio(TEMP_PIN_2); 
    gpio_set_direction(TEMP_PIN_2, GPIO_MODE_INPUT);

    esp_rom_gpio_pad_select_gpio(TEMP_PIN_3); 
    gpio_set_direction(TEMP_PIN_3, GPIO_MODE_INPUT);

    // Configuração do timer
    const esp_timer_create_args_t timer1Conf = {
        .callback = timerTrigger1,
        .name = "Timer1",
    };

    esp_timer_create(&timer1Conf, &timer1Handle);

    // Cria a fila amostragem 1
    signalQueue = xQueueCreate(BUFFER_SIZE, sizeof(int));
    if (signalQueue == NULL) {
        ESP_LOGE("app_main", "Falha ao criar fila.");
        return;
    }

    // Cria a tarefa de processamento de sinal
    xTaskCreate(signalProcessingTask, "signalProcessingTask", 2048, NULL, 5, NULL);
    
    // Cria a fila amostragem 2
    sequenceQueue = xQueueCreate(BUFFER_SIZE2, sizeof(SignalData_t));
    if (sequenceQueue == NULL) {
        ESP_LOGE("app_main", "Falha ao criar fila.");
        return;
    }

    // Inicializa o sistema de arquivos
    if (init_spiffs() != ESP_OK) {
        ESP_LOGE("app_main", "Erro ao inicializar o sistema de arquivos SPIFFS");
        return; // Não continue se falhar
    } 

    //Verifica arquivo, caso não tenha, crie
    get_file_info("/spiffs/temp.txt"); 
    // delete_file("/spiffs/temp.txt"); 

    while(1){//loop que irá coletar a temperatura de hora em hora.
        vTaskDelay(10000/ portTICK_PERIOD_MS); // Espera por 10
        int loop=0;
        while (loop<=8) {//Coleta a temperatura dos 3 sensores 3 vezes, por isso o loop roda 9 vezes.
            if (start) {//Iniciar o timer somente se start for true
                calcFrequency();
                clearQueue(sequenceQueue);
                start = false;
                tempPin = getPinTemp();//Modificação para coletar informação de 3 sensores
                gpio_set_level(LED_PIN, 1);
                esp_timer_start_periodic(timer1Handle, AMOSTRAGEM_US);
            }
            vTaskDelay(10000 / portTICK_PERIOD_MS); // Espera por 10 segundos
            loop++;
        }

        ESP_LOGI("main", "Aguardando para próxima amostra...");
        write_to_file("/spiffs/temp.txt", "Aguardando para próxima amostra...");
        // get_file_info("/spiffs/temp.txt"); 
        vTaskDelay(3600000 / portTICK_PERIOD_MS); // Espera por 1h
    }


    // Para pegar arquivo
    // while(1){
    //     read_from_file("/spiffs/temp.txt");
    //     vTaskDelay(600000 / portTICK_PERIOD_MS);
    // }
}
