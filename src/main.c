#include <esp_wifi.h>               //Conexão Wireless
#include "esp_log.h"                //log de eventos no monitor serial
#include <esp_http_server.h>        //biblioteca para poder usar o server http
#include "freertos/event_groups.h"  //grupo de eventos
#include "nvs_flash.h"              //memória nvs
#include "driver/ledc.h"            //PWM
#include <math.h>                   //Função log
#include <sys/param.h>              //Função MIN




/*------------------------Mapeamento de Hardware----------------------*/
//Vamos definir quais pinos do ESP serão usados como saída do PWM0 e PWM1
#define pino_PWM0    18
#define pino_PMW1    19



/*------------------------Definições de Projeto-----------------------*/
/* O event Group do wireless permite muitos eventos por grupo mas nos só nos importamos com 2 eventos:
 * - Conseguimos conexão com o AP com um IP
 * - Falhamos na conexão apos o número máximo de tentativas*/
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1


/*Aqui definimos o SSID, a Senha e o número máximo de tentativas que vamos 
tentar ao se conectar ao access point da rede wireless*/
#define ESP_WIFI_SSID      "tira_o_zoio"
#define ESP_WIFI_PASS      "jabuticaba"
#define ESP_MAXIMUM_RETRY  10


/*-----------------------------------------------Constantes de Projeto --------------------------------------*/

static const char *TAG = "LOG";             //A tag que será impressa no log do sistema 



/*----------------------------------------------------Objetos------------------------------------------------*/

//Handle dos Eventos Wireless
static EventGroupHandle_t s_wifi_event_group;

//Handle do server http
static httpd_handle_t server =NULL;


/*-----------------------------------------------Declaração das Funções--------------------------------------*/

static void setup_PWM();     //Realiza as configurações iniciais do PWM.

static void setup_nvs();     //Inicia a memória nvs. Ela é necessária para se conectar à rede Wireless

void wifi_init_sta(); //Configura a rede wireless

/* Lida com os Eventos da rede Wireless(reconexão, IPs, etc), essa função é ativada durante a chamada de 
 * void wifi_init_sta() e permanece monitorando os eventos de rede e imprimindo no terminal
 * as tentativas de conexão e o endereço IP recebido que o ESP ganhou*/
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);

//static void atualiza_PWM(struct parametros_PWM pwm); //Atualiza os valores de resolução do duty, frequencia e duty cicle

static int calc_resolucao_duty(long double pwm_freq,long double timer_clk_freq);

//Cria o Server, Faz as configurações Padrão e Inicia os URI Handlers para os GETs
static httpd_handle_t start_webserver(void);

//Imprimimos a Webpage
static void print_webpage(httpd_req_t *req);

//handler do Get da Página Principal
static esp_err_t main_page_get_handler(httpd_req_t *req);

//handler do formulário html do pwm0
static esp_err_t pwm0_post_handler(httpd_req_t *req);

//handler do formulário html do pwm1
static esp_err_t pwm1_post_handler(httpd_req_t *req);

static void atualiza_PWM(int pwm_index,uint32_t frequencia, uint32_t percentual_duty);

static void cria_delay(void *pvParameter);


/*--------------------------------------Declaração dos GETs do http------------------------------------------*/
//a declaração do GET da página Principal
static const httpd_uri_t main_page = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = main_page_get_handler,
    .user_ctx  = NULL
};

// URI handler do formulário do pwm0 
static const httpd_uri_t post_pwm0 = {
    .uri      = "/pwm0",
    .method   = HTTP_POST,
    .handler  = pwm0_post_handler,
    .user_ctx = NULL
};

// URI handler do formulário do pwm1
static const httpd_uri_t post_pwm1 = {
    .uri      = "/pwm1",
    .method   = HTTP_POST,
    .handler  = pwm1_post_handler,
    .user_ctx = NULL
};

void app_main() {
    setup_PWM();                    //configura os canais e timers do PWM0
    setup_nvs();                    //inicia a memória nvs necessária para uso do wireless
    wifi_init_sta();                //inicia o wireless e se conecta à rede
    server = start_webserver();     //configura e inicia o server
   // atualiza_PWM(0,5, 30);
   // atualiza_PWM(1,10, 70);
    xTaskCreate(&cria_delay, "cria_delay", 512,NULL,5,NULL );

}

/*-------------------------------------Implementação das Funções----------------------------------------------*/


//Realiza as configurações iniciais do PWM baseado na documentação de Espressiv:
//https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/ledc.html
static void setup_PWM()
{

    //************************Struct e Configuração do Timer0******************************
    ledc_timer_config_t pwm_timer_config = { //Configurações do PWM0
        .duty_resolution =   LEDC_TIMER_4_BIT,       // resolução do duty do PWM
        .freq_hz =           16,                   // frequencia do sinal PWM
        .speed_mode =        LEDC_LOW_SPEED_MODE,   // modo do timer
        .timer_num =         LEDC_TIMER_0,           // identificador do timer
        .clk_cfg =           LEDC_USE_APB_CLK,       // fonte de clock
    };
    // Aplicar os parâmetros da struct no timer
    ESP_ERROR_CHECK(ledc_timer_config(&pwm_timer_config));

    //************************Struct e Configuração do Timer1****************************************
    pwm_timer_config.timer_num = LEDC_TIMER_1;
    // Aplicar os parâmetros da struct no timer 1
    ESP_ERROR_CHECK(ledc_timer_config(&pwm_timer_config));

    //************************Struct e Configuração do channel0******************************
    ledc_channel_config_t pwm_channel_config = {
        .gpio_num   = pino_PWM0,                  //Gpio de saída do sinal PWM
        .speed_mode = LEDC_HIGH_SPEED_MODE,       //modo do PWM
        .channel    = LEDC_CHANNEL_0,             //Canal do PWM
        .intr_type  = LEDC_INTR_DISABLE,          //interrupções
        .timer_sel  = LEDC_TIMER_0,               //qual timer está asociado a esse canal
        .duty       = 10,                          //duty cicle inicial
        .hpoint     = 0,         
    };
    // Aplicar os parâmetros da struct no canal
    ESP_ERROR_CHECK(ledc_channel_config(&pwm_channel_config));



    //************************Struct e Configuração do channel1****************************************
    pwm_channel_config.gpio_num  = pino_PMW1;
    pwm_channel_config.channel   = LEDC_CHANNEL_1;
    pwm_channel_config.timer_sel = LEDC_TIMER_1;
    pwm_channel_config.duty      = 2;

    // Aplicar os parâmetros da struct no canal 1
    ESP_ERROR_CHECK(ledc_channel_config(&pwm_channel_config));
}



/*----Inicializa a memória nvs pois é necessária para o funcionamento do Wireless---------*/
static void setup_nvs()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}



/*----------------Lida com os Eventos da rede Wireless, conexão à rede e endereço IP-----------------------*/
//essa função é executada em segundo plano
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

/*---------------------------Inicializa a Conexão Wireless-------------------------*/
void wifi_init_sta()
{
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




/*--------------Cria o Server, Faz as configurações Padrão e Inicia os URI Handlers--------------*/
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
        httpd_register_uri_handler(server, &post_pwm0);
        httpd_register_uri_handler(server, &post_pwm1);
        return server;
    }

    printf("Erro ao iniciar Server\n");
    return NULL;
}

/*---Essa função concatena a página web como um vetor char e a envia como resposta da requisição req---*/
static void print_webpage(httpd_req_t *req)
{
    char *buffer;
    buffer = (char *) malloc(3500);
    
    const char *index_html_part1= "<!DOCTYPE html><html><head><meta content=\"text/html;charset=utf-8\" http-equiv=\"Content-Type\"> <meta content=\"utf-8\" http-equiv=\"encoding\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><link rel=\"icon\" href=\"data:,\"> <title>Projeto 10 - Gerador PWM controlado via Wireless</title><style>html{color: #ffffff;font-family: Verdana;text-align: center;background-color:#272727fd}.wrap {padding-top: 2.5%;padding-right: 2.5%;padding-left: 2.5%;width: 95%;overflow:auto;}.fleft {border-style: solid;border-radius: 1px;border-width: 2px;border-color: #bdbdbd;float:left; width: 47.5%;background: rgb(95, 95, 95);height:fit-content;padding-bottom: 3%;}.fright {border-style: solid;border-radius: 1px;border-width: 2px;border-color: #bdbdbd;float: right;width: 47.5%;background:rgb(95, 95, 95);height:fit-content;padding-bottom: 3%; } .fcenter {float: center;width: 5%; background:#272727fd;height:fit-content; padding-bottom: 3%;}.formulario{padding: 0px 0px;float: center;width: 95%;text-align:center;}.campo_freq{width: 30%;text-align: center;font-family:sans-serif;font-size: 14px;font-weight: bold;border-style: solid;border-radius: 1px;border-width: 3px;border-color: #000000;}.campo_duty{-webkit-appearance: none;width: 28%;height: 5px;background: #ffffff;outline: none;opacity: 1;-webkit-transition: .2s;transition: opacity .2s;    }.campo_duty::-webkit-slider-thumb{-webkit-appearance: none;appearance: none;background: #04AA6D;}.btn_submit{text-align: center;background-color: #02500f;font-size: 20px;font-family: sans-serif;font-weight: bold;color: #ffffff;border-style: solid; border-radius: 1px;border-width: 3px;border-color: #ffffff;} </style> </head> <body><h2>Projeto 10 - Gerador PWM controlado via Wireless</h2><div class=\"wrap\"><div class=\"fleft\"><h3>PWM 0</h3><form class=\"formulario\" method=\"post\"><label>Frequência: </label><input class=\"campo_freq\" type=\"text\" value=";

    char pwm0frequencia[10];
   // sprintf(pwm0frequencia, "\"%d\"", pwm0.frequencia);

    const char *index_html_part2= "autocomplete=\"off\"><label>  Hz </label><br><br> <label>Duty Cicle:&#160; </label><input class=\"campo_duty\" type=\"range\" min=\"0\" max=\"100\" value=";

    char pwm0percentual[6];
  //  sprintf(pwm0percentual, "\"%d\"", pwm0.percentual_duty_cicle);
    
    const char *index_html_part3="id=\"myRange\"><label> <span id=\"demo\"></span>%</label><br><br><label>Output: </label><input name=\"estado\" class= \"campo_saida\" type=\"radio\" ";
    
    const char *pwmchecked="checked=\"checked\"";

    const char *index_html_part4="><label>Ligado </label><input name=\"estado\" class= \"campo_saida\" type=\"radio\"";

    const char *index_html_part5="><label>Desligado </label><br><br><input class=\"btn_submit\" type=\"submit\" value=\"Atualizar\" name=\"pwm0\"></form>  </div><div class=\"fright\"><h3>PWM 1</h3><form class=\"formulario\" method=\"post\"><label>Frequência: </label><input class=\"campo_freq\" type=\"text\" value=";
    
    char pwm1frequencia[10];
   // sprintf(pwm1frequencia, "\"%d\"", pwm1.frequencia);
    
    const char *index_html_part6="autocomplete=\"off\"><label>  Hz </label><br><br> <label>Duty Cicle:&#160; </label><input class=\"campo_duty\" type=\"range\" min=\"0\" max=\"100\" value=";
    
    char pwm1percentual[6];
   // sprintf(pwm1percentual, "\"%d\"", pwm1.percentual_duty_cicle);
    
    const char *index_html_part7="id=\"myRange2\"><label> <span id=\"demo2\"></span>%</label><br><br><label>Output: </label>   <input name=\"estado\" class= \"campo_saida\" type=\"radio\"";
    
    const char *index_html_part8="><label>Desligado </label><br><br><input class=\"btn_submit\" type=\"submit\" value=\"Atualizar\" name=\"pwm1\"></form></div></div><script>var slider = document.getElementById(\"myRange\");var output = document.getElementById(\"demo\");output.innerHTML = slider.value;slider.oninput = function() {output.innerHTML = this.value;}; var slider2 = document.getElementById(\"myRange2\");var output2 = document.getElementById(\"demo2\");output2.innerHTML = slider2.value;slider2.oninput = function() {output2.innerHTML = this.value;}</script></body></html>";
   
    strcpy(buffer, index_html_part1);
    strcat(buffer, pwm0frequencia);
    strcat(buffer, index_html_part2);
    strcat(buffer, pwm0percentual);
    strcat(buffer, index_html_part3);
   // if(pwm0.estado)
    {
        strcat(buffer, pwmchecked);
    }
    strcat(buffer, index_html_part4);
  //  if(!pwm0.estado)
    {
        strcat(buffer, pwmchecked);
    }
    strcat(buffer, index_html_part5);
    strcat(buffer, pwm1frequencia);
    strcat(buffer, index_html_part6);
    strcat(buffer, pwm1percentual);
    strcat(buffer, index_html_part7);
   // if(pwm1.estado)
    {
        strcat(buffer, pwmchecked);
    }
    strcat(buffer, index_html_part4);
   // if(!pwm0.estado)
    {
        strcat(buffer, pwmchecked);
    }
    strcat(buffer, index_html_part8);


    httpd_resp_send(req, buffer, strlen(buffer)); //envia via http o buffer que contém a página html completa

    vTaskDelay(3000 / portTICK_RATE_MS);
    free(buffer);

}


/*--------------handler do Get da Página Principal html-------------------*/
static esp_err_t main_page_get_handler(httpd_req_t *req)
{
    //imprime a página
    print_webpage(req);
    //retorna OK
    return ESP_OK;
}

static esp_err_t pwm0_post_handler(httpd_req_t *req)
{

    char buf[100];
    int ret, remaining = req->content_len;

    while (remaining > 0) {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf,
                        MIN(remaining, sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG,"post pwm0");
    print_webpage(req);
    
    
    return ESP_OK;
}

static esp_err_t pwm1_post_handler(httpd_req_t *req)
{
    
    char buf[100];
    int ret, remaining = req->content_len;

    while (remaining > 0) 
    {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf,
                        MIN(remaining, sizeof(buf)))) <= 0) 
        {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) 
            {
                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG,"post pwm1");
    print_webpage(req);
    return ESP_OK;
}


//retorna o log na base 2 do valor recebido
static int calc_resolucao_duty(long double pwm_freq,long double timer_clk_freq)


    //PWM duty Resolution(bits)= |log2 (   PWMFreq      )|
    //                           |      --------------   |
    //                           |      timer_clk_freq   |
{
    long double frac = pwm_freq/timer_clk_freq;
    long double retorno= (logl(frac)/logl(2));
    return (uint32_t)abs((int)retorno);
}


//Atualiza o sinal PWM baseado no canal selecionado, na frequência pedida e na porcentagem do duty
static void atualiza_PWM(int pwm_index,uint32_t frequencia, uint32_t percentual_duty){


    int timer_clk_freq;                 // 80 MHz é a velocidade do clock APB. Na função 
                                        // "inicializa variaveis", se quiser usar outro clock, usando
                                        // um valor diferente de LEDC_USE_APB_CLK terá que adaptar
                                        // a variavel "timer_clk_freq" com a frequência do clock
                                        // selecionado que achar no datasheet
    
    timer_clk_freq=80000000;

    uint32_t resolucao_duty= calc_resolucao_duty(frequencia,timer_clk_freq);

    ESP_LOGI(TAG,"Atualizar PWM: Freq: %d, Duty: %d, Resolução: %d",
            frequencia,
            percentual_duty,
            resolucao_duty);

    //aplica a Resolução do Duty(bits) no timer correto  
    ESP_ERROR_CHECK(ledc_timer_set(LEDC_HIGH_SPEED_MODE,pwm_index,1,resolucao_duty,LEDC_USE_APB_CLK));

    //Atualiza Frequência no timer correto
    ESP_ERROR_CHECK(ledc_set_freq(LEDC_HIGH_SPEED_MODE,pwm_index,frequencia));

    //transforma o valor do duty de percentual para bits
    double duty_double = (pow(2.0,(double)resolucao_duty)*((double)percentual_duty/100.0));
    uint32_t duty = (uint32_t)duty_double;

    ESP_LOGE(TAG, "Duty: %zu",duty);
    // Atualiza o duty cicle do canal
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE,pwm_index,duty));

    // Aplica as configurações
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE,pwm_index));

    ESP_LOGE(TAG, " duty retornado: %d",ledc_get_duty(LEDC_HIGH_SPEED_MODE,pwm_index));
}



static void cria_delay(void *pvParameter)
{
    while(1)
    {
        vTaskDelay(100 / portTICK_RATE_MS);
    }
    
}