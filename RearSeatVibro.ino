#include "I2CSlave.h"

bool isDebug=true;
bool isTest=false;
int testTimer=0;

I2CSlave slave;
void SaveError(uint8_t code);

#define PIN_L_IN_1  2
#define PIN_L_IN_2  3
#define PIN_L_IN_3  4
#define PIN_L_IN_4  5
#define PIN_R_IN_1  6
#define PIN_R_IN_2  7
#define PIN_R_IN_3  8
#define PIN_R_IN_4  9

enum MassageMode : uint8_t {
  MODE_OFF   = 0,
  MODE_CALM  = 1,
  MODE_FLOW  = 2,
  MODE_MIXED = 3
};

struct Step {
  uint8_t  mask;   // биты [D][C][B][A], 1 = включён
  uint16_t ms;     // длительность шага
};

// ============================================================
//  РЕЖИМ 1 — CALM (спокойный, медитативный)
//
//  Правило: ни один шаг без активной группы.
//  Каждый переход — через шаг перекрытия:
//    A → A+B → B → B+D → D → C+D → C → C+A → [repeat]
//
//  Цикл ~11 секунд. Ощущение: медленные океанские волны.
// ============================================================
static const Step PAT_CALM[] PROGMEM = {
  { 0b0001, 2000 },  // A           перёд сидушки
  { 0b0011,  800 },  // A+B         B нарастает, A ещё работает
  { 0b0010, 2000 },  // B           зад сидушки
  { 0b1010,  800 },  // B+D         волна уходит в спину
  { 0b1000, 2000 },  // D           поясница
  { 0b1100,  800 },  // C+D         подъём вверх по спинке
  { 0b0100, 2000 },  // C           верх спинки
  { 0b0101,  800 },  // C+A         возврат в круг через диагональ
};

// ============================================================
//  РЕЖИМ 2 — FLOW (плавный, перекатный)
//
//  Основной круг с диагональными вставками.
//  Кроссовые шаги (B+C, D+A) дают ощущение связи
//  между сидушкой и спинкой — как руки массажиста.
//  Ни одной прямой смены группы без перекрытия.
//
//  Цикл ~5.9 секунд. Ощущение: шведский массаж.
// ============================================================
static const Step PAT_FLOW[] PROGMEM = {
  { 0b0001,  550 },  // A
  { 0b0011,  280 },  // A+B         ← B добавляется
  { 0b0010,  550 },  // B
  { 0b0110,  300 },  // B+C         ← кросс: сидушка и спинка вместе
  { 0b0100,  550 },  // C
  { 0b1100,  280 },  // C+D         ← D добавляется снизу
  { 0b1000,  550 },  // D
  { 0b1001,  350 },  // D+A         ← диагональ: поясница и перёд сидушки
  { 0b0101,  450 },  // A+C         ← диагональная пауза: перёд + верх спинки
  { 0b0001,  300 },  // A
  { 0b0011,  200 },  // A+B
  { 0b0010,  400 },  // B
  { 0b1010,  350 },  // B+D         ← диагональ: зад + поясница
  { 0b1000,  350 },  // D
  { 0b1100,  200 },  // D+C         ← возврат в основной круг
};

// ============================================================
//  РЕЖИМ 3 — MIXED (активный, микшированный)
//
//  Быстрые диагонали, ритмичные синкопы, пульс всех групп,
//  короткие паузы для контраста. Никаких прямых прыжков —
//  все переходы через смежные или диагональные состояния.
//
//  Цикл ~3.1 секунды. Ощущение: перкуссионный / спортивный.
// ============================================================
static const Step PAT_MIXED[] PROGMEM = {
  { 0b0001,  220 },  // A
  { 0b0101,  180 },  // A+C         диагональ вверх
  { 0b0100,  220 },  // C
  { 0b1100,  180 },  // C+D
  { 0b1000,  220 },  // D
  { 0b1010,  180 },  // B+D         диагональ вниз
  { 0b0010,  220 },  // B
  { 0b0110,  180 },  // B+C         кросс
  { 0b0100,  160 },  // C
  { 0b0101,  160 },  // A+C
  { 0b0001,  200 },  // A
  { 0b0011,  160 },  // A+B         сидушка целиком
  { 0b1010,  200 },  // B+D         диагональ
  { 0b1001,  200 },  // A+D         диагональ
  { 0b1111,  180 },  // ALL         акцент — все группы
  { 0b0000,  150 },  // —           пауза (только в этом режиме)
  { 0b1010,  180 },  // B+D
  { 0b0101,  180 },  // A+C
  { 0b0000,  130 },  // —           пауза
};

// Таблица паттернов для удобного доступа по индексу режима
struct PatternInfo {
  const Step* data;
  uint8_t     len;
};

static const PatternInfo PATTERNS[] = {
  { nullptr,   0 },
  { PAT_CALM,  sizeof(PAT_CALM)  / sizeof(Step) },
  { PAT_FLOW,  sizeof(PAT_FLOW)  / sizeof(Step) },
  { PAT_MIXED, sizeof(PAT_MIXED) / sizeof(Step) },
};

// ============================================================
//  Класс SeatMassage
// ============================================================
class SeatMassage {
public:
  // Текущий режим — менять напрямую из loop / обработчиков
  MassageMode mode;

  SeatMassage(uint8_t pinA, uint8_t pinB, uint8_t pinC, uint8_t pinD)
    : mode(MODE_OFF)
    , _step(0)
    , _tStep(0)
    , _prevMode(MODE_OFF)
  {
    _pins[0] = pinA;
    _pins[1] = pinB;
    _pins[2] = pinC;
    _pins[3] = pinD;
  }

  void begin() {
    for (uint8_t i = 0; i < 4; i++) {
      pinMode(_pins[i], OUTPUT);
      digitalWrite(_pins[i], LOW);
    }
  }

  // Вызывать из loop() — неблокирующий
  void runMassage() {
    const unsigned long now = millis();

    if (mode == MODE_OFF) {
      if (_prevMode != MODE_OFF) {
        _applyMask(0x00);
        _prevMode = MODE_OFF;
      }
      return;
    }

    const PatternInfo& pi = PATTERNS[mode];

    // Смена режима: сброс и немедленный первый шаг
    if (_prevMode != mode) {
      _step     = 0;
      _tStep    = now;
      _prevMode = mode;
      _applyCurrentStep(pi);
      return;
    }

    // Переход к следующему шагу по истечении времени
    Step s;
    memcpy_P(&s, &pi.data[_step], sizeof(Step));
    if (now - _tStep >= (unsigned long)s.ms) {
      _tStep = now;
      _step  = (_step + 1) % pi.len;
      _applyCurrentStep(pi);
    }
  }

private:
  uint8_t       _pins[4];    // [A, B, C, D]
  uint8_t       _step;
  unsigned long _tStep;
  MassageMode   _prevMode;

  void _applyMask(uint8_t mask) {
    for (uint8_t i = 0; i < 4; i++) {
      digitalWrite(_pins[i], (mask >> i) & 1 ? HIGH : LOW);
    }
  }

  void _applyCurrentStep(const PatternInfo& pi) {
    Step s;
    memcpy_P(&s, &pi.data[_step], sizeof(Step));
    _applyMask(s.mask);
  }
};

// ============================================================
//  Экземпляры кресел
// ============================================================
SeatMassage seatL(PIN_L_IN_1, PIN_L_IN_2, PIN_L_IN_3, PIN_L_IN_4);
SeatMassage seatR(PIN_R_IN_1, PIN_R_IN_2, PIN_R_IN_3, PIN_R_IN_4);

byte L_Mode=0;
byte R_Mode=0;

byte modeSeq[]={0, 3, 2, 1};

//Ошибки в памяти
struct Error{
  uint8_t code=0;
  uint32_t tfs=0;
  uint8_t times=0;
}__attribute__((packed));
Error errors[1];
int sizeErr;
int errLen;
int nextError=0;

struct ErrorDesc {
    uint8_t code;
    const char* description;
};
const ErrorDesc errorDescriptions[] PROGMEM = {
    {51,   "Left vibro wrong state"},
    {52,   "Right vibro wrong state"},
    {0,   ""} //terminator (обязательно в конце!)
};

void setup() {
  seatL.begin();
  seatR.begin();
  Serial.begin(115200);
  InitEEPROM();
  
  slave.onCommand(REG_PING, cmdPing);
  slave.onCommand(REG_GetErrorCount, cmdGetErrorCount);
  slave.onCommand(REG_GetNextError, cmdGetError);
  slave.onCommand(REG_ClearErrors, cmdClearErrors);
  slave.onCommand(REG_L_MODE, cmdMode);
  slave.onCommand(REG_R_MODE, cmdMode);
  slave.onCommand(REG_L_GetStatus, cmdGetStatus);
  slave.onCommand(REG_R_GetStatus, cmdGetStatus);
  slave.begin();
}

void loop() {
  slave.process();

  seatL.runMassage();
  seatR.runMassage();

  if(isTest && millis()-testTimer > 5000) {
    testTimer=millis();
    ClickHardware(0);
    ClickHardware(1);
  }

  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    command.toLowerCase();

    if (command == "mode0") {
      ClickHardware(0);
      Serial.println(L_Mode);
    } else if (command == "mode1") {
      ClickHardware(1);
      Serial.println(R_Mode);
    } else if (command == "test") {
      isTest = !isTest;
      Serial.println(isTest ? "Тест включён" : "Тест выключен");
    } else if (command == "eeprom init") {
      Serial.println("Сброс памяти к заводским настройкам...");
      FirstInit();
      LoadErrors();
      Serial.println("Готово");
    } else if (command == "eeprom read") {
      Serial.println("Вывод содержимого памяти...");
      LoadErrors();
      for(int i=0;i<errLen;i++)
      {
        Serial.print("#");
        Serial.print(errors[i].code);
        Serial.print("|");
        Serial.print(errors[i].times);
        Serial.print("|");
        Serial.println(errors[i].tfs);
      }
      Serial.println("Готово");
    } else {
      Serial.println("Команды: mode0 | mode1 | test | eeprom init | eeprom read");
    }
  }
  delay(5);
}

//0-left; 1-right
void ClickHardware(int seatNum){
  if(seatNum==0)
  {
    L_Mode=GetNextMode(L_Mode);
    logI("Seat #0", L_Mode);
    seatL.mode=L_Mode;
  }
  else if(seatNum==1)
  {
    R_Mode=GetNextMode(R_Mode);
    logI("Seat #1", R_Mode);
    seatR.mode=R_Mode;
  }
}

//Возвращает номер следующего режима.
byte GetNextMode(byte mode){
  int i=0;
  while(i<4){
    if(modeSeq[i]==mode){
      break;
    }
    i++;
  }
  i++;
  if(i>3) i=0;
  return modeSeq[i];
}

byte GetIndicator(byte seatNum){
  if(seatNum==0)
  {
    logI("Seat #0", L_Mode);
    return L_Mode;
  }
  else if(seatNum==1)
  {
    logI("Seat #1", R_Mode);
    return R_Mode;
  }
  else{
    return 0;
  }
}

//I2C commands
void cmdMode(const uint8_t* buf, uint8_t len) {
  Serial.print("cmdMode ");
  if (len < 1) { slave.respondByte(0x00); return; }
  uint8_t seat = 2;
  if(buf[0]==REG_L_MODE)
    seat=0;
  if(buf[0]==REG_R_MODE)
    seat=1;
  Serial.println(seat);
  if (seat > 1) { slave.respondByte(0x00); return; }
  ClickHardware(seat);
  uint8_t ind=GetIndicator(seat);
  uint8_t resp[2] = {1, ind};
  slave.respond(resp, sizeof(resp));
}

void cmdGetStatus(const uint8_t* buf, uint8_t len) {
  Serial.print("cmdGetStatus ");
  if (len < 1) { slave.respondByte(0x00); return; }
  uint8_t seat = 2;
  if(buf[0]==REG_L_GetStatus)
    seat=0;
  if(buf[0]==REG_R_GetStatus)
    seat=1;
  Serial.println(seat);
  if (seat > 1) { slave.respondByte(0x00); return; }
  uint8_t ind=GetIndicator(seat);
  uint8_t resp[2] = {1, ind};
  slave.respond(resp, sizeof(resp));
}

void cmdPing(const uint8_t*, uint8_t) {
  slave.respondByte(0x01);
}

void cmdGetErrorCount(const uint8_t*, uint8_t) {
  Serial.print("cmdGetErrorCount: ");
  uint8_t count = 0;
  for (uint8_t i = 0; i < errLen; i++)
      if (errors[i].times > 0) count++;
  uint8_t resp[2]={1, count};
  Serial.println(count);
  slave.respond(resp, sizeof(resp));
}

void cmdGetError(const uint8_t* buf, uint8_t len) {
  Serial.print("getError #");
  uint8_t index = (len >= 2) ? buf[1] : 0;
  Serial.println(index);
  uint8_t found = 0;
  for (uint8_t i = 0; i < errLen; i++) {
    if (errors[i].times == 0) continue;
    if (found++ == index) {
      Serial.print("Code: ");
      Serial.println(errors[i].code);
      uint8_t resp[7];
      resp[0] = 1;
      resp[1] = errors[i].code;
      memcpy(&resp[2], &errors[i].tfs, 4);
      resp[6] = errors[i].times;
      slave.respond(resp, 7);
      return;
    }
  }
  uint8_t resp[7] = {};
  slave.respond(resp, 7);
}

void cmdClearErrors(const uint8_t*, uint8_t) {
  Serial.println("cmdClearErrors");
  ClearAllErrors();
  slave.respondByte(0x01);
}

void logS(String str){
  if(!isDebug)
    return;
  Serial.println(str);
}

void logI(String str, int i){
  if(!isDebug)
    return;
  Serial.print(str);
  Serial.print(" : ");
  Serial.println(i);
}
