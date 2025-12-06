/* Minimal CLI smoke-test / manual-inspection tool for the sim/ layer.
 * Copyright 2025 Thomas Reidemeister, MIT License (see devices/hc573.h)
 *
 * sim_test.c
 *
 * Not the primary way to write tests (that's the Python ctypes wrapper
 * over capi.h, see python/pysim) -- this is a quick, dependency-free way
 * to point the simulator at a real .hex file from the command line and
 * see what the enabled peripherals ended up showing, useful when
 * iterating on a device model itself.
 *
 * Usage: sim_test <hexfile> <instructions> [lcd] [digits=N] [adc=VALUE]
 *                 [ds1302=SS,MM,HH,DD,MO,WD,YY] [clock_hz=N]
 *                 [servo=PORT,BIT[,MIN_US,MAX_US]]
 *                 [enc28j60=CS_P,CS_B,SCK_P,SCK_B,MOSI_P,MOSI_B,MISO_P,MISO_B]
 *                 [fan=PWM_PORT,PWM_BIT,TACH_PORT,TACH_BIT]
 *   e.g. sim_test 18_lcd_name_id.hex 500000 lcd
 *        sim_test 17_digit_tube_student_id.hex 200000 digits=8
 *        sim_test clock_digit_tube.hex 500000 digits=8 ds1302=30,15,9,14,9,7,25
 *        sim_test 344_clock_lcd.hex 500000 lcd ds1302=0,0,0,1,1,1,25 clock_hz=10000000
 *        sim_test 05_enc_servo.hex 500000 servo=3,7
 *        sim_test 09_ethernet.hex 500000 enc28j60=0,3,0,2,0,0,0,1
 *        sim_test 06_fan_tach.hex 500000 fan=1,0,1,1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "capi.h"

int main(int argc, char **argv)
{
    sim_handle_t sim;
    long instructions;
    int want_lcd = 0;
    int want_digits = 0;
    int want_adc = 0;
    int adc_value = 0;
    int want_ds1302 = 0;
    int ds1302_fields[7] = {0, 0, 0, 1, 1, 1, 0}; // sec,min,hour,date,month,weekday,year
    unsigned long clock_hz = 0; // 0 = leave the board's own default
    int want_servo = 0;
    int servo_fields[4] = {0, 0, 1000, 2000}; // port,bit,min_us,max_us
    int want_enc28j60 = 0;
    int enc28j60_fields[8] = {0, 3, 0, 2, 0, 0, 0, 1}; // cs,sck,mosi,miso port/bit pairs
    int want_fan = 0;
    int fan_fields[4] = {1, 0, 1, 1}; // pwm_port,pwm_bit,tach_port,tach_bit
    int i;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <hexfile> <instructions> [lcd] [digits=N] [adc=VALUE]\n", argv[0]);
        return EXIT_FAILURE;
    }
    instructions = atol(argv[2]);

    for (i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "lcd") == 0)
            want_lcd = 1;
        else if (strncmp(argv[i], "digits=", 7) == 0)
            want_digits = atoi(argv[i] + 7);
        else if (strncmp(argv[i], "adc=", 4) == 0)
        {
            want_adc = 1;
            adc_value = atoi(argv[i] + 4);
        }
        else if (strncmp(argv[i], "ds1302=", 7) == 0)
        {
            char buf[64];
            char *tok;
            int f = 0;
            strncpy(buf, argv[i] + 7, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            tok = strtok(buf, ",");
            while (tok && f < 7)
            {
                ds1302_fields[f++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
            want_ds1302 = 1;
        }
        else if (strncmp(argv[i], "clock_hz=", 9) == 0)
            clock_hz = strtoul(argv[i] + 9, NULL, 10);
        else if (strncmp(argv[i], "servo=", 6) == 0)
        {
            char buf[64];
            char *tok;
            int f = 0;
            strncpy(buf, argv[i] + 6, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            tok = strtok(buf, ",");
            while (tok && f < 4)
            {
                servo_fields[f++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
            want_servo = 1;
        }
        else if (strncmp(argv[i], "enc28j60=", 9) == 0)
        {
            char buf[64];
            char *tok;
            int f = 0;
            strncpy(buf, argv[i] + 9, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            tok = strtok(buf, ",");
            while (tok && f < 8)
            {
                enc28j60_fields[f++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
            want_enc28j60 = 1;
        }
        else if (strncmp(argv[i], "fan=", 4) == 0)
        {
            char buf[64];
            char *tok;
            int f = 0;
            strncpy(buf, argv[i] + 4, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            tok = strtok(buf, ",");
            while (tok && f < 4)
            {
                fan_fields[f++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
            want_fan = 1;
        }
    }

    sim = sim_open("hc6800_es");
    if (!sim)
    {
        fprintf(stderr, "sim_open failed\n");
        return EXIT_FAILURE;
    }
    if (clock_hz > 0)
        sim_set_clock_hz(sim, clock_hz); // before any sim_enable_*: see capi.h
    if (sim_load_hex(sim, argv[1]) != 0)
    {
        fprintf(stderr, "failed to load %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    if (want_lcd)
        sim_enable_lcd(sim);
    if (want_digits > 0)
        sim_enable_digit_display(sim, want_digits);
    if (want_adc)
    {
        sim_enable_xpt2046(sim);
        sim_xpt2046_set_reading(sim, adc_value);
    }
    if (want_ds1302)
    {
        sim_enable_ds1302(sim);
        sim_ds1302_set_time(sim, ds1302_fields[0], ds1302_fields[1], ds1302_fields[2],
                             ds1302_fields[3], ds1302_fields[4], ds1302_fields[5], ds1302_fields[6]);
    }
    if (want_servo)
        sim_enable_servo(sim, servo_fields[0], servo_fields[1], servo_fields[2], servo_fields[3]);
    if (want_enc28j60)
        sim_enable_enc28j60(sim, enc28j60_fields[0], enc28j60_fields[1],
                             enc28j60_fields[2], enc28j60_fields[3],
                             enc28j60_fields[4], enc28j60_fields[5],
                             enc28j60_fields[6], enc28j60_fields[7]);
    if (want_fan)
        sim_enable_fan(sim, fan_fields[0], fan_fields[1], fan_fields[2], fan_fields[3]);

    {
        long done = sim_step_instructions(sim, instructions);
        printf("ran %ld instructions (%ld requested), %lu ticks\n",
               done, instructions, sim_get_tick_count(sim));
    }

    printf("P0=0x%02x P1=0x%02x P2=0x%02x P3=0x%02x\n",
           sim_get_port(sim, 0), sim_get_port(sim, 1),
           sim_get_port(sim, 2), sim_get_port(sim, 3));

    if (want_lcd)
    {
        char line0[41], line1[41];
        sim_lcd_get_line(sim, 0, 16, line0, sizeof(line0));
        sim_lcd_get_line(sim, 1, 16, line1, sizeof(line1));
        printf("LCD line0: \"%s\"\nLCD line1: \"%s\"\n", line0, line1);
    }

    if (want_digits > 0)
    {
        printf("digits:");
        for (i = 0; i < want_digits; i++)
            printf(" %c", sim_digit_get_char(sim, i));
        printf("\n");
        printf("digit segments (raw):");
        for (i = 0; i < want_digits; i++)
            printf(" 0x%02x", sim_digit_get_segments(sim, i));
        printf("\n");
    }
    if (want_ds1302)
    {
        printf("ds1302 registers (raw BCD):");
        for (i = 0; i < 7; i++)
            printf(" 0x%02x", sim_ds1302_get_register(sim, i));
        printf("\n");
    }

    if (want_adc)
        printf("adc last channel requested: %d\n", sim_xpt2046_get_last_channel(sim));

    if (want_servo)
        printf("servo pulse_us=%d angle_decidegrees=%d\n",
               sim_servo_get_pulse_us(sim), sim_servo_get_angle_decidegrees(sim));

    if (want_enc28j60)
        printf("enc28j60 bank=%d last_opcode=0x%02x buffer_bytes=%lu\n",
               sim_enc28j60_get_bank(sim), sim_enc28j60_get_last_opcode(sim),
               sim_enc28j60_get_buffer_byte_count(sim));

    if (want_fan)
        printf("fan duty_percent=%d rpm=%d\n",
               sim_fan_get_duty_percent(sim), sim_fan_get_rpm(sim));

    {
        int uart_n = sim_uart_tx_count(sim);
        printf("uart tx count: %d, bytes:", uart_n);
        for (i = 0; i < uart_n; i++)
            printf(" 0x%02x", (unsigned char)sim_uart_tx_byte(sim, i));
        printf("\n");
    }

    {
        int codes[16];
        int n = sim_get_exceptions(sim, codes, 16);
        if (n > 0)
        {
            printf("exceptions:");
            for (i = 0; i < n; i++)
                printf(" %d", codes[i]);
            printf("\n");
        }
    }

    sim_close(sim);
    return EXIT_SUCCESS;
}
