#pragma once
#include <Wire.h>
#include "i2c_protocol.h"

/**
 * I2CSlave — класс для ESP32 в роли I2C слейва
 *
 * Использование:
 *   1. Создайте экземпляр: I2CSlave slave;
 *   2. Зарегистрируйте обработчики команд через onCommand()
 *   3. Вызовите begin() в setup()
 *   4. Вызывайте process() в каждой итерации loop()
 *
 * Пример:
 *   I2CSlave slave;
 *
 *   void setup() {
 *     slave.onCommand(REG_PING, [](const uint8_t*, uint8_t) {
 *       uint8_t resp[] = {0x01};
 *       slave.respond(resp, 1);
 *     });
 *     slave.begin();
 *   }
 *
 *   void loop() {
 *     slave.process();
 *   }
 */

// Тип обработчика команды: принимает буфер запроса и его длину
typedef void (*CommandHandler)(const uint8_t* buf, uint8_t len);

class I2CSlave {
public:

    // ─── Конфигурация ────────────────────────────────────────────────────────

    void begin(uint8_t address = SLAVE_ADDR,
               int sda = 21, int scl = 22, uint32_t clock = 100000) {
        Wire.begin((uint8_t)address);
        Wire.setClock(clock);
        Wire.onReceive([](int n) { instance->_onReceive(n); });
        Wire.onRequest([]()      { instance->_onRequest();  });
        instance = this;
    }

    // Зарегистрировать обработчик для команды (регистра)
    // Максимум MAX_HANDLERS команд
    bool onCommand(uint8_t reg, CommandHandler handler) {
        if (_handlerCount >= MAX_HANDLERS) return false;
        _handlers[_handlerCount++] = {reg, handler};
        return true;
    }

    // ─── Вызывается из loop() ─────────────────────────────────────────────────

    void process() {
        if (!_hasRequest) return;

        // Копируем из volatile буфера
        uint8_t buf[RX_BUF_SIZE];
        uint8_t len = _rxLen;
        for (uint8_t i = 0; i < len; i++) buf[i] = _rxBuf[i];

        uint8_t reg = buf[0];
        bool handled = false;
        Serial.println(reg);
        for (uint8_t i = 0; i < _handlerCount; i++) {
            if (_handlers[i].reg == reg) {
                _handlers[i].handler(buf, len);
                handled = true;
                break;
            }
        }

        if (!handled) {
            Serial.println("Не известная команда: "+reg);
            // Неизвестная команда
            uint8_t resp[] = {STATUS_ERROR};
            respond(resp, 1);
        }

        _hasRequest = false;
        _isReady    = true;
    }

    // ─── Вызывается из обработчиков команд ───────────────────────────────────

    // Подготовить ответ (данные + CRC)
    void respond(const uint8_t* data, uint8_t len) {
        memcpy(_txBuf, data, len);
        _txBuf[len] = crc8(data, len);
        _txLen = len + 1;
    }

    // Подготовить ответ из одного байта (удобный хелпер)
    void respondByte(uint8_t byte) {
        respond(&byte, 1);
    }

private:
    // ─── Константы ───────────────────────────────────────────────────────────
    static const uint8_t RX_BUF_SIZE   = 8;
    static const uint8_t TX_BUF_SIZE   = 16;
    static const uint8_t MAX_HANDLERS  = 16;

    // ─── Буферы ──────────────────────────────────────────────────────────────
    volatile uint8_t _rxBuf[RX_BUF_SIZE] = {};
    volatile uint8_t _rxLen      = 0;
    volatile bool    _hasRequest = false;
    volatile bool    _isReady    = false;

    uint8_t _txBuf[TX_BUF_SIZE] = {};
    uint8_t _txLen = 0;

    // ─── Таблица обработчиков ─────────────────────────────────────────────────
    struct HandlerEntry {
        uint8_t        reg;
        CommandHandler handler;
    };
    HandlerEntry _handlers[MAX_HANDLERS] = {};
    uint8_t      _handlerCount = 0;

    // Синглтон для колбэков Wire (Wire требует статические функции)
    static I2CSlave* instance;

    // ─── I2C Callbacks (ISR) ─────────────────────────────────────────────────

    void _onReceive(int numBytes) {
        if (numBytes <= 0 || numBytes > RX_BUF_SIZE) return;
        if (_hasRequest) return;  // предыдущий запрос не обработан

        _isReady = false;
        _rxLen = 0;
        for (uint8_t i = 0; i < (uint8_t)numBytes; i++)
            _rxBuf[_rxLen++] = Wire.read();
        _hasRequest = true;
    }

    void _onRequest() {
        if (!_isReady) {
            for (uint8_t i = 0; i < TX_BUF_SIZE; i++)
                Wire.write(STATUS_BUSY);
            return;
        }
        Wire.write(_txBuf, _txLen);
    }
};

// Определение статического члена (должно быть в .cpp или в одном .ino)
I2CSlave* I2CSlave::instance = nullptr;
