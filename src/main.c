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


//Definições de ambos PWMs:
#define PWM_res_inicial LEDC_TIMER_13_BIT      //13 bits de resolução inicial
#define PWM_Modo        LEDC_HIGH_SPEED_MODE   //Modo High speed
#define PWM_Clock       LEDC_USE_APB_CLK       //A fonte de clock do timer. APB_CLK = 80MHz

//Definições do PWM 0:
#define PWM0_Timer       LEDC_TIMER_0           //timer 0 para o PWM0
#define PWM0_Canal       LEDC_CHANNEL_0         //PWM0->Timer0->Canal0
#define PWM0_Gpio        18                     //PWM0->Timer0->Canal0->Gpio18

//Definições do PWM 1:
#define PWM1_Timer       LEDC_TIMER_1           //timer 1 para o PWM0
#define PWM1_Canal       LEDC_CHANNEL_1         //PWM1->Timer1->Canal1
#define PWM1_Gpio        19                     //PWM1->Timer1->Canal1->Gpio19




/*-----------------------------------------------Constantes de Projeto --------------------------------------*/

static const char *TAG = "ESP";             //A tag que será impressa no log do sistema 

const int Timers_Source_Speed = 80000000;   //80 MHZ



/*-----------------------------------------------------Variáveis GLobais-------------------------------------*/

static uint32_t numero_tentativa_de_conexao_wifi = 0;   //numero atual da tentativa de se conectar a rede,
                                                        //tentativas máximas= EXAMPLE_ESP_MAXIMUM_RETRY

static uint32_t PWM0_freq = 5000;       //frequencia em Hertz
static uint32_t PWM1_freq = 5000;       //frequencia em Hertz

//static uint32_t PWM0_duty = 0;          //porcentagem do ciclo positivo do duty
//static uint32_t PWM1_duty = 0;          //porcentagem do ciclo positivo do duty


/*----------------------------------------------------Objetos------------------------------------------------*/

//Handle dos Eventos Wireless
static EventGroupHandle_t s_wifi_event_group;

//Declaração do server http
//static httpd_handle_t server =NULL;


/*-----------------------------------------------Declaração das Funções--------------------------------------*/

static void setup_PWM();     //Realiza as configurações iniciais do PWM.

static void setup_nvs();     //Inicia a memória nvs. Ela é necessária para se conectar à rede Wireless

void wifi_init_sta(); //Configura a rede wireless

/* Lida com os Eventos da rede Wireless(reconexão, IPs, etc), essa função é ativada durante a chamada de 
 * void wifi_init_sta() e permanece monitorando os eventos de rede e imprimindo no terminal
 * as tentativas de conexão e o endereço IP recebido que o ESP ganhou*/
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);

static void atualiza_PWM(); //Atualiza os valores de resolução do duty, frequencia e duty cicle

static int log_base2(int valor);




void app_main() {
    setup_PWM();
    setup_nvs();
    wifi_init_sta();

    //durante a execução:
    atualiza_PWM(0,0,0);





}

/*-------------------------------------Implementação das Funções----------------------------------------------*/

//Realiza as configurações iniciais do PWM.
static void setup_PWM(){
    ledc_timer_config_t pwm0_config = { //Configurações do PWM0
        .duty_resolution =   PWM_res_inicial,                  // resolution of PWM duty
        .freq_hz =           PWM0_freq,                         // frequency of PWM signal
        .speed_mode =        PWM_Modo,                         // timer mode
        .timer_num =         PWM0_Timer,                        // timer index
        .clk_cfg =           PWM_Clock,                        // clock source
    };
    // Aplicar a configuração do PWM0
    ledc_timer_config(&pwm0_config);

    ledc_timer_config_t pwm1_config = { //Configurações do PWM1
        .duty_resolution =   PWM_res_inicial,                  // resolution of PWM duty
        .freq_hz =           PWM1_freq,                         // frequency of PWM signal
        .speed_mode =        PWM_Modo,                         // timer mode
        .timer_num =         PWM1_Timer,                        // timer index
        .clk_cfg =           PWM_Clock,                        // clock source
    };
    // Aplicar a configuração do PWM1
    ledc_timer_config(&pwm1_config);

    ledc_channel_config_t pwm0_ch_config = {
        .gpio_num = PWM0_Gpio,
        .speed_mode = PWM_Modo,
        .channel =PWM0_Canal,
        .intr_type = 0,
        .timer_sel = PWM0_Timer,
        .duty =0,
        .hpoint =0,
    };

    ledc_channel_config(&pwm0_ch_config);

    ledc_channel_config_t pwm1_ch_config = {
        .gpio_num = PWM1_Gpio,
        .speed_mode = PWM_Modo,
        .channel =PWM1_Canal,
        .intr_type = 0,
        .timer_sel = PWM1_Timer,
        .duty =0,
        .hpoint =0,
    };

    ledc_channel_config(&pwm1_ch_config);
}

//retorna o log na base 2 do valor recebido
static int log_base2(int valor){
    return (log(valor)/log(2));

}

static void atualiza_PWM(int canal,uint32_t frequencia,uint32_t duty_cicle){

    //PWM Resolution(bits)= log2 (   PWMFreq    )
    //                             -------------
    //                             timer_clk_freq
    int resolucao_duty= log_base2(frequencia/Timers_Source_Speed);
    int timer;
    if(canal==PWM0_Canal)
        timer=PWM0_Timer;                         
    else 
        timer=PWM1_Timer;

    //Altera a Duty Resolution (bits)  
    ledc_timer_set(PWM_Modo,timer,1,resolucao_duty,PWM_Clock);


    //Atualiza Frequência
    ledc_set_freq(PWM_Modo,timer,frequencia);

    uint32_t duty = pow(2,resolucao_duty)*(duty_cicle/100);
  
    // Atualiza o duty cicle
    ledc_set_duty(PWM_Modo,canal,duty);


    // Aplica as configurações do Duty
    ledc_update_duty(PWM_Modo,canal);
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