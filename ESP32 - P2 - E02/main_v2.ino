#include <Arduino.h>
#include <Wire.h> 
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h> 
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"

// Pines
const uint8_t PIN_MQ135 = 32;
const uint8_t PIN_KY038 = 34;
const uint8_t PIN_LED_ROJO = 17;
const uint8_t PIN_LED_AZUL = 18;
const uint8_t PIN_LED_AMARILLO = 19;
const uint8_t PIN_BUZZER = 25;
const uint8_t PIN_BOTON = 26; 

// --- VARIABLES DE RED Y TOPICOS ---
const uint32_t ESPERA_INICIAL   = 2000;
const uint32_t ESPERA_MAXIMA    = 30000;
uint32_t esperaReconexion = ESPERA_INICIAL;
uint32_t tReconexion = 0; // tWiFi eliminado por completo para evitar colisión

String clientId, topicDatos, topicEstado, topicCmd;
// ----------------------------------

// Calibración y Umbrales
const float M_CAL_GAS = 1.0f;
const float B_CAL_GAS = 0.0f;
const float UMBRAL_PELIGRO_GAS = 400.0f;
const float UMBRAL_PELIGRO_RUIDO = 1000.0f;

// Variables de interrupción para el KY038 (volatile OBLIGATORIO para no perder datos en RAM)
volatile uint32_t contador_ruido_isr = 0;

// Tiempos en ms
const uint32_t INTERVALO_MQ135 = 1000; 
const uint32_t INTERVALO_KY038 = 1000; 
const uint32_t INTERVALO_OLED = 500;   
const uint32_t INTERVALO_MQTT = 15000; 
const uint32_t TIEMPO_WARMUP = 30000;  

// Variables de tiempo
uint32_t t_previo_mq135 = 0;
uint32_t t_previo_ky038 = 0;
uint32_t t_previo_oled = 0;
uint32_t t_previo_mqtt = 0;
uint32_t t_inicio = 0;

// Variables de estado
bool airePeligro = false;
bool ruidoPeligro = false;
float valor_final_gas = 0.0f;
float valor_final_ruido = 0.0f;

// Variables de Filtrado Gas
const uint8_t N_FILTRO_GAS = 10;
float ventana_gas[16];
uint8_t idx_gas = 0;
uint8_t validas_gas = 0;

// Botón de emergencia
uint32_t t_ultimo_cambio_boton = 0;
const uint32_t ANTIREBOTE_MS = 2000;

// Instancias de Hardware y Red
Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiClient espClient; 
PubSubClient client(espClient);

enum EstadoFSM : uint8_t { WARM_UP, STABLE, AIR_ALERT, NOISE_ALERT, EMERGENCY, ERROR_SYS };
EstadoFSM estadoActual = WARM_UP;

// --- INTERRUPCIÓN KY038 ---
// Esta función corre en hardware puro, independiente de si el loop() se bloquea por la red
void IRAM_ATTR isrRuido() {
    contador_ruido_isr++;
}

// Funciones Matemáticas
float aplicar_calibracion(float valor_crudo, float m, float b) {
    return (m * valor_crudo) + b;
}

float media_movil(float arreglo[], uint8_t validas) {
    if (validas == 0) return 0.0f;
    float suma = 0.0f;
    for (uint8_t i = 0; i < validas; i++) suma += arreglo[i];
    return suma / validas;
}

void actualizarSensores() {
    uint32_t ahora = millis();

    // 1. LECTURA GAS (MQ135)
    if (ahora - t_previo_mq135 >= INTERVALO_MQ135) {
        t_previo_mq135 = ahora;
        analogRead(PIN_MQ135); // Estabilizador de ADC
        float crudo = (float)analogRead(PIN_MQ135);
        
        ventana_gas[idx_gas] = aplicar_calibracion(crudo, M_CAL_GAS, B_CAL_GAS);
        idx_gas = (idx_gas + 1) % N_FILTRO_GAS;
        if (validas_gas < N_FILTRO_GAS) validas_gas++;

        valor_final_gas = media_movil(ventana_gas, validas_gas);
        airePeligro = (valor_final_gas >= UMBRAL_PELIGRO_GAS);
    }

    // 2. LECTURA DE KY038 (Cálculo seguro desde la interrupción)
    if (ahora - t_previo_ky038 >= INTERVALO_KY038) {
        t_previo_ky038 = ahora;

        // Bloqueamos interrupciones 1 microsegundo para no leer datos corruptos a medio cambiar
        noInterrupts();
        uint32_t pulsos_detectados = contador_ruido_isr;
        contador_ruido_isr = 0; // Reseteamos la cuenta para el próximo segundo
        interrupts();

        // Mapeo simple: si en 1 seg capta 50 picos (ajusta este valor al tornillo de tu sensor), es 100% ruidoso
        int nivel_pseudo_analogo = map(pulsos_detectados, 0, 50, 0, 100); 
        if (nivel_pseudo_analogo > 100) nivel_pseudo_analogo = 100;

        valor_final_ruido = (float)map(nivel_pseudo_analogo, 0, 100, 0, 4095);
        ruidoPeligro = (valor_final_ruido >= UMBRAL_PELIGRO_RUIDO);
    }
}

// Recepción de comandos MQTT
void recibirComando(char* topic, byte* payload, unsigned int largo) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, largo);
    if (error) {
        Serial.printf("[cmd] JSON invalido en %s: %s\n", topic, error.c_str());
        return;
    }
    Serial.printf("[cmd] recibido en %s\n", topic);
}

// MQTT sin bloquear y con testamento
void mantenerMQTT() {
    if (client.connected()) return;
    if (WiFi.status() != WL_CONNECTED) return;
    uint32_t ahora = millis();
    if (ahora - tReconexion < esperaReconexion) return;
    tReconexion = ahora;

    Serial.printf("[mqtt] conectando como %s ... ", clientId.c_str());
    if (client.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, topicEstado.c_str(), 1, true, "offline")) {
        Serial.println("OK");
        client.publish(topicEstado.c_str(), "online", true);   // estado retenido QoS 1
        client.subscribe(topicCmd.c_str(), 1);                 // resuscribir tópico
        esperaReconexion = ESPERA_INICIAL;
    } else {
        Serial.printf("FALLO rc=%d, reintento en %u s\n", client.state(), (unsigned)(esperaReconexion / 1000));
        esperaReconexion = (esperaReconexion * 2 > ESPERA_MAXIMA) ? ESPERA_MAXIMA : esperaReconexion * 2;
    }
}

// Publicación de JSON
void publicarDatos() {
    if (!client.connected()) return;

    JsonDocument doc;
    doc["estado_fsm"] = estadoActual;
    doc["mq135_aire"] = roundf(valor_final_gas * 10.0f) / 10.0f;
    doc["ky038_sonido"] = roundf(valor_final_ruido * 10.0f) / 10.0f;
    doc["alerta_aire"] = airePeligro ? 1 : 0;
    doc["alerta_ruido"] = ruidoPeligro ? 1 : 0;
    doc["rssi_dbm"] = WiFi.RSSI();

    char payload[256];
    size_t n = serializeJson(doc, payload, sizeof(payload));

    if (client.publish(topicDatos.c_str(), (const uint8_t*)payload, n, true)) {
        Serial.printf("[pub] %s -> %s\n", topicDatos.c_str(), payload);
    } else {
        Serial.println("[pub] ERROR publish() (buffer o sesion)");
    }
}

void botonEmergencia() {
    if (digitalRead(PIN_BOTON) == LOW && (millis() - t_ultimo_cambio_boton >= ANTIREBOTE_MS)) {
        t_ultimo_cambio_boton = millis();
        estadoActual = (estadoActual == ERROR_SYS) ? STABLE : ERROR_SYS;
    }
}

void actualizarPantalla() {
    if (estadoActual == ERROR_SYS) return;

    if (millis() - t_previo_oled >= INTERVALO_OLED) {
        t_previo_oled = millis();
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("PANTALLA DE DATOS");

        display.print("Estado: ");
        switch (estadoActual) {
            case WARM_UP: {
                display.println("WARM UP");
                uint32_t tiempo_pasado = millis() - t_inicio;
                if (tiempo_pasado > TIEMPO_WARMUP) tiempo_pasado = TIEMPO_WARMUP;

                uint8_t ancho_barra = map(tiempo_pasado, 0, TIEMPO_WARMUP, 0, 128);
                display.drawRect(0, 45, 128, 10, SSD1306_WHITE);
                display.fillRect(0, 45, ancho_barra, 10, SSD1306_WHITE);
                break;
            }
            case STABLE:
                display.println("STABLE");
                break;
            case AIR_ALERT:
                display.println("AIR ALERT");
                break;
            case NOISE_ALERT:
                display.println("NOISE ALERT");
                break;
            case EMERGENCY:
                display.println("EMERGENCY");
                break;
        }

        display.print("Gas: ");
        if (estadoActual == WARM_UP) {
            display.println("Calibrando...");
        } else {
            display.print(valor_final_gas);
            display.println(" PPM");
        }

        display.print("Ruido: ");
        if (estadoActual == WARM_UP) {
            display.println("Calibrando...");
        } else {
            display.print(valor_final_ruido);
            display.println(" ADC");
        }

        display.display();
    }
}

void procesarFSM() {
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

void setup() {
    Serial.begin(115200);
    analogReadResolution(12);

    pinMode(PIN_KY038, INPUT); 
    pinMode(PIN_LED_ROJO, OUTPUT);
    pinMode(PIN_LED_AZUL, OUTPUT);
    pinMode(PIN_LED_AMARILLO, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_BOTON, INPUT_PULLUP);

    // Activamos la interrupción por hardware para contar picos de ruido sin bloquear el loop()
    attachInterrupt(digitalPinToInterrupt(PIN_KY038), isrRuido, RISING);

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("Fallo al iniciar SSD1306");
        estadoActual = ERROR_SYS;
    } else {
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE); 
    }

    clientId    = String(MQTT_USER) + "-" + NODO;
    topicDatos  = String("curso/") + MQTT_USER + "/" + PROYECTO + "/" + NODO;
    topicEstado = topicDatos + "/estado";
    topicCmd    = topicDatos + "/cmd";
    Serial.printf("[id] Client ID: %s\n[id] Datos   : %s\n", clientId.c_str(), topicDatos.c_str());

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    client.setServer(MQTT_SERVER, MQTT_PORT);
    client.setCallback(recibirComando);
    client.setBufferSize(512);
    client.setKeepAlive(15);
    client.setSocketTimeout(3);

    t_inicio = millis();
}

void loop() {
    mantenerMQTT();
    client.loop();
    
    botonEmergencia();
    actualizarSensores();
    
    uint32_t ahora = millis();
    if (ahora - t_previo_mqtt >= INTERVALO_MQTT) {
        t_previo_mqtt = ahora;
        publicarDatos();
    }
    
    actualizarPantalla();
    procesarFSM();
}