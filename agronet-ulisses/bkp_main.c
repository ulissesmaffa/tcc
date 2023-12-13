#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define LED_PIN 2
#define TEMP_PIN 34
#define BUFFER_SIZE 512
// #define AMOSTRAGEM_US 1000 //1ms
#define AMOSTRAGEM_US 100

//resolver esses caras aqui!!!
int buffer[BUFFER_SIZE];
volatile int bufferIndex = 0;
esp_timer_handle_t timer1Handle;

void printVector(int *v, int size){
    int i;
    
    for(i=0;i<size;i++){
        printf("%i\n",v[i]);
    }
    printf("\n");
}

void newBufferSize(int *buffer, int size, int *valueNewBufferSize, int *valueStartCutBuffer){
    bool ctr,changed;
    int qtd,i,startCut;

    //discartando inicio do sinal
    ctr=true;
    changed=false;
    i=0;

    while(ctr){
        if(buffer[i]!=buffer[i+1]){
            if(changed){
                ctr=false;
            }else{
                changed=true;
            }
        }
        i++;
    }
    startCut=i;
    qtd=size-i;

    //discartando final do sinal
    ctr=true;
    changed=false;
    i=size-1;
    
    //se sinal começa com o mesmo valor que termina
    if(buffer[0]==buffer[size-1]){
        while(ctr){
            if(buffer[i]==buffer[i-1]){
                i--;
            }else{
                ctr=false;
            }
        }
    }else{//se o sinal começa com um valor diferente do que termina
        while(ctr){
            if(buffer[i]!=buffer[i-1]){
                if(changed){
                    ctr=false;
                }else{
                    changed=true;
                }
            }
            i--;
        }
    }
    qtd=qtd-(size-i);
    
    *valueNewBufferSize=qtd;
    *valueStartCutBuffer=startCut;
}


void insertNewBuffer(int *buffer, int *newBuffer, int size_b2, int startCut){
    int i;
    for(i=0;i<size_b2;i++){
        newBuffer[i]=buffer[startCut+i];
    }
}

float calculateFrequency(int *v, int size){
    bool changed;
    int i,group;
    float freq=0.0;

    changed=false;
    group=0;

    for(i=0;i<size;i++){
        if(v[i]!=v[i+1]){
            if(changed){
                changed=false;
                group++;
            }else{
                changed=true;
            }   
        }
        // freq=freq+(AMOSTRAGEM_US/1000);
        freq=freq+(AMOSTRAGEM_US);
    }
    // ESP_LOGI("calcFreq","Período:%f - group:%i - freq:%f",freq,group,(1/(freq/group))*1000000);
    freq=freq/group;
    freq=(1/freq)*1000000;

    return freq;
}

void frequencyThread(void *arg){
    // ESP_LOGI("FreqTh","Estou na frequencyThread");
        
    int *newBuffer,valueNewBufferSize,valueStartCutBuffer;
    float frequency;
    newBufferSize(buffer,BUFFER_SIZE,&valueNewBufferSize,&valueStartCutBuffer);
    newBuffer=(int *) malloc(valueNewBufferSize * sizeof(int));
    if(!newBuffer){
        ESP_LOGE("FreqTh","Erro Malloc newBuffer");
    }else{
        insertNewBuffer(buffer,newBuffer,valueNewBufferSize,valueStartCutBuffer);
        frequency=calculateFrequency(newBuffer,valueNewBufferSize);
        ESP_LOGI("FreqTh","Frequencia é: %.2fHz",frequency);
        // vTaskDelete(NULL);
        free(newBuffer); // Liberando a memória alocada para newBuffer
        bufferIndex=0;
        // ESP_LOGI("FreqTh","bufferIndex=%i",bufferIndex);
        vTaskDelete(NULL);
    }
}

void timerTrigger1(void *arg){   

    if(bufferIndex<BUFFER_SIZE){
        buffer[bufferIndex]=gpio_get_level(TEMP_PIN);
        gpio_set_level(LED_PIN, buffer[bufferIndex]);
        // ESP_LOGI("timerTrigger1","Amostra Sinal: [%i]=%i",bufferIndex,buffer[bufferIndex]);
        bufferIndex++;
    }else{
        // ESP_LOGI("timerTrigger1","Amostragem do sinal concluída");
        esp_timer_stop(timer1Handle);
        // esp_timer_delete(timer1Handle);
        xTaskCreatePinnedToCore(frequencyThread, "FrequencyThread", 2048, NULL, 1, NULL, 0);
    }
}

//função que zera todos os valores de um determinado vetor inteiro
void resetVector(int *v, int size){
    int i;
    for(i=0;i<size;i++){
        v[i]=0;
    }
}

void app_main(void){
    //zerar vetor buffer
    resetVector(buffer,BUFFER_SIZE);

    //Configuração do led interno
    esp_rom_gpio_pad_select_gpio(LED_PIN); 
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    //Configuração do pino que recebe o sinal do sensor
    esp_rom_gpio_pad_select_gpio(TEMP_PIN); 
    gpio_set_direction(TEMP_PIN, GPIO_MODE_INPUT);

    //configuração do timer
    const esp_timer_create_args_t timer1Conf = {
        .callback = timerTrigger1,
        .name = "Timer1",
    };

    esp_timer_create(&timer1Conf,&timer1Handle);//cria o timer conforme timer1Config e armazena id do timer em timer1Handle
    esp_timer_start_periodic(timer1Handle,AMOSTRAGEM_US);//inicia o timer para executar a func disparoTimer1 periodicamente a cada AMOSTRAGEM_US

    while(1){
        // Iniciar timer somente se o buffer estiver vazio
        if(bufferIndex == 0){
            // ESP_LOGI("Main","Fazer mais uma amostra");
            esp_timer_start_periodic(timer1Handle,AMOSTRAGEM_US);
        }
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }

 

}
