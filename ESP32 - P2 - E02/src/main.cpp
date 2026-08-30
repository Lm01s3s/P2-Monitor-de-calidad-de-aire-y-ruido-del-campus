#include <Arduino.h>

#include <Wire.h>                  // pantalla led
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>      // pantalla led

#include <WiFi.h>

#include <PubSubClient.h>          // manejo de datos
#include <ArduinoJson.h>           // manejo de datos


// modifiquen los pines aqui
const uint8_t PIN_MQ135 = 32; 
const uint8_t PIN_KY038 = 33; 

const uint8_t PIN_LED_ROJO = ;
const uint8_t PIN_LED_AZUL = ;
const uint8_t PIN_LED_AMARILLO = ;

const uint8_t PIN_BUZZER = ;

const uint8_t PIN_BOTON = ;        // la wea de emergencia
// modifiquen los pines aqui

// para el net y el manejo de datos
const char* WIFI_SSID = "nombre de la wifi";
const char* WIFI_PASS = "clave de la wifi";

const char* MQTT_SERVER = "ip del broker";
const uint16_t MQTT_PORT = 1883;
// para el net y el manejo de datos

// para la calibracion (y = mx + b)
const float M_CAL_GAS   = 1.0f;
const float B_CAL_GAS   = 0.0f;

const float M_CAL_RUIDO = 1.0f;
const float B_CAL_RUIDO = 0.0f;
// para la calibracion (y = mx + b)

// umbral para alertas
const float UMBRAL_PELIGRO_GAS   = 400.0f; 
const float UMBRAL_PELIGRO_RUIDO = 80.0f;
// umbral para alertas

// tipos de filtrado
const uint8_t N_FILTRO_GAS   = 10;
const bool USAR_MEDIANA_GAS   = false; 

const uint8_t N_FILTRO_RUIDO = 15;
const bool USAR_MEDIANA_RUIDO = true;
// tipos de filtrado

// tiempos en ms
const uint32_t INTERVALO_MQ135  = 1000;       // gas
const uint32_t INTERVALO_KY038  = 50;         // ruido
const uint32_t INTERVALO_OLED   = 500;        // hz de la pantalla
const uint32_t INTERVALO_MQTT   = 15000;      // envio de datos
const uint32_t INTERVALO_RECON  = 7500;       // comprueba la net
const uint32_t TIEMPO_WARMUP    = 30000;      // tiempo de inicializacion
// tiempos en ms

// tiempo interno del sistema (millis)
uint32_t t_previo_mq135 = 0;
uint32_t t_previo_ky038 = 0;
uint32_t t_previo_oled  = 0;
uint32_t t_previo_mqtt  = 0;
uint32_t t_previo_recon = 0;
uint32_t t_inicio       = 0;
// tiempo interno del sistema (millis)

// comprobacion para la fsm
bool airePeligro  = false;
bool ruidoPeligro = false;
// comprobacion para la fsm

// valores calibrados
float valor_final_gas = 0.0f;
float valor_final_ruido = 0.0f;
// valores calibrados

// valores utiles en calculos y punteros
float   ventana_gas[16];
uint8_t idx_gas = 0;
uint8_t validas_gas = 0;

float   ventana_ruido[16];
uint8_t idx_ruido = 0;
uint8_t validas_ruido = 0;
// valores utiles en calculos y punteros

// valores para el boton de emergencia
uint32_t t_ultimo_cambio_boton = 0;
const uint32_t ANTIREBOTE_MS = 2000;
// valores para el boton de emergencia

// pantalla y wifi
Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiClient espClient;
PubSubClient client(espClient);

// FSM
enum EstadoFSM : uint8_t {
    WARM_UP,
    STABLE,
    AIR_ALERT,
    NOISE_ALERT,
    EMERGENCY,
    ERROR_SYS 
};

EstadoFSM estadoActual = WARM_UP;

// funciones matematicas
float aplicar_calibracion(float valor_crudo, float m, float b) {
    return (m * valor_crudo) + b;
}

float media_movil(float arreglo[], uint8_t validas) {
    if (validas == 0) return 0.0f;
    float suma = 0.0f;
    for (uint8_t i = 0; i < validas; i++) suma += arreglo[i];
    return suma / validas;
}

float mediana(float arreglo[], uint8_t validas) {
    if (validas == 0) return 0.0f;
    float copia[16];
    for (uint8_t i = 0; i < validas; i++) copia[i] = arreglo[i];
    for (uint8_t i = 1; i < validas; i++) {
        float clave = copia[i];
        int8_t j = i - 1;
        while (j >= 0 && copia[j] > clave) { 
            copia[j + 1] = copia[j]; 
            j--; 
        }
        copia[j + 1] = clave;
    }
    return copia[validas / 2];
}

// funciones logicas
void actualizarSensores() {
    uint32_t ahora = millis();

    if (ahora - t_previo_mq135 >= INTERVALO_MQ135) {
        t_previo_mq135 = ahora;

        float crudo = (float)analogRead(PIN_MQ135);
        float calibrado = aplicar_calibracion(crudo, M_CAL_GAS, B_CAL_GAS);

        ventana_gas[idx_gas] = calibrado;
        idx_gas = (idx_gas + 1) % N_FILTRO_GAS;

        if (validas_gas < N_FILTRO_GAS) validas_gas++;

        valor_final_gas = USAR_MEDIANA_GAS ? mediana(ventana_gas, validas_gas) : media_movil(ventana_gas, validas_gas);

        airePeligro = (valor_final_gas >= UMBRAL_PELIGRO_GAS);
    }

    if (ahora - t_previo_ky038 >= INTERVALO_KY038) {
        t_previo_ky038 = ahora;

        float crudo = (float)analogRead(PIN_KY038);
        float calibrado = aplicar_calibracion(crudo, M_CAL_RUIDO, B_CAL_RUIDO);

        ventana_ruido[idx_ruido] = calibrado;
        idx_ruido = (idx_ruido + 1) % N_FILTRO_RUIDO;

        if (validas_ruido < N_FILTRO_RUIDO) validas_ruido++;

        valor_final_ruido = USAR_MEDIANA_RUIDO ? mediana(ventana_ruido, validas_ruido) : media_movil(ventana_ruido, validas_ruido);

        ruidoPeligro = (valor_final_ruido >= UMBRAL_PELIGRO_RUIDO);
    }
}

void actualizarPantalla() {
    if (estadoActual == ERROR_SYS){
      return;
    }

    if (millis() - t_previo_oled >= INTERVALO_OLED) {
        t_previo_oled = millis();
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("PANTALLA DE DATOS");
        
        display.print("Estado: ");
        display.println(estadoActual);
        
        display.print("Gas: ");
        display.print(valor_final_gas);
        display.println(" PPM");
        
        display.print("Ruido: ");
        display.print(valor_final_ruido);
        display.println(" dB");
        
        display.display();
    }
}

void manejarConexion() {
    if (millis() - t_previo_recon >= INTERVALO_RECON) {
        t_previo_recon = millis();
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.begin(WIFI_SSID, WIFI_PASS);
        } else if (!client.connected()) {
            client.connect("ESP32_EdgeNode");
        }
    }
    client.loop(); // Mantiene vivo el protocolo MQTT
}

void publicarJSON() {
    if (client.connected() && (millis() - t_previo_mqtt >= INTERVALO_MQTT)) {
        t_previo_mqtt = millis();
        
        // Asignacion estatica para evitar desbordamiento de RAM
        StaticJsonDocument<200> doc;
        doc["estado_fsm"] = estadoActual;
        doc["gas_ppm"] = valor_final_gas;
        doc["ruido_db"] = valor_final_ruido;
        doc["alerta_aire"] = airePeligro;
        doc["alerta_ruido"] = ruidoPeligro;

        char buffer[200];
        serializeJson(doc, buffer);
        client.publish("sensores/telemetria", buffer);
    }
}

void botonEmergencia() {
  bool estadoBoton = digitalRead(PIN_BOTON);

  if (estadoBoton == LOW && (millis() - t_ultimo_cambio_boton >= ANTIREBOTE_MS)){
    t_ultimo_cambio_boton = millis();

    if (estadoActual == ERROR_SYS){
      estadoActual = STABLE;
    } else {
      estadoActual = ERROR_SYS;
    }
  }
}

void setup() {
    Serial.begin(115200); 
    analogReadResolution(12);
    
    pinMode(PIN_LED_ROJO, OUTPUT);
    pinMode(PIN_LED_AZUL, OUTPUT);
    pinMode(PIN_LED_AMARILLO, OUTPUT);

    pinMode(PIN_BUZZER, OUTPUT);

    pinMode(PIN_BOTON, INPUT_PULLUP);
    
    // Inicializacion I2C de la OLED (Direccion 0x3C es la mas comun)
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("Fallo al iniciar SSD1306");
        estadoActual = ERROR_SYS;
    } else {
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
    }

    WiFi.mode(WIFI_STA);
    client.setServer(MQTT_SERVER, MQTT_PORT);
    
    t_inicio = millis();
}

void loop() {
    botonEmergencia();
    actualizarSensores();
    manejarConexion();
    publicarJSON();
    actualizarPantalla();

    switch (estadoActual) {
        case WARM_UP:
            if (millis() - t_inicio >= TIEMPO_WARMUP) {
                estadoActual = STABLE;
            }
            break;

        case STABLE:

            digitalWrite(PIN_LED_ROJO, LOW);
            digitalWrite(PIN_LED_AZUL, LOW);
            digitalWrite(PIN_LED_AMARILLO, LOW);

            digitalWrite(PIN_BUZZER, LOW);
          
            if (airePeligro && ruidoPeligro) {
                estadoActual = EMERGENCY;
            } else if (airePeligro && !ruidoPeligro) {
                estadoActual = AIR_ALERT;
            } else if (!airePeligro && ruidoPeligro) {
                estadoActual = NOISE_ALERT;
            }
            break;

        case AIR_ALERT:

            digitalWrite(PIN_LED_ROJO, LOW);
            digitalWrite(PIN_LED_AZUL, HIGH);
            digitalWrite(PIN_LED_AMARILLO, LOW);

            digitalWrite(PIN_BUZZER, LOW);

            if (!airePeligro) {
                estadoActual = STABLE;
            } else if (airePeligro && ruidoPeligro) {
                estadoActual = EMERGENCY;
            }
            break;

        case NOISE_ALERT:
            
            digitalWrite(PIN_LED_ROJO, LOW);
            digitalWrite(PIN_LED_AZUL, LOW);
            digitalWrite(PIN_LED_AMARILLO, HIGH);

            digitalWrite(PIN_BUZZER, LOW);

            if (!ruidoPeligro) {
                estadoActual = STABLE;
            } else if (airePeligro && ruidoPeligro) {
                estadoActual = EMERGENCY;
            }
            break;

        case EMERGENCY:

            digitalWrite(PIN_LED_ROJO, HIGH);
            digitalWrite(PIN_LED_AZUL, LOW);
            digitalWrite(PIN_LED_AMARILLO, LOW);

            digitalWrite(PIN_BUZZER, HIGH);

            if (!airePeligro && !ruidoPeligro) {
                estadoActual = STABLE;
            } else if (airePeligro && !ruidoPeligro) {
                estadoActual = AIR_ALERT;
            } else if (!airePeligro && ruidoPeligro) {
                estadoActual = NOISE_ALERT;
            }
            break;

        case ERROR_SYS:
            digitalWrite(PIN_LED_ROJO, HIGH);
            digitalWrite(PIN_LED_AZUL, LOW);
            digitalWrite(PIN_LED_AMARILLO, LOW);

            digitalWrite(PIN_BUZZER, LOW);

            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("EMERGENCY BUTTON");
            display.display();
            break;
    }
}