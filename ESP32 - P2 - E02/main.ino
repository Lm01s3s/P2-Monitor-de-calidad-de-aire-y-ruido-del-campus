#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"

// Pines
const uint8_t PIN_MQ2 = 32;
const uint8_t PIN_KY038 = 34;
const uint8_t PIN_LED_ROJO = 17;
const uint8_t PIN_LED_AZUL = 18;
const uint8_t PIN_LED_VERDE = 19;
const uint8_t PIN_BUZZER = 25;
const uint8_t PIN_BOTON = 26;

// Variables de red y topicos
const uint32_t ESPERA_INICIAL = 2000;
const uint32_t ESPERA_MAXIMA = 30000;
uint32_t esperaReconexion = ESPERA_INICIAL;
uint32_t tReconexion = 0;
uint32_t t_previo_wifi = 0;
const uint32_t INTERVALO_RECONEXION_WIFI = 10000;

String clientId, topicDatos, topicEstado, topicCmd;

// Calibración y Umbrales
const float M_CAL_GAS = 1.0f;
const float B_CAL_GAS = 0.0f;
const float UMBRAL_PELIGRO_GAS = 400.0f;

const float M_CAL_RUIDO = 1.0f;
const float B_CAL_RUIDO = 0.0f;
const float UMBRAL_PELIGRO_RUIDO = 2500.0f;

// Tiempos en ms
const uint32_t INTERVALO_MQ2 = 1000;
const uint32_t INTERVALO_KY038 = 1000;
const uint32_t INTERVALO_OLED = 500;
const uint32_t INTERVALO_MQTT = 15000;
const uint32_t TIEMPO_WARMUP = 30000;

// Variables de tiempo
uint32_t t_previo_mq2 = 0;
uint32_t t_previo_ky038 = 0;
uint32_t t_previo_oled = 0;
uint32_t t_previo_mqtt = 0;
uint32_t t_inicio = 0;

// Variables de estado
bool airePeligro = false;
bool ruidoPeligro = false;
float valor_final_gas = 0.0f;
float valor_final_ruido = 0.0f;
bool ultimo_envio_ok = false;

// Variables de Filtrado Gas
const uint8_t N_FILTRO_GAS = 10;
float ventana_gas[16];
uint8_t idx_gas = 0;
uint8_t validas_gas = 0;

// Botón de emergencia
uint32_t t_ultimo_cambio_boton = 0;
const uint32_t ANTIREBOTE_MS = 2000;

// Instancias de Hardware y Red
Adafruit_SH1106G display(128, 64, &Wire, -1);
WiFiClient espClient;
PubSubClient client(espClient);

// Variables para control remoto por MQTT (GT6)
enum CmdRemoto { CMD_NINGUNO, CMD_ON, CMD_OFF };
CmdRemoto cmdRemoto = CMD_NINGUNO;
uint32_t t_actuacion = 0;
const uint32_t T_MAX_MS = 60000; // 60s maximo de actuacion por seguridad

// FSM del sistema
enum EstadoFSM : uint8_t { WARM_UP, STABLE, AIR_ALERT, NOISE_ALERT, EMERGENCY, ERROR_SYS, REMOTE_STATE };
EstadoFSM estadoActual = WARM_UP;

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

    // 1. LECTURA GAS MQ2
    if (ahora - t_previo_mq2 >= INTERVALO_MQ2) {
        t_previo_mq2 = ahora;
        analogRead(PIN_MQ2); // Lectura de sacrificio para estabilizar ADC
        float crudo = (float)analogRead(PIN_MQ2);

        ventana_gas[idx_gas] = aplicar_calibracion(crudo, M_CAL_GAS, B_CAL_GAS);
        idx_gas = (idx_gas + 1) % N_FILTRO_GAS;
        if (validas_gas < N_FILTRO_GAS) validas_gas++;

        valor_final_gas = media_movil(ventana_gas, validas_gas);
        airePeligro = (valor_final_gas >= UMBRAL_PELIGRO_GAS);
    }

    // 2. LECTURA DE KY038
    if (ahora - t_previo_ky038 >= INTERVALO_KY038) {
        t_previo_ky038 = ahora;

        int max_lectura = 0;
        int min_lectura = 4095;

        uint32_t inicio_muestreo = millis();
        while (millis() - inicio_muestreo < 50) {
            int lectura = analogRead(PIN_KY038);
            if (lectura > max_lectura) max_lectura = lectura;
            if (lectura < min_lectura) min_lectura = lectura;
        }

        int amplitud = max_lectura - min_lectura;

        int ruido_mapeado = map(amplitud, 0, 500, 0, 4095);

        if (ruido_mapeado > 4095) ruido_mapeado = 4095;
        if (ruido_mapeado < 0) ruido_mapeado = 0;

        float crudo_ruido = (float)ruido_mapeado;
        valor_final_ruido = aplicar_calibracion(crudo_ruido, M_CAL_RUIDO, B_CAL_RUIDO);

        ruidoPeligro = (valor_final_ruido >= UMBRAL_PELIGRO_RUIDO);
    }
}

// Recepción de comandos MQTT
void recibirComando(char* topic, byte* payload, unsigned int largo) {
    JsonDocument doc;
    // Deserializa el JSON inyectado desde Node-RED
    DeserializationError error = deserializeJson(doc, payload, largo);
    if (error) {
        Serial.printf("[cmd] JSON invalido en %s: %s\n", topic, error.c_str());
        return;
    }

    // Extraemos la orden exacta de la clave "cmd"
    const char* cmd = doc["cmd"];
    
    // Asignamos el estado a la variable global comparando el string
    if (strcmp(cmd, "ON") == 0) cmdRemoto = CMD_ON;
    else if (strcmp(cmd, "OFF") == 0) cmdRemoto = CMD_OFF;

    Serial.printf("[cmd] recibido en %s: %s\n", topic, cmd);
}

// Mantener el wifi sin bloquear
void mantenerWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    uint32_t ahora = millis();
    if (ahora - t_previo_wifi >= INTERVALO_RECONEXION_WIFI) {
        t_previo_wifi = ahora;
        // estado: 1 = no encuentra la red, 4 = clave mala, 6 = desconectado
        Serial.printf("[wifi] sin red (estado %d), reintentando...\n", WiFi.status());
        WiFi.reconnect();
    }
}

// MQTT sin bloquear
void mantenerMQTT() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (client.connected()) return;

    uint32_t ahora = millis();
    if (ahora - tReconexion < esperaReconexion) return;
    tReconexion = ahora;

    Serial.printf("[mqtt] conectando como %s ... ", clientId.c_str());
    if (client.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, topicEstado.c_str(), 1, true, "offline")) {
        Serial.println("OK");
        client.publish(topicEstado.c_str(), "online", true);
        client.subscribe(topicCmd.c_str(), 1);
        esperaReconexion = ESPERA_INICIAL;
    } else {
        Serial.printf("FALLO rc=%d, reintento en %u s\n", client.state(), (unsigned)(esperaReconexion / 1000));
        esperaReconexion = (esperaReconexion * 2 > ESPERA_MAXIMA) ? ESPERA_MAXIMA : esperaReconexion * 2;
    }
}

// Publicación de JSON
void publicarDatos() {
    if (!client.connected()) return;
    if (estadoActual == WARM_UP) return;

    JsonDocument doc;
    doc["estado_fsm"] = estadoActual;
    doc["mq2_aire"] = roundf(valor_final_gas * 10.0f) / 10.0f;
    doc["ky038_sonido"] = roundf(valor_final_ruido * 10.0f) / 10.0f;
    doc["alerta_aire"] = airePeligro ? 1 : 0;
    doc["alerta_ruido"] = ruidoPeligro ? 1 : 0;
    doc["rssi_dbm"] = WiFi.RSSI();

    char payload[256];
    size_t n = serializeJson(doc, payload, sizeof(payload));

    if (client.publish(topicDatos.c_str(), (const uint8_t*)payload, n, true)) {
        Serial.printf("[pub] %s -> %s\n", topicDatos.c_str(), payload);
        ultimo_envio_ok = true;
    } else {
        Serial.println("[pub] ERROR publish()");
        ultimo_envio_ok = false;
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

        display.print("Estado: ");
        switch (estadoActual) {
            case WARM_UP: {
                display.println("WARM UP");
                uint32_t tiempo_pasado = millis() - t_inicio;
                if (tiempo_pasado > TIEMPO_WARMUP) tiempo_pasado = TIEMPO_WARMUP;

                uint8_t ancho_barra = map(tiempo_pasado, 0, TIEMPO_WARMUP, 0, 128);
                display.drawRect(0, 45, 128, 10, SH110X_WHITE);
                display.fillRect(0, 45, ancho_barra, 10, SH110X_WHITE);
                break;
            }
            case STABLE: display.println("STABLE"); break;
            case AIR_ALERT: display.println("AIR ALERT"); break;
            case NOISE_ALERT: display.println("NOISE ALERT"); break;
            case REMOTE_STATE: display.println("REMOTE STATE"); break;
            case EMERGENCY: display.println("EMERGENCY"); break;
        }

        display.print("Gas: ");
        if (estadoActual == WARM_UP) {
            display.println("Espera...");
        } else {
            display.print(valor_final_gas);
            display.println(" ADC");
        }

        display.print("Ruido: ");
        if (estadoActual == WARM_UP) {
            display.println("Espera...");
        } else {
            display.print(valor_final_ruido);
            display.println(" ADC");
        }

        display.print("WiFi: ");
        if (WiFi.status() == WL_CONNECTED) {
            display.println("OK");
        } else {
            display.println("OFF");
        }

        display.print("MQTT: ");
        if (!client.connected()) {
            display.println("OFF"); return;
        }
        if (estadoActual == WARM_UP) {
            display.println("Espera...");
        } else {
            uint32_t tp_envio = millis() - t_previo_mqtt;
            if (tp_envio < 2000) {
                display.println(ultimo_envio_ok ? "OK!" : "ERR!");
            } else {
                if (tp_envio > INTERVALO_MQTT) tp_envio = INTERVALO_MQTT;
                uint8_t w_barra = map(tp_envio, 0, INTERVALO_MQTT, 0, 50);
                display.drawRect(display.getCursorX(), display.getCursorY(), 50, 8, SH110X_WHITE);
                display.fillRect(display.getCursorX(), display.getCursorY(), w_barra, 8, SH110X_WHITE);
                display.println();
            }
        }
        display.display();
    }
}

void procesarFSM() {
    switch (estadoActual) {
        case WARM_UP:
            if (millis() - t_inicio >= TIEMPO_WARMUP) estadoActual = STABLE;
            break;

        case STABLE:
            digitalWrite(PIN_LED_ROJO, LOW);
            digitalWrite(PIN_LED_AZUL, LOW);
            digitalWrite(PIN_LED_VERDE, LOW);
            digitalWrite(PIN_BUZZER, LOW);
            if (airePeligro && ruidoPeligro) estadoActual = EMERGENCY;
            else if (airePeligro) estadoActual = AIR_ALERT;
            else if (ruidoPeligro) estadoActual = NOISE_ALERT;
            else if (cmdRemoto == CMD_ON) {
                estadoActual = REMOTE_STATE;
                t_actuacion = millis();
            }
            break;

        case AIR_ALERT:
            digitalWrite(PIN_LED_ROJO, LOW);
            digitalWrite(PIN_LED_AZUL, HIGH);
            digitalWrite(PIN_LED_VERDE, LOW);
            digitalWrite(PIN_BUZZER, LOW);
            if (!airePeligro) estadoActual = STABLE;
            else if (ruidoPeligro) estadoActual = EMERGENCY;
            break;

        case NOISE_ALERT:
            digitalWrite(PIN_LED_ROJO, LOW);
            digitalWrite(PIN_LED_AZUL, LOW);
            digitalWrite(PIN_LED_VERDE, HIGH);
            digitalWrite(PIN_BUZZER, LOW);
            if (!ruidoPeligro) estadoActual = STABLE;
            else if (airePeligro) estadoActual = EMERGENCY;
            break;

        case EMERGENCY:
            digitalWrite(PIN_LED_ROJO, HIGH);
            digitalWrite(PIN_LED_AZUL, LOW);
            digitalWrite(PIN_LED_VERDE, LOW);
            digitalWrite(PIN_BUZZER, HIGH);
            if (!airePeligro && !ruidoPeligro) estadoActual = STABLE;
            else if (airePeligro && !ruidoPeligro) estadoActual = AIR_ALERT;
            else if (!airePeligro && ruidoPeligro) estadoActual = NOISE_ALERT;
            break;

        case ERROR_SYS:
            digitalWrite(PIN_LED_ROJO, HIGH);
            digitalWrite(PIN_LED_AZUL, LOW);
            digitalWrite(PIN_LED_VERDE, LOW);
            digitalWrite(PIN_BUZZER, LOW);
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("EMERGENCY BUTTON");
            display.display();
            break;

        case REMOTE_STATE:

            digitalWrite(PIN_LED_ROJO, LOW);
            digitalWrite(PIN_LED_AZUL, HIGH);
            digitalWrite(PIN_LED_VERDE, HIGH);
            digitalWrite(PIN_BUZZER, LOW);

            // Si Node-RED envia ON repetidamente, refrescamos el timer para no apagar
            if (cmdRemoto == CMD_ON) t_actuacion = millis(); 

            // Prioridad local estricta: si el entorno se vuelve peligroso, cortamos el remoto de raiz
            if (airePeligro || ruidoPeligro) {
                estadoActual = STABLE; 
                digitalWrite(PIN_LED_AZUL, LOW);
                digitalWrite(PIN_LED_VERDE, LOW);
            }
            // Timeout de seguridad de 60s o recepcion explicita de OFF
            else if (cmdRemoto == CMD_OFF || millis() - t_actuacion > T_MAX_MS) {
                estadoActual = STABLE;
                digitalWrite(PIN_LED_AZUL, LOW);
                digitalWrite(PIN_LED_VERDE, LOW);
            }
            break;
    }

    cmdRemoto = CMD_NINGUNO;
}

// Sistema de deteccion de redes
void escanearRedes() {
    Serial.println("[wifi] redes visibles:");
    int n = WiFi.scanNetworks();
    if (n == 0) {
        Serial.println("  ninguna");
        return;
    }
    for (int i = 0; i < n; i++) {
        Serial.printf("  %s  (%d dBm)\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    }
}

void setup() {
    Serial.begin(115200);
    analogReadResolution(12);

    pinMode(PIN_KY038, INPUT);
    pinMode(PIN_LED_ROJO, OUTPUT);
    pinMode(PIN_LED_AZUL, OUTPUT);
    pinMode(PIN_LED_VERDE, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_BOTON, INPUT_PULLUP);

    if (!display.begin(0x3C, true)) {
        Serial.println("Fallo al iniciar la pantalla");
        estadoActual = ERROR_SYS;
    } else {
        display.setTextSize(1);
        display.setTextColor(SH110X_WHITE);
    }

    clientId = String(MQTT_USER) + "-" + NODO;
    topicDatos = String("curso/") + MQTT_USER + "/" + PROYECTO + "/" + NODO;
    topicEstado = topicDatos + "/estado";
    topicCmd = topicDatos + "/cmd";

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);

    Serial.printf("[wifi] conectando a \"%s\"...", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED && intentos < 20) {
        delay(500);
        Serial.print(".");
        intentos++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(" OK!");
        Serial.print("[wifi] IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.printf(" FALLO (estado %d)\n", WiFi.status());
        Serial.println("[wifi] 1 = no encuentra la red | 4 = clave mala | 6 = desconectado");
        escanearRedes();
        Serial.println("[wifi] sigo reintentando en segundo plano.");
    }

    t_previo_wifi = millis();

    client.setServer(MQTT_SERVER, MQTT_PORT);
    client.setCallback(recibirComando);
    client.setBufferSize(512);
    client.setKeepAlive(15);
    client.setSocketTimeout(3);

    t_inicio = millis();
}

void loop() {
    mantenerWiFi();
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