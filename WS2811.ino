#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include "credentials.h"

volatile extern uint32_t PIN_OUT;

#define LED_COUNT     (19)
#define BYTE_COUNT    (LED_COUNT * 3)
#define PIN           (5)

#define NOP_10        "nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;"
#define NOP_100       NOP_10 NOP_10 NOP_10 NOP_10 NOP_10 NOP_10 NOP_10 NOP_10 NOP_10 NOP_10
#define NOP_1000      NOP_100 NOP_100 NOP_100 NOP_100 NOP_100 NOP_100 NOP_100 NOP_100 NOP_100 NOP_100

void setColorRGB(uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
void render();

uint8_t* rgb_arr = NULL;
uint32_t resetTimeout;

const char* ssid = SSID;
const char* password = PSK;

WiFiUDP Udp;

void setup() {
  pinMode(5, OUTPUT);
  digitalWrite(PIN, LOW);

  if ((rgb_arr = (uint8_t *) malloc(BYTE_COUNT))) {
    memset(rgb_arr, 0, BYTE_COUNT);
  }
  
  render();

  #ifdef debug
  Serial.begin(115200);
  Serial.print("\nConnecting ");
  #endif
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    #ifdef debug
    Serial.print(".");
    #endif
  }

  #ifdef debug
  Serial.println(" done!");
  Serial.printf("Connected to %s with %s!\n", ssid, WiFi.localIP().toString().c_str());
  #endif
  Udp.begin(1234);
}

uint8_t *buff = (uint8_t*) malloc(4);

uint8_t value = 0;

void loop() {
  int packetSize = Udp.parsePacket();
  if (packetSize) {
    while (Udp.available() > 0) {
      
      #ifdef debug
      Serial.printf("Available: %d\n", Udp.available());
      Serial.printf("peek() = %d\n", Udp.peek());
      #endif
      
      if ((uint8_t) Udp.peek() == (uint8_t)254) {
        
        #ifdef debug
        Serial.println("Rendering!");
        #endif
        
        Udp.read(buff, 1);
        render();
        continue;
      }

      Udp.read(buff, 4);
      uint16_t idx = (uint16_t)(*buff);
      uint8_t r = *(buff+1), g = *(buff+2), b = *(buff+3);
      setColorRGB(idx, r, g, b);

      #ifdef debug
      Serial.printf("Set %d to (%d, %d, %d)\n", idx, r, g, b);
      #endif
    }
  }
}

void setColorRGB(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
  if (idx < LED_COUNT) {
    uint8_t *p = &rgb_arr[idx * 3];
    *p++ = b;
    *p++ = r;
    *p = g;
  }
}

void render() {

  if (!rgb_arr)
    return;

  //while ((micros() - t_f) < 50L);

  cli(); // Disable interrupts

  volatile uint8_t
  *p              = rgb_arr;
  volatile uint32_t
   high           = PIN_OUT | _BV(PIN),
   low            = PIN_OUT & ~_BV(PIN);

  /**
     Timing:  HIGH LOW (In cycles)
     0 :      20   80
     1 :      48   52

     Frame:   100 cycles

     Reset:   4000 cycles

     
  */

  #define HIGH_INS "s8i %2, %0, 0; memw;"
  #define LOW_INS "s8i %1, %0, 0; memw;"

  asm volatile(
    "memw;"
    "movi a15, 1;"
    "mov  a12, %3;"
    
    HIGH_INS
    LOW_INS
    NOP_1000
    NOP_1000
    NOP_1000
    NOP_1000
    NOP_1000
    
    "nextbyte:"
    "l8ui a13, a12, 0;"
    "movi a14, 8;"
    
    "nextbit:"
    HIGH_INS              // Initial high
    "bbsi a13, 7, high;"

    NOP_10
    "nop; nop;"
    LOW_INS
    NOP_10 NOP_10 NOP_10
    "nop;"
    "j cont;"
    
    "high:"
    NOP_10
    NOP_10
    
    NOP_10
    "nop; nop; nop; nop; nop; nop; nop; nop;"
    LOW_INS
    
    "cont:"
    
    NOP_10 NOP_10 NOP_10 "nop; nop; nop; nop;"
    
    "sub a14, a14, a15;"
    "slli a13, a13, 1;"

    // Next bit?
    "bnez a14, nextbit;"
    // No

    "sub %4, %4, a15;"

    // Next byte?
    "beqz %4, done;"
    // Yes

    "addi a12, a12, 1;"
    "j nextbyte;"

     "done:"
    
    ::
    "r" ( &PIN_OUT ),     // %0
    "r" ( low ),          // %1
    "r" ( high ),         // %2
    "r" ( p ),            // %3
    "r" ( BYTE_COUNT )    // %4
    
    :
    "a12",                // Pointer
    "a13",                // Current value
    "a14",                // Bits left
    "a15"                 // One constant
  );

  #undef HIGH_INS
  #undef LOW_INS

  sei();                  // Re-enable interrupts
  //t_f = micros();
}

