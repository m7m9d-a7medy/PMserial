/* SerialPM
 Arduino library for PM sensors with serial interface
  PMS1003 aka G1
  PMS3003 aka G2
  PMS5003 aka G5
  PMS7003 aka G7
  PMSA003 aka G10
*/

#include <PMserial.h>

/* Sensor data format:
  https://github.com/avaldebe/AQmon/blob/master/lua_modules/pms3003.lua

PMS2003, PMS3003:
  24 byte long messages via UART 9600 8N1 (3.3V TTL).
DATA(MSB,LSB): Message header (4 bytes), 2 pairs of bytes (MSB,LSB)
  -1(  1,  2): Begin message       (hex:424D, ASCII 'BM')
   0(  3,  4): Message body length (hex:0014, decimal 20)
DATA(MSB,LSB): Message body (28 bytes), 10 pairs of bytes (MSB,LSB)
   1(  5,  6): PM 1.0 [ug/m3] (TSI standard)
   2(  7,  8): PM 2.5 [ug/m3] (TSI standard)
   3(  9, 10): PM 10. [ug/m3] (TSI standard)
   4( 11, 12): PM 1.0 [ug/m3] (std. atmosphere)
   5( 13, 14): PM 2.5 [ug/m3] (std. atmosphere)
   6( 15, 16): PM 10. [ug/m3] (std. atmosphere)
   7( 17, 18): no idea.
   8( 19, 19): no idea.
   9( 21, 22): no idea.
  10( 23, 24): cksum=byte01+..+byte22
PMS1003, PMS5003, PMS7003:
  32 byte long messages via UART 9600 8N1 (3.3V TTL).
DATA(MSB,LSB): Message header (4 bytes), 2 pairs of bytes (MSB,LSB)
  -1(  1,  2): Begin message       (hex:424D, ASCII 'BM')
   0(  3,  4): Message body length (hex:001C, decimal 28)
DATA(MSB,LSB): Message body (28 bytes), 14 pairs of bytes (MSB,LSB)
   1(  5,  6): PM 1.0 [ug/m3] (TSI standard)
   2(  7,  8): PM 2.5 [ug/m3] (TSI standard)
   3(  9, 10): PM 10. [ug/m3] (TSI standard)
   4( 11, 12): PM 1.0 [ug/m3] (std. atmosphere)
   5( 13, 14): PM 2.5 [ug/m3] (std. atmosphere)
   6( 15, 16): PM 10. [ug/m3] (std. atmosphere)
   7( 17, 18): num. particles with diameter > 0.3 um in 100 cm3 of air
   8( 19, 19): num. particles with diameter > 0.5 um in 100 cm3 of air
   9( 21, 22): num. particles with diameter > 1.0 um in 100 cm3 of air
  10( 23, 24): num. particles with diameter > 2.5 um in 100 cm3 of air
  11( 25, 26): num. particles with diameter > 5.0 um in 100 cm3 of air
  12( 27, 28): num. particles with diameter > 10. um in 100 cm3 of air
  13( 29, 30): Reserved
  14( 31, 32): cksum=byte01+..+byte30
*/

#define buff2word(n) ((buffer[n]<<8)|buffer[n+1])
const uint8_t
  TSI_START =  4,             // PM [ug/m3] (TSI standard)
  ATM_START = 10,             // PM [ug/m3] (std. atmosphere)
  NUM_START = 16;             // num. particles in 100 cm3 of air

const uint8_t
  msgLen = 7,
//slp[msgLen] = {0x42,0x4D,0xE4,0x00,0x00,0x01,0x73}, // sleep
//wak[msgLen] = {0x42,0x4D,0xE4,0x00,0x01,0x01,0x74}, // wake
//cfg[msgLen] = {0x42,0x4D,0xE1,0x00,0x01,0x01,0x71}, // set active mode
  cfg[msgLen] = {0x42,0x4D,0xE1,0x00,0x00,0x01,0x70}, // set passive mode
  trg[msgLen] = {0x42,0x4D,0xE2,0x00,0x00,0x01,0x71}; // passive mode read

void SerialPM::init(){
  if(hwSerial) {
#ifdef HAS_HW_SERIAL
    static_cast<HardwareSerial*>(uart)->begin(9600,SERIAL_8N1);
#endif
  } else {
#ifdef HAS_SW_SERIAL
    static_cast<SoftwareSerial*>(uart)->begin(9600);
#endif
  }
  switch (pms) {
  case PLANTOWER_24B:
    has_num=false;
    break;
  default: // PLANTOWER_32B
    has_num=true;
  }
  uart->write(cfg, msgLen);  // set passive mode
  uart->flush();
}

SerialPM::STATUS SerialPM::trigRead(){
  while (uart->available()) {
    uart->read();           // empty the RX buffer
  }
  uart->write(trg, msgLen); // passive mode read
  uart->flush();

  uint16_t start_ms = millis();
  do {                      // ~650ms to complete a measurements
    delay(10);              // wait up to 800ms
  } while (!uart->available() && millis()-start_ms<800);

  // we should an asnwer/message after 650ms
  if (!uart->available())
    return ERROR_TIMEOUT;

  // read message header
  const size_t headLen = 4;       // message header lenght
  if (uart->readBytes(&buffer[0], headLen) != headLen)
    return ERROR_MSG_HEADER;

  // message header starts with 'BM'
  if (buff2word(0)!=0x424D)
    return ERROR_MSG_START;

  // full message should fit in the buffer
  size_t bodyLen = buff2word(2);  // message body lenght
  if (headLen+bodyLen>BUFFER_LEN)
    return ERROR_MSG_LENGHT;
  
  // read message body
  if (uart->readBytes(&buffer[headLen], bodyLen) != bodyLen)
    return ERROR_MSG_BODY;

  if (!checkBuffer(headLen+bodyLen))
    return ERROR_MSG_CKSUM;

  return OK;
}

bool SerialPM::checkBuffer(size_t bufferLen){
  uint16_t cksum=buff2word(bufferLen-2);
  for (uint8_t n=0; n<bufferLen-2; n++){
    cksum-=buffer[n];
  }

  return (cksum==0);
}

void SerialPM::decodeBuffer(bool tsi_mode, bool truncated_num){
  uint8_t bin, n;
  for (bin=0, n=tsi_mode?TSI_START:ATM_START; bin<3; bin++, n+=2){
    pm[bin] = buff2word(n);
  }

  if (!has_num)
    return;
  for (bin=0, n=NUM_START; bin<6; bin++, n+=2){
    nc[bin] = buff2word(n); // number particles w/diameter > r_bin
  }

  if (!truncated_num)
    return;
  for (bin=5; bin>0; bin--){
    nc[bin] -= nc[bin-1];   // de-accumulate number concentrations
  }
}

SerialPM::STATUS SerialPM::read(bool tsi_mode, bool truncated_num){
  STATUS status = trigRead();  // read comand on passive mode
  if (status != OK) // decode message only if buffer checks out
    return status;
  decodeBuffer();
  return OK;
}
