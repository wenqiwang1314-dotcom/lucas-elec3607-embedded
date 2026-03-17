#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <i2c/smbus.h>

#include "Si5351A-RevB-Registers.h"

#define I2C_FNAME   "/dev/i2c-3"
#define SI5351_ADDR 0x60

static int i2c_file = -1;

void i2c_init(void)
{
    i2c_file = open(I2C_FNAME, O_RDWR);
    if (i2c_file < 0) {
        perror("open /dev/i2c-3");
        exit(1);
    }

    if (ioctl(i2c_file, I2C_SLAVE, SI5351_ADDR) < 0) {
        perror("ioctl I2C_SLAVE");
        close(i2c_file);
        exit(1);
    }

    printf("I2C init OK: %s addr=0x%x\n", I2C_FNAME, SI5351_ADDR);
}

void i2c_close_dev(void)
{
    if (i2c_file >= 0) {
        close(i2c_file);
        i2c_file = -1;
    }
}

int i2c_read(unsigned char reg)
{
    int res = i2c_smbus_read_byte_data(i2c_file, reg);
    if (res < 0) {
        perror("i2c_smbus_read_byte_data");
        exit(1);
    }

    printf("R reg(0x%02x) = 0x%02x (%d)\n", reg, res & 0xff, res & 0xff);
    return res;
}

void i2c_write(unsigned char reg, unsigned char val)
{
    int res = i2c_smbus_write_byte_data(i2c_file, reg, val);
    if (res < 0) {
        perror("i2c_smbus_write_byte_data");
        exit(1);
    }

    printf("W reg(0x%02x) = 0x%02x\n", reg, val);
}

int should_write_from_table(unsigned int addr)
{
    return ((addr >= 15 && addr <= 92) ||
            (addr >= 149 && addr <= 170));
}

void si5351_apply_config(void)
{
    int i;

    puts("== Disable outputs ==");
    i2c_write(3, 0xFF);

    puts("== Power down all outputs ==");
    for (int reg = 16; reg <= 23; reg++) {
        i2c_write((unsigned char)reg, 0x80);
    }

    puts("== Set interrupt mask and XTAL load ==");
    i2c_write(2, 0x53);
    i2c_write(183, 0x92);

    puts("== Write ClockBuilder register map (15-92,149-170) ==");
    for (i = 0; i < SI5351A_REVB_REG_CONFIG_NUM_REGS; i++) {
        unsigned int addr = si5351a_revb_registers[i].address;
        unsigned char val = si5351a_revb_registers[i].value;

        if (should_write_from_table(addr)) {
            i2c_write((unsigned char)addr, val);
        }
    }

    puts("== PLL soft reset ==");
    i2c_write(177, 0xAC);

    usleep(10000);  // 10 ms，给一点稳定时间

    puts("== Enable outputs ==");
    i2c_write(3, 0x00);
}

int main(void)
{
    i2c_init();

    si5351_apply_config();

    puts("== Read back key registers ==");
    i2c_read(0x02);
    i2c_read(0x03);
    i2c_read(0x10);
    i2c_read(0x11);
    i2c_read(0x1A);
    i2c_read(0x2A);
    i2c_read(0x34);
    i2c_read(0xA6);
    i2c_read(0xB1);
    i2c_read(0xB7);

    i2c_close_dev();
    return 0;
}