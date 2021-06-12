#include <esp_wifi.h>               //Conexão Wireless
#include "esp_log.h"                //log de eventos no monitor serial
#include <esp_http_server.h>        //biblioteca para poder usar o server http
#include "freertos/event_groups.h"  //grupo de eventos
#include "nvs_flash.h"              //memória nvs
#include "driver/ledc.h"            //PWM
#include <math.h>                   //Função log




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

static const char *TAG = "LOG";             //A tag que será impressa no log do sistema 




typedef struct parametros_PWM
{
    bool                     estado; //ligado =1 , desligado=0
    uint32_t             frequencia; //frequencia do PWM   
    uint8_t   percentual_duty_cicle; //ciclo alto do PWM de 0 a 100%
    uint8_t                   timer; //qual timer é associado a esse pwm
    uint8_t                   canal; //qual canal é associado a esse pwm
    uint8_t       resolucao_inicial;
    uint8_t              speed_mode;
    uint8_t                   clock;
    uint8_t                    gpio;
}parametros_PWM;


/*----------------------------------------------------Objetos------------------------------------------------*/

//Handle dos Eventos Wireless
static EventGroupHandle_t s_wifi_event_group;

//Declaração do server http
static httpd_handle_t server =NULL;





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

//Cria o Server, Faz as configurações Padrão e Inicia os URI Handlers para os GETs
static httpd_handle_t start_webserver(void);

//Imprimimos a Webpage
static void print_webpage(httpd_req_t *req);

//handler do Get da Página Principal
static esp_err_t main_page_get_handler(httpd_req_t *req);

static void cria_delay(void *pvParameter);


/*--------------------------------------Declaração dos GETs do http------------------------------------------*/
//a declaração do GET da página Principal
static const httpd_uri_t main_page = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = main_page_get_handler,
    .user_ctx  = NULL
};


    struct parametros_PWM  pwm0,pwm1;   //cria a struct com os parametros do pwm
void app_main() {
    inicializa_variaveis(&pwm0,&pwm1);  //inicializa variaveis como frequencia, duty cicle.. etc
    setup_PWM(pwm0);                    //configura os canais e timers do PWM0
    setup_PWM(pwm1);                    //configura os canais e timers do PWM1
    setup_nvs();                        //inicia a memória nvs necessária para uso do wireless
    wifi_init_sta();                    //inicia o wireless e se conecta à rede
    server = start_webserver();   //configura e inicia o server
    xTaskCreate(&cria_delay, "cria_delay", 512,NULL,5,NULL );

}

/*-------------------------------------Implementação das Funções----------------------------------------------*/

//Função executada no início da execução.. inicia os valores do pwm
static void inicializa_variaveis(struct parametros_PWM  *pwm0,struct parametros_PWM  *pwm1)
{
    pwm0->estado=0;                              //desligado
    pwm0->frequencia=1000;                       //1000 Hz
    pwm0->percentual_duty_cicle=0 ;              //0%
    pwm0->timer=0;                               //timer0
    pwm0->canal=0;                               //canal0
    pwm0->gpio=PWM0_Gpio;                        //gpio definido
    pwm0->resolucao_inicial=LEDC_TIMER_13_BIT;   //13 bits de resolução inicial;
    pwm0->speed_mode       =LEDC_HIGH_SPEED_MODE;//Modo High speed;
    pwm0->clock            =LEDC_USE_APB_CLK;    //A fonte de clock do timer. APB_CLK = 80MHz;

    pwm1->estado=0;                              //desligado
    pwm1->frequencia=1000;                       //1000 Hz
    pwm1->percentual_duty_cicle=0;               //0%
    pwm1->timer=1;                               //timer0
    pwm1->canal=1;                               //canal0
    pwm1->gpio=PWM1_Gpio;                        //gpio definido
    pwm1->resolucao_inicial=LEDC_TIMER_13_BIT;   //13 bits de resolução inicial;
    pwm1->speed_mode       =LEDC_HIGH_SPEED_MODE;//Modo High speed;
    pwm1->clock            =LEDC_USE_APB_CLK;    //A fonte de clock do timer. APB_CLK = 80MHz;
}


static void cria_delay(void *pvParameter)
{
    while(1)
    {
        vTaskDelay(100 / portTICK_RATE_MS);
    }
    
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
    static uint32_t numero_tentativa_de_conexao_wifi = 0;//numero atual da tentativa de se conectar a rede,
                                                         //tentativas máximas= EXAMPLE_ESP_MAXIMUM_RETRY
    
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

//Cria o Server, Faz as configurações Padrão e Inicia os URI Handlers
//para os GETs
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server   = NULL;
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Inicia o server http
    printf("Iniciando o Server na Porta: '%d'\n", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        printf("Registrando URI handlers\n");
        httpd_register_uri_handler(server, &main_page);
        return server;
    }

    printf("Erro ao iniciar Server\n");
    return NULL;
}

//Imprime a Webpage
static void print_webpage(httpd_req_t *req)
{
    char *buffer;
    buffer = (char *) malloc(3500);
    //Constantes Cstring para Armazenar os pedaços do código HTML 
    const char *index_html_part1= "<!DOCTYPE html><html><head><meta content=\"text/html;charset=utf-8\" http-equiv=\"Content-Type\"> <meta content=\"utf-8\" http-equiv=\"encoding\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><link rel=\"icon\" href=\"data:,\"> <title>Projeto 10 - Gerador PWM controlado via Wireless</title><style>html{color: #ffffff;font-family: Verdana;text-align: center;background-color:#272727fd}.wrap {padding-top: 2.5%;padding-right: 2.5%;padding-left: 2.5%;width: 95%;overflow:auto;}.fleft {border-style: solid;border-radius: 1px;border-width: 2px;border-color: #bdbdbd;float:left; width: 47.5%;background: rgb(95, 95, 95);height:fit-content;padding-bottom: 3%;}.fright {border-style: solid;border-radius: 1px;border-width: 2px;border-color: #bdbdbd;float: right;width: 47.5%;background:rgb(95, 95, 95);height:fit-content;padding-bottom: 3%; } .fcenter {float: center;width: 5%; background:#272727fd;height:fit-content; padding-bottom: 3%;}.formulario{padding: 0px 0px;float: center;width: 95%;text-align:center;}.campo_freq{width: 30%;text-align: center;font-family:sans-serif;font-size: 14px;font-weight: bold;border-style: solid;border-radius: 1px;border-width: 3px;border-color: #000000;}.campo_duty{-webkit-appearance: none;width: 28%;height: 5px;background: #ffffff;outline: none;opacity: 1;-webkit-transition: .2s;transition: opacity .2s;    }.campo_duty::-webkit-slider-thumb{-webkit-appearance: none;appearance: none;background: #04AA6D;}.btn_submit{text-align: center;background-color: #02500f;font-size: 20px;font-family: sans-serif;font-weight: bold;color: #ffffff;border-style: solid; border-radius: 1px;border-width: 3px;border-color: #ffffff;} </style> </head> <body><h2>Projeto 10 - Gerador PWM controlado via Wireless</h2><div class=\"wrap\"><div class=\"fleft\"><h3>PWM 0</h3><form class=\"formulario\"><label>Frequência: </label><input class=\"campo_freq\" type=\"text\" value=";


    char pwm0frequencia[10];
    sprintf(pwm0frequencia, "\"%d\"", pwm0.frequencia);


    const char *index_html_part2= "autocomplete=\"off\"><label>  Hz </label><br><br> <label>Duty Cicle:&#160; </label><input class=\"campo_duty\" type=\"range\" min=\"0\" max=\"100\" value=";
    

    char pwm0percentual[6];
    sprintf(pwm0percentual, "\"%d\"", pwm0.percentual_duty_cicle);
    

    const char *index_html_part3="id=\"myRange\"><label> <span id=\"demo\"></span>%</label><br><br><label>Output: </label><input name=\"estado\" class= \"campo_saida\" type=\"radio\" ";
    

    const char *pwmchecked="checked=\"checked\"";


    const char *index_html_part4="><label>Ligado </label><input name=\"estado\" class= \"campo_saida\" type=\"radio\"";


    const char *index_html_part5="><label>Desligado </label><br><br><input class=\"btn_submit\" type=\"submit\" value=\"Atualizar\"></form>  </div><div class=\"fright\"><h3>PWM 1</h3><form class=\"formulario\"><label>Frequência: </label><input class=\"campo_freq\" type=\"text\" value=";
    

    char pwm1frequencia[10];
    sprintf(pwm1frequencia, "\"%d\"", pwm1.frequencia);
    

    const char *index_html_part6="autocomplete=\"off\"><label>  Hz </label><br><br> <label>Duty Cicle:&#160; </label><input class=\"campo_duty\" type=\"range\" min=\"0\" max=\"100\" value=";
    

    char pwm1percentual[6];
    sprintf(pwm1percentual, "\"%d\"", pwm1.percentual_duty_cicle);
    

    const char *index_html_part7="id=\"myRange2\"><label> <span id=\"demo2\"></span>%</label><br><br><label>Output: </label>   <input name=\"estado\" class= \"campo_saida\" type=\"radio\"";
    




    const char *index_html_part8="><label>Desligado </label><br><br><input class=\"btn_submit\" type=\"submit\" value=\"Atualizar\"></form></div></div><script>var slider = document.getElementById(\"myRange\");var output = document.getElementById(\"demo\");output.innerHTML = slider.value;slider.oninput = function() {output.innerHTML = this.value;}; var slider2 = document.getElementById(\"myRange2\");var output2 = document.getElementById(\"demo2\");output2.innerHTML = slider2.value;slider2.oninput = function() {output2.innerHTML = this.value;}</script></body></html>";
   


    strcpy(buffer, index_html_part1);
    strcat(buffer, pwm0frequencia);
    strcat(buffer, index_html_part2);
    strcat(buffer, pwm0percentual);
    strcat(buffer, index_html_part3);
    if(pwm0.estado)
    {
        strcat(buffer, pwmchecked);
    }
    strcat(buffer, index_html_part4);
    if(!pwm0.estado)
    {
        strcat(buffer, pwmchecked);
    }
    strcat(buffer, index_html_part5);
    strcat(buffer, pwm1frequencia);
    strcat(buffer, index_html_part6);
    strcat(buffer, pwm1percentual);
    strcat(buffer, index_html_part7);
    if(pwm1.estado)
    {
        strcat(buffer, pwmchecked);
    }
    strcat(buffer, index_html_part4);
    if(!pwm0.estado)
    {
        strcat(buffer, pwmchecked);
    }
    strcat(buffer, index_html_part8);


    httpd_resp_send(req, buffer, strlen(buffer)); //envia via http o buffer que contém a página html completa

    vTaskDelay(3000 / portTICK_RATE_MS);
    free(buffer);

}


//handler do Get da Página Principal
static esp_err_t main_page_get_handler(httpd_req_t *req)
{
    //imprime a página
    print_webpage(req);
    //retorna OK
    return ESP_OK;
}