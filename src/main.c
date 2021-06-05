#include <esp_wifi.h>               //Conexão Wireless
#include "esp_log.h"                //log de eventos no monitor serial
#include <esp_http_server.h>        //biblioteca para poder usar o server http
#include "freertos/event_groups.h"  //grupo de eventos
#include "nvs_flash.h"              //memória nvs
#include "driver/ledc.h"            //PWM
#include <math.h>




/*------------------------Definições de Projeto-----------------------*/
/* O event Group permite muitos eventos por grupo mas nos só nos importamos com 2 eventos:
 * - Conseguimos conexão com o AP com um IP
 * - Falhamos na conexão apos o número máximo de tentativas*/
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1


/*Aqui definimos o SSID, a Senha e o número máximo de tentativas que vamos 
tentar ao se conectar à rede Wireless*/
#define ESP_WIFI_SSID      "tira_o_zoio"
#define ESP_WIFI_PASS      "jabuticaba"
#define ESP_MAXIMUM_RETRY  10


//Definições do PWM 0:
#define PWM0_Gpio        18                     //PWM0->Timer0->Canal0->Gpio18

//Definições do PWM 1:
#define PWM1_Gpio        19                     //PWM1->Timer1->Canal1->Gpio19




/*-----------------------------------------------Constantes de Projeto --------------------------------------*/

static const char *TAG = "ESP";             //A tag que será impressa no log do sistema 



/*-----------------------------------------------------Variáveis GLobais-------------------------------------*/

static uint32_t numero_tentativa_de_conexao_wifi = 0;   //numero atual da tentativa de se conectar a rede,
                                                        //tentativas máximas= EXAMPLE_ESP_MAXIMUM_RETRY


typedef struct parametros_PWM
{
    bool                     estado; //ligado =1 , desligado=0
    uint32_t             frequencia; //frequencia do PWM   
    uint8_t   percentual_duty_cicle; //ciclo alto do PWM de 0 a 100%
    int                       timer; //qual timer é associado a esse pwm
    int                       canal; //qual canal é associado a esse pwm
    int           resolucao_inicial;
    int                  speed_mode;
    int                        clock;
    int                        gpio;
}parametros_PWM;


/*----------------------------------------------------Objetos------------------------------------------------*/

//Handle dos Eventos Wireless
static EventGroupHandle_t s_wifi_event_group;

//Declaração do server http
//static httpd_handle_t server =NULL;


/*-----------------------------------------------Declaração das Funções--------------------------------------*/

static void inicializa_variaveis(struct parametros_PWM *pwm0, struct parametros_PWM *pwm1);

static void setup_PWM(struct parametros_PWM  pwm);     //Realiza as configurações iniciais do PWM.

static void setup_nvs();     //Inicia a memória nvs. Ela é necessária para se conectar à rede Wireless

void wifi_init_sta(); //Configura a rede wireless

/* Lida com os Eventos da rede Wireless(reconexão, IPs, etc), essa função é ativada durante a chamada de 
 * void wifi_init_sta() e permanece monitorando os eventos de rede e imprimindo no terminal
 * as tentativas de conexão e o endereço IP recebido que o ESP ganhou*/
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);

static void atualiza_PWM(struct parametros_PWM pwm); //Atualiza os valores de resolução do duty, frequencia e duty cicle

static int calc_resolucao_duty(long double pwm_freq,long double timer_clk_freq);




void app_main() {
    struct parametros_PWM  pwm0;
    struct parametros_PWM  pwm1;
    inicializa_variaveis(&pwm0,&pwm1);
    setup_PWM(pwm0);
    setup_PWM(pwm1);
    setup_nvs();
    wifi_init_sta();

    while(true)
    {
        pwm0.percentual_duty_cicle =0;
        pwm0.frequencia= 100000;
        atualiza_PWM(pwm0);
        vTaskDelay(2000 / portTICK_RATE_MS);
        pwm0.percentual_duty_cicle =25;
        pwm0.frequencia= 1000;
        atualiza_PWM(pwm0);
        vTaskDelay(2000 / portTICK_RATE_MS);
    }

    //durante a execução:
    //atualiza_PWM(pwm0);
}

/*-------------------------------------Implementação das Funções----------------------------------------------*/

static void inicializa_variaveis(struct parametros_PWM  *pwm0,struct parametros_PWM  *pwm1)
{
    pwm0->estado=0;                              //desligado
    pwm0->frequencia=1000;                       //1000 Hz
    pwm0->percentual_duty_cicle=50;              //50%
    pwm0->timer=0;                               //timer0
    pwm0->canal=0;                               //canal0
    pwm0->gpio=PWM0_Gpio;                        //gpio definido
    pwm0->resolucao_inicial=LEDC_TIMER_13_BIT;   //13 bits de resolução inicial;
    pwm0->speed_mode       =LEDC_HIGH_SPEED_MODE;//Modo High speed;
    pwm0->clock            =LEDC_USE_APB_CLK;    //A fonte de clock do timer. APB_CLK = 80MHz;

    pwm1->estado=0;                              //desligado
    pwm1->frequencia=1000;                       //1000 Hz
    pwm1->percentual_duty_cicle=50;              //50%
    pwm1->timer=1;                               //timer0
    pwm1->canal=1;                               //canal0
    pwm1->gpio=PWM1_Gpio;                        //gpio definido
    pwm1->resolucao_inicial=LEDC_TIMER_13_BIT;   //13 bits de resolução inicial;
    pwm1->speed_mode       =LEDC_HIGH_SPEED_MODE;//Modo High speed;
    pwm1->clock            =LEDC_USE_APB_CLK;    //A fonte de clock do timer. APB_CLK = 80MHz;
}




//Realiza as configurações iniciais do PWM baseado na documentação de Espressiv:
//https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/ledc.html
static void setup_PWM(struct parametros_PWM  pwm){

    ledc_timer_config_t pwm_config = { //Configurações do PWM
        .duty_resolution =   pwm.resolucao_inicial, // resolution of PWM duty
        .freq_hz =           pwm.frequencia,        // frequency of PWM signal
        .speed_mode =        pwm.speed_mode,        // timer mode
        .timer_num =         pwm.timer,             // timer index
        .clk_cfg =           pwm.clock,             // clock source
    };
    // Aplicar a configuração do PWM0
    ledc_timer_config(&pwm_config);

    //Configurações do canal do PWM
    ledc_channel_config_t pwm_ch_config = {
        .gpio_num   = pwm.gpio,            //Gpio de saída do sinal PWM
        .speed_mode = pwm.speed_mode,      //modo do PWM
        .channel    = pwm.canal,           //Canal do PWM
        .intr_type  = 0,                   //interrupções desabilitadas
        .timer_sel  = pwm.timer,           //qual timer está asociado a esse canal
        .duty       = 0,                   //duty cicle inicial
        .hpoint     = 0,         
    };

    // Aplicar a configuração do Canal do PWM0
    ledc_channel_config(&pwm_ch_config);
}

//retorna o log na base 2 do valor recebido
static int calc_resolucao_duty(long double pwm_freq,long double timer_clk_freq)


    //PWM duty Resolution(bits)= |log2 (   PWMFreq      )|
    //                           |      --------------   |
    //                           |      timer_clk_freq   |
{
    long double frac = pwm_freq/timer_clk_freq;
    long double retorno= (logl(frac)/logl(2));
    return abs((int)retorno);
}

//Atualiza o sinal PWM baseado no canal selecionado, na frequência pedida e na porcentagem do duty
static void atualiza_PWM(struct parametros_PWM pwm){


    int timer_clk_freq;                 // 80 MHz é a velocidade do clock APB. Na função 
                                        // "inicializa variaveis", se quiser usar outro clock, usando
                                        // um valor diferente de LEDC_USE_APB_CLK terá que adaptar
                                        // a variavel "timer_clk_freq" com a frequência do clock
                                        // selecionado que achar no datasheet
    if(pwm.clock==LEDC_USE_APB_CLK)
    {
        timer_clk_freq=80000000;
    }

    int resolucao_duty= calc_resolucao_duty(pwm.frequencia,timer_clk_freq);

    ESP_LOGI(TAG,"Atualizar PWM: Freq: %d, Duty: %d, Resolução: %d",
            pwm.frequencia,
            pwm.percentual_duty_cicle,
            resolucao_duty);

    //aplica a Resolução do Duty(bits) no timer correto  
    ledc_timer_set(pwm.speed_mode,pwm.timer,1,resolucao_duty,pwm.clock);

    //Atualiza Frequência no timer correto
    ledc_set_freq(pwm.speed_mode,pwm.timer,pwm.frequencia);

    //transforma o valor do duty de percentual para bits
    uint32_t duty = pow(2,resolucao_duty)*((double)pwm.percentual_duty_cicle/100);
  
    // Atualiza o duty cicle do canal
    ledc_set_duty(pwm.speed_mode,pwm.canal,duty);

    // Aplica as configurações
    ledc_update_duty(pwm.speed_mode,pwm.canal);
}



//Inicializa a memória nvs pois é necessária para o funcionamento do Wireless
static void setup_nvs(){
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}


//Lida com os Eventos da rede Wireless, conexão à rede e endereço IP
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect(); //se o Wifi ja foi iniciado tenta se conectar
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (numero_tentativa_de_conexao_wifi < ESP_MAXIMUM_RETRY) { //se o numero atual de tentativas
            esp_wifi_connect();                                             //de conexão não atingiu o máximo
            numero_tentativa_de_conexao_wifi++;                             //tenta denovo            
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);          //se o numero atingiu o máximo ativa
        }                                                                   //o envento de falha no wifi            
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) { //se estamos conectados a rede vamos
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;         //imprimir o IP no terminal via ESP_LOGI()
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        numero_tentativa_de_conexao_wifi = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(){
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}