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
#include <c_types.h>
#include "ws2812_i2c.h"
#include "user_interface.h"
#include "pin_mux_register.h"
#include "i2s_reg.h"

//Creates an I2S SR of 93,750 Hz, or 3 MHz Bitclock (.333us/sample)
// 1600000000L/(div*bestbck)
//It is likely you could speed this up a little.

#define WS_I2S_BCK 21  //Seems to work as low as 18, but is shakey at 1.
#define WS_I2S_DIV 3


#ifndef i2c_bbpll
#define i2c_bbpll                                 0x67
#define i2c_bbpll_en_audio_clock_out            4
#define i2c_bbpll_en_audio_clock_out_msb        7
#define i2c_bbpll_en_audio_clock_out_lsb        7
#define i2c_bbpll_hostid                           4

#define i2c_writeReg_Mask(block, host_id, reg_add, Msb, Lsb, indata)  rom_i2c_writeReg_Mask(block, host_id, reg_add, Msb, Lsb, indata)
#define i2c_readReg_Mask(block, host_id, reg_add, Msb, Lsb)  rom_i2c_readReg_Mask(block, host_id, reg_add, Msb, Lsb)
#define i2c_writeReg_Mask_def(block, reg_add, indata) \
      i2c_writeReg_Mask(block, block##_hostid,  reg_add,  reg_add##_msb,  reg_add##_lsb,  indata)
#define i2c_readReg_Mask_def(block, reg_add) \
      i2c_readReg_Mask(block, block##_hostid,  reg_add,  reg_add##_msb,  reg_add##_lsb)
#endif
#ifndef ETS_SLC_INUM
#define ETS_SLC_INUM       1
#endif

//From sdio_slv.h


struct sdio_queue {
    uint32 blocksize:12;
    uint32 datalen:12;
    uint32 unused:5;
    uint32 sub_sof:1;
    uint32 eof:1;
    uint32 owner:1;

    uint32 buf_ptr;
    uint32 next_link_ptr;
};

struct sdio_slave_status_element {
    uint32 wr_busy:1;
    uint32 rd_empty :1;
    uint32 comm_cnt :3;
    uint32 intr_no :3;
    uint32 rx_length:16;
    uint32 res:8;
};

union sdio_slave_status
{
    struct sdio_slave_status_element elm_value;
    uint32 word_value;
};

#define RX_AVAILIBLE 2
#define TX_AVAILIBLE 1
#define INIT_STAGE     0

#define SDIO_QUEUE_LEN 8
#define MOSI  0
#define MISO  1
#define SDIO_DATA_ERROR 6

#define SLC_INTEREST_EVENT (SLC_TX_EOF_INT_ENA | SLC_RX_EOF_INT_ENA | SLC_RX_UDF_INT_ENA | SLC_TX_DSCR_ERR_INT_ENA)
#define TRIG_TOHOST_INT()    SET_PERI_REG_MASK(SLC_INTVEC_TOHOST , BIT0);\
                            CLEAR_PERI_REG_MASK(SLC_INTVEC_TOHOST , BIT0)


///Rest of program...

//Pointer to the I2S DMA buffer data
//static unsigned int i2sBuf[I2SDMABUFCNT][I2SDMABUFLEN];
//I2S DMA buffer descriptors
//static struct sdio_queue i2sBufDesc[I2SDMABUFCNT];
static struct sdio_queue i2sBufDescOut;
static struct sdio_queue i2sBufDescZeroes;

static unsigned int i2sZeroes[32];
static unsigned int i2sBlock[WS_BLOCKSIZE/4];

//Initialize I2S subsystem for DMA circular buffer use
void ICACHE_FLASH_ATTR ws2812_init() {
    int x, y;
    
    //Reset DMA
    SET_PERI_REG_MASK(SLC_CONF0, SLC_RXLINK_RST);//|SLC_TXLINK_RST);
    CLEAR_PERI_REG_MASK(SLC_CONF0, SLC_RXLINK_RST);//|SLC_TXLINK_RST);

    //Clear DMA int flags
    SET_PERI_REG_MASK(SLC_INT_CLR,  0xffffffff);
    CLEAR_PERI_REG_MASK(SLC_INT_CLR,  0xffffffff);

    //Enable and configure DMA
    CLEAR_PERI_REG_MASK(SLC_CONF0, (SLC_MODE<<SLC_MODE_S));
    SET_PERI_REG_MASK(SLC_CONF0,(1<<SLC_MODE_S));
    SET_PERI_REG_MASK(SLC_RX_DSCR_CONF,SLC_INFOR_NO_REPLACE|SLC_TOKEN_NO_REPLACE);
    CLEAR_PERI_REG_MASK(SLC_RX_DSCR_CONF, SLC_RX_FILL_EN|SLC_RX_EOF_MODE | SLC_RX_FILL_MODE);

    i2sBufDescOut.owner = 1;
    i2sBufDescOut.eof = 1;
    i2sBufDescOut.sub_sof = 0;
    i2sBufDescOut.datalen = WS_BLOCKSIZE;  //Size (in bytes)
    i2sBufDescOut.blocksize = WS_BLOCKSIZE; //Size (in bytes)
    i2sBufDescOut.buf_ptr=(uint32_t)&i2sBlock[0];
    i2sBufDescOut.unused=0;
    i2sBufDescOut.next_link_ptr=(uint32_t)&i2sBufDescZeroes; //At the end, just redirect the DMA to the zero buffer.

    i2sBufDescZeroes.owner = 1;
    i2sBufDescZeroes.eof = 1;
    i2sBufDescZeroes.sub_sof = 0;
    i2sBufDescZeroes.datalen = 32;
    i2sBufDescZeroes.blocksize = 32;
    i2sBufDescZeroes.buf_ptr=(uint32_t)&i2sZeroes[0];
    i2sBufDescZeroes.unused=0;
    i2sBufDescZeroes.next_link_ptr=(uint32_t)&i2sBufDescOut;


    for( x = 0; x < 32; x++ )
        i2sZeroes[x] = 0x00;
    
    for( x = 0; x < WS_BLOCKSIZE/4; x++ )
        i2sBlock[x] = 0x00000000;

    CLEAR_PERI_REG_MASK(SLC_RX_LINK,SLC_RXLINK_DESCADDR_MASK);
    SET_PERI_REG_MASK(SLC_RX_LINK, ((uint32)&i2sBufDescOut) & SLC_RXLINK_DESCADDR_MASK);

    //Start transmission
    SET_PERI_REG_MASK(SLC_RX_LINK, SLC_RXLINK_START);

//----

    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_I2SO_DATA);

    //Enable clock to i2s subsystem
    i2c_writeReg_Mask_def(i2c_bbpll, i2c_bbpll_en_audio_clock_out, 1);

    //Reset I2S subsystem
    CLEAR_PERI_REG_MASK(I2SCONF,I2S_I2S_RESET_MASK);
    SET_PERI_REG_MASK(I2SCONF,I2S_I2S_RESET_MASK);
    CLEAR_PERI_REG_MASK(I2SCONF,I2S_I2S_RESET_MASK);

    //Select 16bits per channel (FIFO_MOD=0), no DMA access (FIFO only)
    CLEAR_PERI_REG_MASK(I2S_FIFO_CONF, I2S_I2S_DSCR_EN|(I2S_I2S_RX_FIFO_MOD<<I2S_I2S_RX_FIFO_MOD_S)|(I2S_I2S_TX_FIFO_MOD<<I2S_I2S_TX_FIFO_MOD_S));
    //Enable DMA in i2s subsystem
    SET_PERI_REG_MASK(I2S_FIFO_CONF, I2S_I2S_DSCR_EN);

    //tx/rx binaureal
    //trans master&rece slave,MSB shift,right_first,msb right

    CLEAR_PERI_REG_MASK(I2SCONF, I2S_TRANS_SLAVE_MOD|
                        (I2S_BITS_MOD<<I2S_BITS_MOD_S)|
                        (I2S_BCK_DIV_NUM <<I2S_BCK_DIV_NUM_S)|
                        (I2S_CLKM_DIV_NUM<<I2S_CLKM_DIV_NUM_S));
    SET_PERI_REG_MASK(I2SCONF, I2S_RIGHT_FIRST|I2S_MSB_RIGHT|I2S_RECE_SLAVE_MOD|
                        I2S_RECE_MSB_SHIFT|I2S_TRANS_MSB_SHIFT|
                        (((WS_I2S_BCK)&I2S_BCK_DIV_NUM )<<I2S_BCK_DIV_NUM_S)|
                        (((WS_I2S_DIV)&I2S_CLKM_DIV_NUM)<<I2S_CLKM_DIV_NUM_S));


    //No idea if ints are needed...
    //clear int
    SET_PERI_REG_MASK(I2SINT_CLR, I2S_I2S_RX_WFULL_INT_CLR|I2S_I2S_PUT_DATA_INT_CLR|I2S_I2S_TAKE_DATA_INT_CLR);
    CLEAR_PERI_REG_MASK(I2SINT_CLR, I2S_I2S_RX_WFULL_INT_CLR|I2S_I2S_PUT_DATA_INT_CLR|I2S_I2S_TAKE_DATA_INT_CLR);
    //enable int
    SET_PERI_REG_MASK(I2SINT_ENA,  I2S_I2S_RX_REMPTY_INT_ENA|I2S_I2S_RX_TAKE_DATA_INT_ENA);


    //Start transmission
    SET_PERI_REG_MASK(I2SCONF,I2S_I2S_TX_START);
}


//All functions below this line are Public Domain 2015 Charles Lohr.
//this code may be used by anyone in any way without restriction or limitation.

static const uint16_t bitpatterns[16] = {
    0b100100100100, 0b100100100110, 0b100100110100, 0b100100110110,
    0b100110100100, 0b100110100110, 0b100110110100, 0b100110110110,
    0b110100100100, 0b110100100110, 0b110100110100, 0b110100110110,
    0b110110100100, 0b110110100110, 0b110110110100, 0b110110110110,
};

void ICACHE_FLASH_ATTR ws2812_push( uint8_t * buffer, uint16_t buffersize ) {
    uint16_t place;
    uint8_t * bufferpl = (uint8_t*)&i2sBlock[0];

    int pl = 0;
    int quit = 0;

    //Once for each led.
    for( place = 0; !quit; place++ ) {
        uint8_t b;
        b = buffer[pl++]; uint16_t c1a = bitpatterns[(b&0x0f)]; uint16_t c1b = bitpatterns[(b>>4)];
        b = buffer[pl++]; uint16_t c2a = bitpatterns[(b&0x0f)]; uint16_t c2b = bitpatterns[(b>>4)];
        b = buffer[pl++]; uint16_t c3a = bitpatterns[(b&0x0f)]; uint16_t c3b = bitpatterns[(b>>4)];
        b = buffer[pl++]; uint16_t c4a = bitpatterns[(b&0x0f)]; uint16_t c4b = bitpatterns[(b>>4)];

        if( pl >= buffersize ) {
            quit = 1;
            if( pl-1 >= buffersize ) c4a = c4b = 0;
            if( pl-2 >= buffersize ) c3a = c3b = 0;
            if( pl-3 >= buffersize ) c2a = c2b = 0;
            if( pl-4 >= buffersize ) c1a = c1b = 0;
        }

        //Order of bits on wire: Reverse from how they appear here.
#define STEP1(x) (c##x##b >> 4 )
#define STEP2(x) ((c##x##b << 4 ) | ( c##x##a>>8 ))
#define STEP3(x) (c##x##a & 0xff )

        *(bufferpl++) = STEP1(2);
        *(bufferpl++) = STEP3(1);
        *(bufferpl++) = STEP2(1);
        *(bufferpl++) = STEP1(1);

        *(bufferpl++) = STEP2(3);
        *(bufferpl++) = STEP1(3);
        *(bufferpl++) = STEP3(2);
        *(bufferpl++) = STEP2(2);

        *(bufferpl++) = STEP3(4);
        *(bufferpl++) = STEP2(4);
        *(bufferpl++) = STEP1(4);
        *(bufferpl++) = STEP3(3);
    }

    while( bufferpl < &((uint8_t*)i2sBlock)[WS_BLOCKSIZE] ) *(bufferpl++) = 0;

}
