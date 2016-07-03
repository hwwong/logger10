



void setDataBits(uint16_t bits) {
  const uint32_t mask = ~((SPIMMOSI << SPILMOSI) | (SPIMMISO << SPILMISO));
  bits--;
  SPI1U1 = ((SPI1U1 & mask) | ((bits << SPILMOSI) | (bits << SPILMISO)));
}


void max31855Setup() {

  //  //start and configure hardware SPI
  SPI.begin();
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE0);
  SPI.setFrequency(5000000);
  setDataBits(32);

}

void max3188_readAllRaw(uint32_t* arr) {

  for (uint8_t i = 0; i < 10; i++) {
    GPOC = TC[i];
    while (SPI1CMD & SPIBUSY) {}
    SPI1CMD |= SPIBUSY;     // SPI send dummy
    while (SPI1CMD & SPIBUSY) {}
    GPOS = CS_HIGH;
    arr[i] = SPI1W0;
  }

}


int16_t max31855_read(uint8_t ch) {

  GPOC = TC[ch];
//  delayMicroseconds(1);
  while (SPI1CMD & SPIBUSY) {}
  SPI1CMD |= SPIBUSY;     // SPI send dummy
  while (SPI1CMD & SPIBUSY) {}
  GPOS = CS_HIGH;

  //check error bit
  if (SPI1W0 & 0x100) {
    return 0xffffffff;    // return 0xffffffff. if error
  } else {
    // check netvigative value
    if (((byte*)&SPI1W0)[0] & 0x80)
      return (((((byte*)&SPI1W0)[0] | 0xffC0) << 6) ) | ((byte*)&SPI1W0)[1] >> 2 ;
    else
      return  ((byte*)&SPI1W0)[0] << 6 | ((byte*)&SPI1W0)[1] >> 2;
  }

}


