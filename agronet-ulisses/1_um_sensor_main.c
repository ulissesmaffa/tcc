#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/queue.h"

#define LED_PIN 2
#define TEMP_PIN 34
#define BUFFER_SIZE 200
#define BUFFER_SIZE2 500
#define AMOSTRAGEM_US 50
#define AMOSTRAS 5000

esp_timer_handle_t timer1Handle;
QueueHandle_t signalQueue; //amostragem parte 1: fila para contagem de 1 e 0
QueueHandle_t sequenceQueue; //amostragem parte 2: fila para agrupamento de 1 e 0

//Variáveis
volatile int sampleIndex=0;
int c0 = 0, c1 = 0, signal;
float dc=0.0;
volatile bool start = true;

typedef struct {
    int value;    // 0 or 1
    int count;    // Count of 0s or 1s
} SignalData_t;

void timerTrigger1(void *arg) {   
    if (sampleIndex<AMOSTRAS) {
        signal=gpio_get_level(TEMP_PIN);
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

    // Se não houver dados na fila, retorne
    if(uxQueueMessagesWaiting(sequenceQueue) < 3) {  // Precisa de pelo menos 3 itens para ter "itens do meio"
        ESP_LOGE("calcFrequency", "Fila com menos de 3 itens.");
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

        ESP_LOGI("calcFrequency", "Frequência calculada: %.2f Hz - DC: %.2fp.p - Dados: %i - Temp: %.2f", frequencyHz,dc,dados,temp);
    } else {
        ESP_LOGE("calcFrequency", "Não foi possível calcular a frequência.");
    }
}


void app_main(void) {
    // Configuração do led interno
    esp_rom_gpio_pad_select_gpio(LED_PIN); 
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    // Configuração do pino que recebe o sinal do sensor
    esp_rom_gpio_pad_select_gpio(TEMP_PIN); 
    gpio_set_direction(TEMP_PIN, GPIO_MODE_INPUT);

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

    while (1) {
        if (start) {//Iniciar o timer somente se start for true
            // printQueueContents();
            // printQueueSize();
            calcFrequency();
            clearQueue(sequenceQueue);

            start = false;
            gpio_set_level(LED_PIN, 1);
            // ESP_LOGI("Main", "Vou iniciar uma amostra de temperatura!");
            esp_timer_start_periodic(timer1Handle, AMOSTRAGEM_US);
        }
        vTaskDelay(4000 / portTICK_PERIOD_MS); 
    }
}
