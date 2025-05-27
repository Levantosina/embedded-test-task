#include <iostream>
#include <map>
#include <cstdint>
#include <string>
#include <cstring>
#include <mosquitto.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <sstream>   
#include <iomanip>

using json = nlohmann::json;

// Константы для задержек (в миллисекундах)
constexpr int MAIN_LOOP_DELAY = 100;       // 100ms задержка основного цикла
constexpr int MQTT_LOOP_DELAY = 10;        // 10ms задержка для обработки MQTT
constexpr int RECONNECT_DELAY = 5000;      // 5s задержка между попытками реконнекта
constexpr int MAX_RECONNECT_ATTEMPTS = 10; // Максимальное количество попыток реконнекта

// Эмуляция состояния пинов
std::map<uint8_t, bool> pinStates;
struct mosquitto *mosq = nullptr;
bool shouldRestart = false;
bool isConnected = false;
int reconnectAttempts = 0;
unsigned long lastTempPublish = 0;
// Пин для светодиодов
const int PIN_RED = 3;
const int PIN_GREEN = 5;
const int PIN_BLUE = 6;
// Пин для датчика температуры
const int A0 = 0;
//Топики для MQTT

const char *ERROR_TOPIC = "embedded/errors";
const char *EMBEDDED_CONTROL_TOPIC = "embedded/control";
const char *PUBLISH_TOPIC = "embedded/pins/state";
const char *TEMPERATURE_TOPIC = "sensor/temperature";

// Получение переменных окружения с значениями по умолчанию
std::string getEnvVar(const char *name, const char *defaultValue)
{
    const char *value = std::getenv(name);
    return value ? value : defaultValue;
}

void publish_message(struct mosquitto *mosq, const char *topic, const std::string &payload)
{
    int ret =
        mosquitto_publish(mosq, nullptr, topic, (int)payload.size(), payload.c_str(), 1, false);
    if (ret != MOSQ_ERR_SUCCESS)
    {
        std::cerr << "Publish error: " << mosquitto_strerror(ret) << std::endl;
    }
}

// Функция для подключения к MQTT
bool connectToMqtt()
{
    // Получение учетных данных из переменных окружения
    std::string mqttHost = getEnvVar("MQTT_HOST", "localhost");
    int mqttPort = std::stoi(getEnvVar("MQTT_PORT", "1883"));
    std::string mqttUsername = getEnvVar("MQTT_USERNAME", "");
    std::string mqttPassword = getEnvVar("MQTT_PASSWORD", "");

    std::cout << "Connecting to MQTT broker at " << mqttHost << ":" << mqttPort << std::endl;

    // Установка учетных данных
    if (!mqttUsername.empty() && !mqttPassword.empty())
    {
        int rc = mosquitto_username_pw_set(mosq, mqttUsername.c_str(), mqttPassword.c_str());
        if (rc != MOSQ_ERR_SUCCESS)
        {
            std::cerr << "Failed to set MQTT username/password: " << mosquitto_strerror(rc)
                      << std::endl;
            return false;
        }
    }

    // Подключение к брокеру
    int result = mosquitto_connect(mosq, mqttHost.c_str(), mqttPort, 60);
    if (result != MOSQ_ERR_SUCCESS)
    {
        std::cerr << "Unable to connect to MQTT broker: " << mosquitto_strerror(result)
                  << std::endl;
        return false;
    }

    // Обработка initial loop, чтобы MQTT начал работать
    mosquitto_loop_start(mosq);

    return true;
}

// Callback для подключения к MQTT
void connect_callback(struct mosquitto *mosq, void *obj, int result)
{
    if (result == MOSQ_ERR_SUCCESS)
    {
        std::cout << "Successfully connected to MQTT broker" << std::endl;
        isConnected = true;
        reconnectAttempts = 0;

        const char *subscribeTopics[] = {ERROR_TOPIC, EMBEDDED_CONTROL_TOPIC,
                                         PUBLISH_TOPIC, TEMPERATURE_TOPIC};
        // Подписываемся на топики после подключения
        for (const char *topic : subscribeTopics)
        {
            int rc = mosquitto_subscribe(mosq, nullptr, topic, 0);
            if (rc != MOSQ_ERR_SUCCESS)
            {
                std::cerr << "Failed to subscribe to topic: " << mosquitto_strerror(rc)
                          << std::endl;
            }
            else
            {
                std::cout << "Successfully subscribed to topic:" << topic << std::endl;
            }
        }
    }
    else
    {
        std::cerr << "Failed to connect to MQTT broker: " << mosquitto_strerror(result)
                  << std::endl;
        isConnected = false;
    }
}

// Callback для отключения от MQTT
void disconnect_callback(struct mosquitto *mosq, void *obj, int result)
{
    std::cout << "Disconnected from MQTT broker" << std::endl;
    isConnected = false;
}

// Функция для установки режима пина (вход/выход)
void pinMode(uint8_t pin, bool isOutput)
{
    std::cout << "Pin " << (int)pin << " set to " << (isOutput ? "OUTPUT" : "INPUT") << std::endl;
    pinStates[pin] = false; // Инициализация состояния пина
}

// Функция для чтения значения с пина
bool digitalRead(uint8_t pin)
{
    std::cout << "Reading from pin " << (int)pin << ": " << (pinStates[pin] ? "HIGH" : "LOW")
              << std::endl;
    return pinStates[pin];
}

// Функция для записи значения на пин
void digitalWrite(uint8_t pin, bool value)
{
    std::cout << "Writing to pin " << (int)pin << ": " << (value ? "HIGH" : "LOW") << std::endl;
    pinStates[pin] = value;

    // Отправляем состояние пина в MQTT только если подключены
    if (mosq && isConnected)
    {
        json message;
        message["pin"] = pin;
        message["value"] = value;
        std::string payload = message.dump();

        std::cout << "Publishing MQTT message to topic 'embedded/pins/state': " << payload
                  << std::endl;

        // Добавляем обработку ошибок и повторные попытки публикации
        int retries = 3;
        while (retries > 0)
        {
            int rc =
                mosquitto_publish(mosq, nullptr, EMBEDDED_CONTROL_TOPIC, payload.length(),
                                  payload.c_str(), 1, false); // QoS=1 для гарантированной доставки
            if (rc == MOSQ_ERR_SUCCESS)
            {
                std::cout << "Successfully published MQTT message" << std::endl;

               // вызвать mosquitto_loop для обработки исходящих сообщений
                mosquitto_loop(mosq, 100, 1); // Время на обработку сообщения
                break;
            }
            else if (rc == MOSQ_ERR_NO_CONN)
            {
                std::cerr << "No connection to broker, attempting to reconnect..." << std::endl;
                if (connectToMqtt())
                {
                    mosquitto_loop(mosq, 100, 1);
                }
            }
            else
            {
                std::cerr << "Failed to publish MQTT message: " << mosquitto_strerror(rc)
                          << std::endl;
            }
            retries--;
            if (retries > 0)
            {
                std::cout << "Retrying publish... (" << retries << " attempts left)" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}

int analogRead(int pin)
{
    if (pin == A0)
    {
        // Генерация температуры от 20-30 ° C
        int minVal = static_cast<int>((20.0 / 50.0) * 1023);
        int maxVal = static_cast<int>((30.0 / 50.0) * 1023);
        return minVal + std::rand() % (maxVal - minVal + 1);
    }
    return 0; 
}

void publishTemperature(struct mosquitto *mosq)
{
    int analogValue = analogRead(A0);
    // Переводим analogValue обратно в температуру °C
    float tempC = (analogValue / 1023.0f) * 50.0f;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << tempC;

    json doc;
    doc["temperature"] = oss.str(); // 2 знака после запятой
    doc["unit"] = "Celsius";

    std::string buffer = doc.dump();
    publish_message(mosq, TEMPERATURE_TOPIC, buffer);
}

// Callback для получения сообщений MQTT
void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
    // Проверяем, что сообщение не пустое
    if (!message->payload)
    {
        std::cout << "Received empty message" << std::endl;
        return;
    }
    //Получаем топик и полезную нагрузку сообщения
    std::string topic(message->topic);
    std::string payload(static_cast<char *>(message->payload), message->payloadlen);

    std::cout << "Received message on topic: " << topic << ", payload: " << payload << std::endl;

    json response; //Обьявляем json объект для ответа
    try
    {
        // Парсим JSON из полезной нагрузки
        json data = json::parse(payload);

        // Проверяем, что полученные данные являются JSON-объектом и на нужный топик
        if (topic == "embedded/control")
        {
            //// Извлекаем значение команды из JSON
            std::string command = data["command"];

            // Команда для перезапуска устройства
            if (command == "restart")
            {
                std::cout << "Received restart command" << std::endl;
                bool currentState = digitalRead(2);
                digitalWrite(2, !currentState);
                shouldRestart = true;
                // // Выводим успешный ответ
                response = {{"status", "OK"}, {"message", "Device will restart"}};
            }

            //установка RGB 
            else if (command == "set_rgb")
            {
                // Проверяем наличие red, green и blue в JSON
                if (!data.contains("red") || !data.contains("green") || !data.contains("blue"))
                {
                    publish_message(mosq, ERROR_TOPIC,
                                    R"({"status":"ERROR","message":"Missing RGB values"})");
                    return;
                }
                // Извлекаем значения RGB
                int red = data["red"].get<int>();
                int green = data["green"].get<int>();
                int blue = data["blue"].get<int>();
                //Установка диапазона от 0 до 255
                if (red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255)
                {
                    publish_message(
                        mosq, ERROR_TOPIC,
                        R"({"status":"ERROR","message":"RGB values must be between 0 and 255"})");
                    return;
                }
                
                //Вывод значений RGB
                std::cout << "Setting RGB values: R=" << red << ", G=" << green << ", B=" << blue
                          << std::endl;
                //Ответ
                response = {{"status", "OK"}, {"red", red}, {"green", green}, {"blue", blue}};
            }
            else
            {
                // Неизвестная команда
                response = {{"status", "ERROR"}, {"message", "Unknown command: " + command}};
            }

            // Отправляем ответ (если он не был отправлен ранее)
            publish_message(mosq, PUBLISH_TOPIC, response.dump());
        }
    }catch (const std::exception& e) {
        //обработка исключения при парсинге
        std::cerr << "Error parsing JSON: " << e.what() << std::endl;
    }
}
// Функция setup - выполняется один раз при старте
void setup()
{
    std::cout << "Setup started" << std::endl;

    if (mosq)
    {
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        mosq = nullptr;
        isConnected = false;
    }

    // Инициализация MQTT библиотеки (только при первом вызове)
    static bool mosquittoInitialized = false;
    if (!mosquittoInitialized)
    {
        mosquitto_lib_init();
        mosquittoInitialized = true;
    }

    mosq = mosquitto_new("embedded-controller", true, nullptr);
    if (!mosq)
    {
        std::cerr << "Error: Out of memory." << std::endl;
        return;
    }

    // Установка callback'ов
    mosquitto_connect_callback_set(mosq, connect_callback);
    mosquitto_disconnect_callback_set(mosq, disconnect_callback);
    mosquitto_message_callback_set(mosq, message_callback);

    // Настройка пинов
    pinMode(13, true); // Пин 13 как выход
    pinMode(2, false); // Пин 2 как вход

    // Инициализация rgb пинов как выходы
    pinMode(PIN_RED, true);
    pinMode(PIN_GREEN, true);
    pinMode(PIN_BLUE, true);

    pinMode(A0, false); // A0 как вход(аналог)

    // Попытка первоначального подключения
    if (connectToMqtt())
    {
        std::cout << "Initial MQTT connection successful" << std::endl;
    }
    else
    {
        std::cerr << "Initial MQTT connection failed" << std::endl;
    }

    std::cout << "Setup completed" << std::endl;
}

// Функция loop - выполняется циклически
void loop()
{
    static bool ledState = false;
    static auto lastReconnectAttempt = std::chrono::steady_clock::now();

    // Проверка подключения и попытка реконнекта
    if (!isConnected)
    {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReconnectAttempt)
                .count() >= RECONNECT_DELAY)
        {
            if (reconnectAttempts < MAX_RECONNECT_ATTEMPTS)
            {
                std::cout << "Attempting to reconnect to MQTT broker (attempt "
                          << (reconnectAttempts + 1) << ")" << std::endl;
                if (connectToMqtt())
                {
                    reconnectAttempts = 0;
                }
                else
                {
                    reconnectAttempts++;
                }
                lastReconnectAttempt = now;
            }
            else
            {
                std::cerr << "Max reconnection attempts reached. Giving up." << std::endl;
            }
        }
    }

    // Если получена команда перезапуска
    if (shouldRestart)
    {
        std::cout << "Restarting..." << std::endl;
        std::cout << "Waiting 3 seconds before restart..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        shouldRestart = false;
        setup(); // Перезапускаем setup
        return;
    }

    // Чтение значения с пина 2
    bool buttonState = digitalRead(2);

    // Если кнопка нажата (пин 2 в HIGH), переключаем светодиод
    if (buttonState)
    {
        ledState = !ledState;
        digitalWrite(13, ledState);
    }

    // Задержка основного цикла
    std::this_thread::sleep_for(std::chrono::milliseconds(MAIN_LOOP_DELAY));
}
unsigned long millis()
{
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
}
int main()
{
    setup();

    // Эмуляция бесконечного цикла
    while (true)
    {
        loop();

        // Проверяем каждые ~100 мс, нужно ли опубликовать температуру
        if (millis() - lastTempPublish > 5000)
        {
            publishTemperature(mosq);
            lastTempPublish = millis();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // Очистка MQTT 
    if (mosq)
    {
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
    }
    mosquitto_lib_cleanup();

    return 0;
}