#include <Arduino.h>

const uint8_t MEM_LED_PINS[4]={3,5,6,9};
const uint8_t IND_MSB_PIN=10;
const uint8_t IND_LSB_PIN=11;
const uint8_t ENC_CLK_PIN=2;
const uint8_t ENC_DT_PIN=4;
const uint8_t BTN1_PIN=7;
const uint8_t BTN2_PIN=8;

const uint8_t BRIGHTNESS_ONE=128;
const uint8_t BRIGHTNESS_ACTIVE=255;

uint16_t memoryCell=0b0001001001001000;
uint8_t currentGroup=0;
uint8_t cursorPos=0;
bool activeVisible=false;

int lastEncoderCLK=HIGH;
unsigned long lastEncoderStepMs=0;

bool comboSession=false;
bool comboTriggered=false;
unsigned long comboStartMs=0;

struct DebouncedButton{
 uint8_t pin=0;
 bool rawPressed=false;
 bool stablePressed=false;
 bool pressedEdge=false;
 bool releasedEdge=false;
 unsigned long rawChangedAt=0;

 void begin(uint8_t p){
   pin=p;
  pinMode(pin,INPUT_PULLUP);
  rawPressed=(digitalRead(pin)==LOW);
   stablePressed=rawPressed;
  rawChangedAt=millis();
 }

 void update(unsigned long now){
  pressedEdge=false;
   releasedEdge=false;

  bool r=(digitalRead(pin)==LOW);
   if(r!=rawPressed){
    rawPressed=r;
    rawChangedAt=now;
   }

  if(rawPressed!=stablePressed&&(now-rawChangedAt)>=25){
   stablePressed=rawPressed;
    if(stablePressed) pressedEdge=true;
   else releasedEdge=true;
  }
 }
};

DebouncedButton button1;
DebouncedButton button2;

uint8_t bitNumberForPos(uint8_t pos){
 return currentGroup*4+(3-pos);
}

void renderIndicators(){
 digitalWrite(IND_MSB_PIN,(currentGroup&0b10)?HIGH:LOW);
 digitalWrite(IND_LSB_PIN,(currentGroup&0b01)?HIGH:LOW);
}

void renderMemoryLeds(){
 for(uint8_t pos=0;pos<4;++pos){
  uint8_t bitNo=bitNumberForPos(pos);

   if(activeVisible&&pos==cursorPos){
    analogWrite(MEM_LED_PINS[pos],BRIGHTNESS_ACTIVE);
   }else{
    bool value=(memoryCell>>bitNo)&1U;
     analogWrite(MEM_LED_PINS[pos],value?BRIGHTNESS_ONE:0);
   }
 }
}

void renderAll(){
 renderIndicators();
  renderMemoryLeds();
}

void printBinary16(uint16_t value){
 for(int8_t b=15;b>=0;--b){
  Serial.print((value>>b)&1U);
 }
}

void printGroupCode(){
 Serial.print((currentGroup>>1)&1U);
 Serial.print(currentGroup&1U);
}

void printVisibleNibble(){
 for(uint8_t pos=0;pos<4;++pos){
  uint8_t bitNo=bitNumberForPos(pos);
  Serial.print((memoryCell>>bitNo)&1U);
 }
}

void printState(const char *reason){
 Serial.println();
 Serial.print(F("===-"));
 Serial.print(reason);
 Serial.println(F("-==="));

 Serial.print(F("GRUPPA-"));
 printGroupCode();
 Serial.print(F("-BITY-"));
 Serial.print(currentGroup*4);
 Serial.print(F("-"));
 Serial.println(currentGroup*4+3);

 Serial.print(F("VIDIMYE-BITY-"));
 printVisibleNibble();
 Serial.println();

 Serial.print(F("DVOICHNOE-"));
 printBinary16(memoryCell);
 Serial.println();

 Serial.print(F("DESYATICHNOE-"));
 Serial.println((unsigned int)memoryCell);
}

void startupLedTest(){
 for(uint8_t i=0;i<4;++i){
   analogWrite(MEM_LED_PINS[i],BRIGHTNESS_ACTIVE);
  delay(500);
 }

 for(uint8_t i=0;i<4;++i){
  analogWrite(MEM_LED_PINS[i],0);
 }
}

void moveCursor(int8_t direction){
 int8_t next=(int8_t)cursorPos+direction;
 if(next<0) next=3;
 if(next>3) next=0;
 cursorPos=(uint8_t)next;
  activeVisible=true;

 renderMemoryLeds();

 Serial.print(F("AKTIVNYI-BIT-"));
 Serial.print(bitNumberForPos(cursorPos));
 Serial.print(F("-POZICIYA-"));
 Serial.print(cursorPos+1);
 Serial.println(F("-IZ-4"));
}

void handleEncoder(){
 int clk=digitalRead(ENC_CLK_PIN);

 if(lastEncoderCLK==HIGH&&clk==LOW){
  unsigned long now=millis();
   if(now-lastEncoderStepMs>=3){
    int8_t direction=(digitalRead(ENC_DT_PIN)==HIGH)?+1:-1;
    moveCursor(direction);
     lastEncoderStepMs=now;
   }
 }

 lastEncoderCLK=clk;
}

void nextGroup(){
 currentGroup=(currentGroup+1)&0x03;
 activeVisible=false;
 renderAll();
  printState("KNOPKA-1-SLEDUYUSHAYA-GRUPPA");
}

void toggleActiveBit(){
 if(!activeVisible){
  Serial.println(F("KNOPKA-2-NE-SRABOTALA-POVERNITE-ENKODER"));
  return;
 }

 uint8_t bitNo=bitNumberForPos(cursorPos);
 memoryCell^=(uint16_t)(1U<<bitNo);
 activeVisible=false;
 renderMemoryLeds();
 printState("KNOPKA-2-BIT-IZMENEN");
}

void invertAllBits(){
 memoryCell^=0xFFFFU;
 activeVisible=false;
 renderAll();
 printState("OBE-KNOPKI-2-SEK-PAMYAT-INVERTIROVANA");
}

void handleButtons(unsigned long now){
 button1.update(now);
 button2.update(now);

 if(button1.stablePressed&&button2.stablePressed){
  if(!comboSession){
   comboSession=true;
    comboTriggered=false;
   comboStartMs=now;
  }

  if(!comboTriggered&&(now-comboStartMs>=2000UL)){
   invertAllBits();
   comboTriggered=true;
  }
  return;
 }

 if(comboSession){
  if(!button1.stablePressed&&!button2.stablePressed){
   comboSession=false;
   comboTriggered=false;
  }
  return;
 }

 if(button1.releasedEdge){
  nextGroup();
 }

 if(button2.releasedEdge){
   toggleActiveBit();
 }
}

void setup(){
 Serial.begin(115200);

 for(uint8_t i=0;i<4;++i){
  pinMode(MEM_LED_PINS[i],OUTPUT);
   analogWrite(MEM_LED_PINS[i],0);
 }

 pinMode(IND_MSB_PIN,OUTPUT);
 pinMode(IND_LSB_PIN,OUTPUT);
 digitalWrite(IND_MSB_PIN,LOW);
 digitalWrite(IND_LSB_PIN,LOW);

 pinMode(ENC_CLK_PIN,INPUT_PULLUP);
 pinMode(ENC_DT_PIN,INPUT_PULLUP);

 button1.begin(BTN1_PIN);
 button2.begin(BTN2_PIN);

 Serial.println(F("KONTROLLER-16-BITNOI-PAMYATI"));
 Serial.println(F("PERVICHNYI-TEST-SVETODIODOV"));
 startupLedTest();

 currentGroup=0;
 activeVisible=false;
 renderAll();

 lastEncoderCLK=digitalRead(ENC_CLK_PIN);
 printState("GOTOVO");
}

void loop(){
 unsigned long now=millis();

 handleButtons(now);
  handleEncoder();
}
