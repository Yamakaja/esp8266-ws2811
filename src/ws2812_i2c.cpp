/******************************************************************************
 * Copyright 2013-2015 Espressif Systems
 *           2015 <>< Charles Lohr
 *           2018 Yamakaja
 *
 * FileName: ws2811_i2c.c
 *
 * Description: I2S output routines for a FreeRTOS system. Uses DMA and a queue
 * to abstract away the nitty-gritty details.
 *
 * Modification history:
 *     2015/06/01, v1.0 File created.
 *     2015/07/23, Switch to making it a WS2812 output device.
*******************************************************************************
Notes:
 This is pretty badly hacked together from the MP3 example.
 I spent some time trying to strip it down to avoid a lot of the TX_ stuff. 
 That seems to work.
 Major suggestions that I couldn't figure out:
    * Use interrupts to disable DMA, so it isn't running nonstop.
    * Use interrupts to flag when new data can be sent.
 When I try using interrupts, it seems to work for a bit but things fall apart
 rather quickly and the engine just refuses to send anymore until reboot.
 The way it works right now is to keep the DMA running forever and just update
 the data in the buffer so it continues sending the frame.
Extra copyright info:
  Actually not much of this file is Copyright Espressif, comparativly little
  mostly just the stuff to make the I2S bus go.
*******************************************************************************/

extern "C" void rom_i2c_writeReg_Mask(int, int, int, int, int, int);

#include "driver/slc_register.h"
#include "i2s_reg.h"
#include "pin_mux_register.h"
#include "user_interface.h"
#include "ws2812_i2c.h"
#include <c_types.h>

// Creates an I2S SR of 93,750 Hz, or 3 MHz Bitclock (.333us/sample)
// 1600000000L/(div*bestbck)
// It is likely you could speed this up a little.

#define WS_I2S_BCK 21 // Seems to work as low as 18, but is shakey at 1.
#define WS_I2S_DIV 3

#ifndef i2c_bbpll
#define i2c_bbpll 0x67
#define i2c_bbpll_en_audio_clock_out 4
#define i2c_bbpll_en_audio_clock_out_msb 7
#define i2c_bbpll_en_audio_clock_out_lsb 7
#define i2c_bbpll_hostid 4

#define i2c_writeReg_Mask(block, host_id, reg_add, Msb, Lsb, indata)           \
  rom_i2c_writeReg_Mask(block, host_id, reg_add, Msb, Lsb, indata)
#define i2c_readReg_Mask(block, host_id, reg_add, Msb, Lsb)                    \
  rom_i2c_readReg_Mask(block, host_id, reg_add, Msb, Lsb)
#define i2c_writeReg_Mask_def(block, reg_add, indata)                          \
  i2c_writeReg_Mask(block, block##_hostid, reg_add, reg_add##_msb,             \
                    reg_add##_lsb, indata)
#define i2c_readReg_Mask_def(block, reg_add)                                   \
  i2c_readReg_Mask(block, block##_hostid, reg_add, reg_add##_msb, reg_add##_lsb)
#endif
#ifndef ETS_SLC_INUM
#define ETS_SLC_INUM 1
#endif


//From sdio_slv.h
struct sdio_queue {
  uint32 blocksize : 12;
  uint32 datalen : 12;
  uint32 unused : 5;
  uint32 sub_sof : 1;
  uint32 eof : 1;
  uint32 owner : 1;

  uint32 buf_ptr;
  uint32 next_link_ptr;
};

// Rest of program...

// I2S DMA buffer descriptors
static struct sdio_queue i2sBufDescOut;
static struct sdio_queue i2sBufDescZeroes;

static uint8_t i2sZeroes[128];
static uint8_t i2sBlock[WS_BLOCKSIZE];

//Initialize I2S subsystem for DMA circular buffer use
void ICACHE_FLASH_ATTR ws2812_init() {
    // Reset DMA
    SET_PERI_REG_MASK(SLC_CONF0, SLC_RXLINK_RST);
    CLEAR_PERI_REG_MASK(SLC_CONF0, SLC_RXLINK_RST);
  
    // Clear DMA int flags
    SET_PERI_REG_MASK(SLC_INT_CLR, 0xffffffff);
    CLEAR_PERI_REG_MASK(SLC_INT_CLR, 0xffffffff);
  
    // Enable and configure DMA
    CLEAR_PERI_REG_MASK(SLC_CONF0, (SLC_MODE << SLC_MODE_S));
    SET_PERI_REG_MASK(SLC_CONF0, (1 << SLC_MODE_S));
    SET_PERI_REG_MASK(SLC_RX_DSCR_CONF, SLC_INFOR_NO_REPLACE | SLC_TOKEN_NO_REPLACE);
    CLEAR_PERI_REG_MASK(SLC_RX_DSCR_CONF, SLC_RX_FILL_EN | SLC_RX_EOF_MODE | SLC_RX_FILL_MODE);
  
    i2sBufDescOut.owner = 1; // Software owned
    i2sBufDescOut.eof = 1;
    i2sBufDescOut.sub_sof = 0;
    i2sBufDescOut.datalen = WS_BLOCKSIZE;   // Size (in bytes)
    i2sBufDescOut.blocksize = WS_BLOCKSIZE; // Size (in bytes)
    i2sBufDescOut.buf_ptr = (uint32_t) i2sBlock;
    i2sBufDescOut.unused = 0;
    // At the end, just redirect the DMA to the zero buffer.
    i2sBufDescOut.next_link_ptr = (uint32_t) &i2sBufDescZeroes;

    i2sBufDescZeroes.owner = 1;
    i2sBufDescZeroes.eof = 1;
    i2sBufDescZeroes.sub_sof = 0;
    i2sBufDescZeroes.datalen = 32;
    i2sBufDescZeroes.blocksize = 32;
    i2sBufDescZeroes.buf_ptr = (uint32_t)i2sZeroes;
    i2sBufDescZeroes.unused = 0;
    i2sBufDescZeroes.next_link_ptr = (uint32_t) &i2sBufDescOut;
  
    for (int x = 0; x < sizeof(i2sZeroes); x++)
      i2sZeroes[x] = 0x00;
  
    for (int x = 0; x < WS_BLOCKSIZE; x++)
      i2sBlock[x] = 0x00;
  
    CLEAR_PERI_REG_MASK(SLC_RX_LINK, SLC_RXLINK_DESCADDR_MASK);
    SET_PERI_REG_MASK(SLC_RX_LINK, ((uint32_t)&i2sBufDescOut) & SLC_RXLINK_DESCADDR_MASK);
  
    // Start transmission
    SET_PERI_REG_MASK(SLC_RX_LINK, SLC_RXLINK_START);
  
    //----
  
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_I2SO_DATA);
  
    // Enable clock to i2s subsystem
    i2c_writeReg_Mask_def(i2c_bbpll, i2c_bbpll_en_audio_clock_out, 1);
  
    // Reset I2S subsystem
    CLEAR_PERI_REG_MASK(I2SCONF, I2S_I2S_RESET_MASK);
    SET_PERI_REG_MASK(I2SCONF, I2S_I2S_RESET_MASK);
    CLEAR_PERI_REG_MASK(I2SCONF, I2S_I2S_RESET_MASK);
  
    // Select 16 bits per channel (FIFO_MOD=0), no DMA access (FIFO only)
    CLEAR_PERI_REG_MASK(I2S_FIFO_CONF, I2S_I2S_DSCR_EN |
                            (I2S_I2S_RX_FIFO_MOD << I2S_I2S_RX_FIFO_MOD_S) |
                            (I2S_I2S_TX_FIFO_MOD << I2S_I2S_TX_FIFO_MOD_S));

    // Enable DMA in i2s subsystem
    SET_PERI_REG_MASK(I2S_FIFO_CONF, I2S_I2S_DSCR_EN);
  
    // tx/rx binaureal
    // trans master&rece slave,MSB shift,right_first,msb right
  
    CLEAR_PERI_REG_MASK(I2SCONF, I2S_TRANS_SLAVE_MOD |
                                     (I2S_BITS_MOD << I2S_BITS_MOD_S) |
                                     (I2S_BCK_DIV_NUM << I2S_BCK_DIV_NUM_S) |
                                     (I2S_CLKM_DIV_NUM << I2S_CLKM_DIV_NUM_S));
    SET_PERI_REG_MASK(
        I2SCONF, I2S_RIGHT_FIRST | I2S_MSB_RIGHT | I2S_RECE_SLAVE_MOD |
                     I2S_RECE_MSB_SHIFT | I2S_TRANS_MSB_SHIFT |
                     (((WS_I2S_BCK)&I2S_BCK_DIV_NUM) << I2S_BCK_DIV_NUM_S) |
                     (((WS_I2S_DIV)&I2S_CLKM_DIV_NUM) << I2S_CLKM_DIV_NUM_S));
  
    // No idea if ints are needed...
    // clear int
    SET_PERI_REG_MASK(I2SINT_CLR, I2S_I2S_RX_WFULL_INT_CLR |
                                      I2S_I2S_PUT_DATA_INT_CLR |
                                      I2S_I2S_TAKE_DATA_INT_CLR);
    CLEAR_PERI_REG_MASK(I2SINT_CLR, I2S_I2S_RX_WFULL_INT_CLR |
                                        I2S_I2S_PUT_DATA_INT_CLR |
                                        I2S_I2S_TAKE_DATA_INT_CLR);
    // enable int
    SET_PERI_REG_MASK(I2SINT_ENA, I2S_I2S_RX_REMPTY_INT_ENA | I2S_I2S_RX_TAKE_DATA_INT_ENA);
  
    // Start transmission
    SET_PERI_REG_MASK(I2SCONF, I2S_I2S_TX_START);
}


//All functions below this line are Public Domain 2015 Charles Lohr.
//this code may be used by anyone in any way without restriction or limitation.

static const uint16_t bitpatterns[16] = {
    0b100100100100, 0b100100100110, 0b100100110100, 0b100100110110,
    0b100110100100, 0b100110100110, 0b100110110100, 0b100110110110,
    0b110100100100, 0b110100100110, 0b110100110100, 0b110100110110,
    0b110110100100, 0b110110100110, 0b110110110100, 0b110110110110,
};

void ICACHE_FLASH_ATTR ws2812_push( uint8_t *buffer, uint16_t buffersize ) {
    uint8_t *target = i2sBlock;

    for (int i = 0; i < buffersize - 3;) {
        uint8_t temp;

        temp = buffer[i++]; uint16_t c1a = bitpatterns[temp >> 4]; uint16_t c1b = bitpatterns[temp & 0xf];
        temp = buffer[i++]; uint16_t c2a = bitpatterns[temp >> 4]; uint16_t c2b = bitpatterns[temp & 0xf];
        temp = buffer[i++]; uint16_t c3a = bitpatterns[temp >> 4]; uint16_t c3b = bitpatterns[temp & 0xf];
        temp = buffer[i++]; uint16_t c4a = bitpatterns[temp >> 4]; uint16_t c4b = bitpatterns[temp & 0xf];

#define STEP1(x) (c##x##a >> 4)
#define STEP2(x) (c##x##a << 4 | (c##x##b >> 8))
#define STEP3(x) (c##x##b)

        *(target++) = STEP1(2);
        *(target++) = STEP3(1);
        *(target++) = STEP2(1);
        *(target++) = STEP1(1);

        *(target++) = STEP2(3);
        *(target++) = STEP1(3);
        *(target++) = STEP3(2);
        *(target++) = STEP2(2);

        *(target++) = STEP3(4);
        *(target++) = STEP2(4);
        *(target++) = STEP1(4);
        *(target++) = STEP3(3);
    }

}
