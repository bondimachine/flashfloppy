/*
 * pico2/board.c
 *
 * Raspberry Pi Pico 2 (RP2350) board-specific setup and management.
 *
 * Pin map (physical GPIO numbers):
 *   0/1    UART0 TX/RX (serial console, debug builds)
 *   2-8    Floppy bus inputs: DIR, STEP, SEL0, MOTOR, WGATE, SIDE, WDATA
 *   9-14   Floppy bus outputs: DSKCHG, INDEX, TRK0, WRPROT, RDATA, RDY
 *   15     Speaker
 *   16-19  SD card on SPI0: MISO, CS, SCK, MOSI
 *   20/21  Display I2C0: SDA, SCL
 *   22     Button: Select
 *   26/27  Buttons: Right/Left, or rotary encoder A/B
 *   28     JC "jumper": interface select / Amiga HD-ID output
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#define pin_button_sel   22
#define pin_button_right 26
#define pin_button_left  27
#define pin_jc           28

/* Pull up currently unused and possibly-floating pins. */
static void gpio_pull_up_pins(GPIO gpio, uint16_t mask)
{
    unsigned int i;
    for (i = 0; i < 16; i++) {
        if (mask & 1)
            gpio_configure_pin(gpio, i, GPI_pull_up);
        mask >>= 1;
    }
}

unsigned int board_get_buttons(void)
{
    /* Select=1, Right=2, Left=4 (matching the Gotek encoding). */
    uint32_t in = sio->gpio_in;
    unsigned int x = ((in >> pin_button_sel) & 1)
        | (((in >> pin_button_right) & 1) << 1)
        | (((in >> pin_button_left) & 1) << 2);
    return ~x & 7;
}

unsigned int board_get_rotary(void)
{
    uint32_t in = sio->gpio_in;
    return ((in >> pin_button_right) & 1) | (((in >> pin_button_left) & 1) << 1);
}

uint32_t board_rotary_exti_mask;
void board_setup_rotary_exti(void)
{
    gpio_irq_enable(pin_button_right, GPIO_IRQ_EDGE_LOW | GPIO_IRQ_EDGE_HIGH);
    gpio_irq_enable(pin_button_left, GPIO_IRQ_EDGE_LOW | GPIO_IRQ_EDGE_HIGH);
    board_rotary_exti_mask = m(pin_button_right) | m(pin_button_left);
}

void board_jc_set_mode(unsigned int mode)
{
    gpio_configure_pin(gpioa, pin_jc, mode);
}

bool_t board_jc_strapped(void)
{
    return !gpio_read_pin(gpioa, pin_jc);
}

void board_init(void)
{
    uint16_t lo_skip, hi_skip;

    board_id = BRDREV_Gotek_standard;
    mcu_package = MCU_LQFP64;

    /* GPIO 0-15: UART (0,1), floppy inputs (2-8), floppy outputs (9-14,
     * configured by floppy_init), speaker (15). */
    lo_skip = 0xffff;

    /* GPIO 16-31: SD SPI (16-19), I2C display (20,21).
     * Pull up buttons (22,26,27) and JC (28). */
    hi_skip = 0x003f;

    gpio_pull_up_pins(gpioa, ~lo_skip);
    /* Upper pins: pull up buttons and JC explicitly. */
    gpio_configure_pin(gpioa, pin_button_sel, GPI_pull_up);
    gpio_configure_pin(gpioa, pin_button_right, GPI_pull_up);
    gpio_configure_pin(gpioa, pin_button_left, GPI_pull_up);
    gpio_configure_pin(gpioa, pin_jc, GPI_pull_up);
    (void)hi_skip;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
