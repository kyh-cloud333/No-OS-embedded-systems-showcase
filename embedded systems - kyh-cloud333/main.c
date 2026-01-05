

/**
 * Github user: kyh-cloud333
 * Date: January 1, 2026
 *
 * Welcome to my Embedded Systems showcase without OS programming!
 *
 */
#include "driverlib/ioc.h" // IOID for lights, UART, Buttons
#include "driverlib/prcm.h" // power resource clock manager
#include "driverlib/timer.h" // timers that can be used in 16bit + 8bit prescalar or a single 32bit timer
#include "driverlib/sys_ctrl.h" //includes core components of the board like hw maps, interrupts, pwr_ctrl
#include "driverlib/osc.h" // system oscillator control (we dont use the oscillator functions but it includes different header files and mem-maps)

#include "driverlib/uart.h" // UART for serial input and output
#include "inc/hw_memmap.h" // memory map for defined constants
#include "inc/hw_gpio.h" // direct access for toggling lights (for showing the difference between direct register access mode and software driver mode)
#include "inc/hw_trng.h" // we need TRNG direct register access
#include "driverlib/trng.h" // random number generator
#include "driverlib/aon_batmon.h" // battery and temperature monitor

#define ONE_MS_32BIT_DIVIDER 48000000/(1000*16) // is 1 millisecond in our CPUT clock speed for the 32-bit timer configuration


// set up globals:
uint32_t random = 0;
char random_str[11]; // 10 characters for maximum number of digits (4294967296 max value for uint32_t), and need 1 character for null terminator '\0'

short currently_running = 0;
short echo_enabled = 0;
short blinker_period = 0;
short stopper = 0;
char mode = ' ';

short first_startup = 1;

int32_t temperature; //32 bit integer signed, and our temperature values are bit 16 to 8 (INT) in Figure 18-12 (page 1450)
static uint32_t voltage; // static 32 bit integer signed, and our voltage values are bit 10 to 8 (INT) and bit 7 to 0 (FRAC) -- Figure 18-10 (page 1448)

// display the user menu
void menu_display(){
    char menu[] = "Menu for 5 user commands:\r\n(stop) - stop current operation, reset system, and wait for next input\r\n(echo) - enables echo mode for user input to the UART\r\n(leds) - runs blinker mode, cycles through red, green, and red + green every second\r\n(moni) - runs temperature and battery monitoring\r\n(trng) - output a random number using TRNG to UART\r\n";

    for (int i = 0; i < sizeof(menu)/sizeof(menu[0]); i++){
        UARTCharPut(UART0_BASE, (uint8_t) (menu[i]));
    }

}

void IOC_Interrupt_Handler(){

}
// set up LED for green and red light, but we will also make a distinction between using the software driver model and direct register access mode
// also enable battery monitor, and buttons
void setup_GPIO(){
    // power on the peripheral domain
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);
    // wait until the power domain is done powering on
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON);

    // enable GPIO peripheral
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    // enable it in sleep mode (it should be sleeping unless you actually need to use it)
    PRCMPeripheralSleepEnable(PRCM_PERIPH_GPIO);

    // load the settings above you just set
    PRCMLoadSet();
    // wait until settings are done loading
    while (!PRCMLoadGet());

    // enable DIO6 and DIO7 (red and green LED) in output mode
    IOCPinTypeGpioOutput(IOID_6);
    IOCPinTypeGpioOutput(IOID_7);

    // Enable battery Monitor as well
    AONBatMonEnable();


    // now finally, set up the buttons

    IOCPinTypeGpioInput(IOID_13); // CONFIGURE GPIO13(BUTTON 1 on CC1350 board) in the INPUT direction (mode), that means it's not in output mode
    IOCPinTypeGpioInput(IOID_14); // ENABLE BUTTON 2 (DIO14)

    IOCIOPortPullSet(IOID_13, IOC_IOPULL_UP); //enable pull-up mode, which means our button1 is connected to a ground (0 voltage)
    IOCIOPortPullSet(IOID_14, IOC_IOPULL_UP); // enable pull up mode for button2

    // lets you enable or disable input hysteresis for a given GPIO (in our case DIO13 and DIO14), and hysteresis means preventing unwanted, rapid switching (bouncing/chattering)
    // by raising the threshold for an input to be recognized as a "high" state, and sets it so that the input/noise must fall below a threshold to be considered back to a "low state"
    IOCIOHystSet(IOID_13,IOC_HYST_ENABLE);
    IOCIOHystSet(IOID_14,IOC_HYST_ENABLE);


    IOCIntClear(IOID_13); // clear any current interrupts with regards to DIO13 (button1)
    IOCIntClear(IOID_14); // button2 (DIO14) clear
    IOCIntRegister(IOC_Interrupt_Handler); // the function we made called "IOC_Interrupt_Handler" is linked to the interrupt event (button press), and in our case, button1 and button2 will call this

    // when it comes to interrupts, you must 1. define/identify the event that triggers the interrupt, and 2. define/identify the function/code to be executed
    // arg 1: DIO13 (button), arg 2: now enable (listening) interrupt event, arg 3: the status of button changes from 1 to 0 (PULL-UP) (it's pull up because our board is made that way)
    IOCIOIntSet(IOID_13,IOC_INT_ENABLE, IOC_FALLING_EDGE); //button1
    IOCIOIntSet(IOID_14,IOC_INT_ENABLE,IOC_FALLING_EDGE); //button2

    /* To know which IO caused interrupt use:
     * IOCIntStatus(uint32_t ui32IOId)
     */

}


// set up the TRNG, this is much more of a "true random", especially when compared to using srand or rand in C (which are pseudo random)
void setup_RNG(){

    // power on peripheral domain
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);

    // again, wait until the power domain is powered on
    while (PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON);

    // enable TRNG peripheral
    PRCMPeripheralRunEnable(PRCM_PERIPH_TRNG);

    // run the TRNG in sleep mode
    PRCMPeripheralSleepEnable(PRCM_PERIPH_TRNG);

    // load the settings
    PRCMLoadSet();
    // wait until the settings are done loading
    while (!PRCMLoadGet());

    /* Technical Reference table 16-3, TRNG initialization sequence (Page 1274)
     * 1. Execute SW reset
     * 2. Wait for SW completion by polling
     * 3. Select number of FRO clock input cycles between 2 samples
     * 4. Select the number of samples taken to gather enough entropy in the FROs of the module and to generate the first random value
     * 5. Select the minimum number of samples taken regenerate entropy in the FROs of the module and to generate subsequent random values
     * 6. Select the maximum number of samples taken regenerate entropy in the FROs of the module and to generate subsequent random values. Also defines timeout period for shutting down the FROs after inactivity
     * 7. Configure the desired FROs to run 5% faster
     * 8. Enable all FROs
     * 9. Select the maximum number of samples after which a detected repeated pattern an alarm event is generated
     * 10. Enable and start
     */

    TRNGReset(); // when you reset, it says you have to wait 5 clock cycles

    // CPUDelay(2); // 6 clock cycles pass <- this is an incorrect way of doing it, you are losing 1 clock cycle this way

    // use direct register access mode to poll the bit

    while ((HWREG(TRNG_BASE + TRNG_O_SWRESET) & 0x00000001) != 0); // wait until the last bit isn't equal to 1 anymore (back to 0)

    //configure the TRNG settings
    TRNGConfigure(128, 16777216, 1); // minimum 128 = (2^7) samples per generated random number, maximum 16777216 = (2^24) samples per generated random number, and taking every 2nd sample generated

    // enable the TRNG after configuring it, it is always generating at the configured rate
    TRNGEnable();

    // wait until a rng is ready to be grabbed
    while (TRNGStatusGet() != TRNG_NUMBER_READY);

    // now actually get the random number
    random = TRNGNumberGet(TRNG_LOW_WORD);
}

// set the UART interrupt handler, for when user inputs commands
void UART_Interrupt_Handler(){

    // if the UARTIntStatus isn't the status of received an interrupt, we return
    if (UARTIntStatus(UART0_BASE, true) != UART_INT_RX){
        return;
    }

    // clear the raised interrupt or we will loop forever
    UARTIntClear(UART0_BASE, UART_INT_RX|UART_INT_TX);

    // we need to be able to store the 4 characters when this interrupt is raised
    int32_t ch1;
    int32_t ch2;
    int32_t ch3;
    int32_t ch4;

    // different messages depending on the mode you enabled
    char echo_on[] = "Echo mode on\r\n";
    char echo_off[] = "Echo mode off\r\n";
    char stop_msg[] = "Operation stopped - waiting for next input\r\n";

    char led_on[] = "LED blinker mode on\r\nPlease use (stop) to safely stop the blinker mode and return to the main menu\r\n(leds) ";
    char moni_on[] = "Temperature and Battery monitor mode on\r\n";
    char trng_on[] = "TRNG mode on\r\n";

    // we have our threshold set to 1/8 (which is 4 characters), so every single time this interrupt is raised, we will have 4 characters to read

    if(UARTCharsAvail(UART0_BASE)){
        // get the 4 characters and store it

        ch1 = UARTCharGetNonBlocking(UART0_BASE) & 0x000000FF;
        ch2 = UARTCharGetNonBlocking(UART0_BASE) & 0x000000FF;
        ch3 = UARTCharGetNonBlocking(UART0_BASE) & 0x000000FF;
        ch4 = UARTCharGetNonBlocking(UART0_BASE) & 0x000000FF;
    }

    /* UART serial input commands:
     * 1. "stop" will stop the current mode's operation
     * 2. "echo" will enable echo inputs you make to UART serial output
     * 3. "leds" - blinker mode using one shot timer
     * 4. "moni" - monitor mode to display temperature and voltage using one shot timer
     * 5. "trng" - generates and provides a random number to you through UART
     */

    // echo user input back if echo mode is enabled
    if (echo_enabled == 1){
        UARTCharPut(UART0_BASE, (uint8_t) (ch1));
        UARTCharPut(UART0_BASE, (uint8_t) (ch2));
        UARTCharPut(UART0_BASE, (uint8_t) (ch3));
        UARTCharPut(UART0_BASE, (uint8_t) (ch4));
        UARTCharPut(UART0_BASE, (uint8_t) ('\r'));
        UARTCharPut(UART0_BASE, (uint8_t) ('\n'));
    }

    // if input is "echo" then enable echo mode
    if (ch1 == 'e' && ch2 == 'c' && ch3 == 'h' && ch4 == 'o'){
        if (echo_enabled == 0){
            echo_enabled = 1;
            for (int i = 0; i < sizeof(echo_on)/sizeof(echo_on[0]); i++){
                UARTCharPut(UART0_BASE, (uint8_t) (echo_on[i]));
            }
        }
        else{
            // if echo is already enabled, turn it off
            echo_enabled = 0;
            for (int i = 0; i < sizeof(echo_off)/sizeof(echo_off[0]); i++){
                UARTCharPut(UART0_BASE, (uint8_t) (echo_off[i]));
            }
        }
    }
    // set stopper flag if stopper is input
    else if(ch1 == 's' && ch2 == 't' && ch3 == 'o' && ch4 == 'p' && stopper == 0 && currently_running == 1){
        stopper = 1;
        for (int i = 0; i < sizeof(stop_msg)/sizeof(stop_msg[0]); i++){
            UARTCharPut(UART0_BASE, (uint8_t) (stop_msg[i]));
        }
    }

    // determine the mode and set flag, then output message to serial
    else if (ch1 == 'l' && ch2 == 'e' && ch3 == 'd' && ch4 == 's'){
        // in this specific case, it's safe to allow multiple inputs of "leds" since we are ONLY setting a flag here, we handle all the actual timing within the general purpose timer ISR
        // the point being, only really want to enforce the user manually stopping the LED mode -- you dont want the led light to suddenly toggle on and off too quickly (dangerous)
        mode = 'b';
        for (int i = 0; i < sizeof(led_on)/sizeof(led_on[0]); i++){
            UARTCharPut(UART0_BASE, (uint8_t) (led_on[i]));
        }

        if (currently_running == 0){
            currently_running = 1;
            TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*0); // will jump to interrupt immediately when timer runs
            TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
            TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW
        }
    }
    else if (ch1 == 'm' && ch2 == 'o' && ch3 == 'n' && ch4 == 'i' && mode != 'b'){

        mode = 'm';
        for (int i = 0; i < sizeof(moni_on)/sizeof(moni_on[0]); i++){
            UARTCharPut(UART0_BASE, (uint8_t) (moni_on[i]));
        }

        if (currently_running == 0){
            currently_running = 1;
            TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*0); // will jump to interrupt immediately when timer runs
            TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
            TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW
        }

    }
    else if (ch1 == 't' && ch2 == 'r' && ch3 == 'n' && ch4 == 'g' && mode != 'b'){
        mode = 'r';
        for (int i = 0; i < sizeof(trng_on)/sizeof(trng_on[0]); i++){
            UARTCharPut(UART0_BASE, (uint8_t) (trng_on[i]));
        }

        if (currently_running == 0){
            currently_running = 1;
            TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*0); // will jump to interrupt immediately when timer runs
            TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
            TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW
        }

    }
    else{
        if (mode == 'b'){
            for (int i = 0; i < sizeof(led_on)/sizeof(led_on[0]); i++){
                UARTCharPut(UART0_BASE, (uint8_t) (led_on[i]));
            }
        }
        else{
            UARTCharPut(UART0_BASE, (uint8_t) ('\r'));
            UARTCharPut(UART0_BASE, (uint8_t) ('\n'));
            menu_display();
        }
    }
    return;



}


// set up the UART for serial output and input
void setup_UART(){

    // Power on the serial domain
    PRCMPowerDomainOn(PRCM_DOMAIN_SERIAL);
    // wait until the serial domain is done powering on
    while(PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL) != PRCM_DOMAIN_POWER_ON);

    PRCMPeripheralRunEnable(PRCM_PERIPH_UART0); // enable UART0 peripheral
    PRCMPeripheralSleepEnable(PRCM_PERIPH_UART0); // enable UART0 into sleep mode
    PRCMLoadSet(); // load the settings
    while (!PRCMLoadGet()); // wait until the settings are loaded

    // 19.6 (page 1460) in the technical reference has it such that if you want to use the UART, you must follow these steps:
        // 1. Enable UART Pins
        IOCPinTypeUart(UART0_BASE , IOID_2, IOID_3, IOID_19, IOID_18);

        // 2. Disable UART
        UARTDisable(UART0_BASE);

        // 3. UART Configuration
        UARTConfigSetExpClk(UART0_BASE,48000000,9600, UART_CONFIG_WLEN_8|UART_CONFIG_STOP_ONE|UART_CONFIG_PAR_NONE);
        // disable flow control, we manually do everything
        UARTHwFlowControlDisable(UART0_BASE);

        // 4. Set FIFO Thresholds
        UARTFIFOLevelSet    (UART0_BASE, UART_FIFO_TX1_8, UART_FIFO_RX1_8); // transmit threshold is 1/8, receive threshold is 1/8, which means every 4 characters (1/8 * 32 = 4)

        // 5. UART interrupt handler assignment
        UARTIntRegister(UART0_BASE,     UART_Interrupt_Handler);  // setting "UART_Interrupt_Handler" to be the ISR that handles UART0 interrupts

        // 6. Enable Interrupts
        UARTIntEnable(UART0_BASE , UART_INT_RX);  // after you set the ISR, you still have to enable it

        // 7. Last step
        UARTEnable(UART0_BASE);

}

void Timer_Interrupt_Handler(){
    TimerIntClear(GPT0_BASE, TIMER_TIMA_TIMEOUT); // clear the raised interrupt or it will loop forever

    // we configured our 1 shot timer with 0 seconds, it will go to this interrupt immediately and display menu message, then return and stay in sleep until command entered
    if(first_startup == 1){
        first_startup = 0;
        menu_display();
        return;
    }
    else if(stopper == 1){
        //stop input is made, stop the system and reset everything
        currently_running = 0;
        blinker_period = 0;
        stopper = 0;

        if (mode == 'b'){
            (HWREG(GPIO_BASE + GPIO_O_DOUT7_4) &=0x00000000); // all lights off
        }
        mode = ' ';
        menu_display();
        return;
    }


    // it's not the first_startup or stopper, check to see if we'll be blinking, temperature/battery monitor, or TRNG output

    switch(mode){

    /*        (HWREG(GPIO_BASE + GPIO_O_DOUT7_4) &=0x00000000); // all lights off
     *        (HWREG(GPIO_BASE + GPIO_O_DOUT7_4) |= 0x01010000); // all lights on

            //(HWREG(GPIO_BASE + GPIO_O_DOUT7_4) |=0x01000000); //green light on
            //(HWREG(GPIO_BASE + GPIO_O_DOUT7_4) |=0x00010000); // red light on
    */
        case 'b':

            switch(blinker_period){
                case 0:
                    // red light on 1000 ms
                    blinker_period = 1;

                    (HWREG(GPIO_BASE + GPIO_O_DOUT7_4) |= 0x00010000); // red light on

                    TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*1000); // 1000ms
                    TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
                    TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW
                    break;

                case 1:
                    // all lights off 400ms
                    blinker_period = 2;

                    (HWREG(GPIO_BASE + GPIO_O_DOUT7_4) &=0x00000000); // all lights off

                    TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*400); // 400ms
                    TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
                    TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW
                    break;

                case 2:
                    // green light on 1000ms
                    blinker_period = 3;

                    (HWREG(GPIO_BASE + GPIO_O_DOUT7_4) |=0x01000000); //green light on

                    TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*1000); // 1000ms
                    TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
                    TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW
                    break;
                case 3:
                    // all lights off 400 ms
                    blinker_period = 4;

                    (HWREG(GPIO_BASE + GPIO_O_DOUT7_4) &=0x00000000); // all lights off

                    TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*400); // 400ms
                    TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
                    TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW
                    break;
                case 4:
                    // red + green light on 1000ms
                    blinker_period = 5;

                    (HWREG(GPIO_BASE + GPIO_O_DOUT7_4) |= 0x01010000); // all lights on

                    TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*1000); // 1000ms
                    TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
                    TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW
                    break;

                case 5:
                    // all lights off 400 ms
                    blinker_period = 0;

                    (HWREG(GPIO_BASE + GPIO_O_DOUT7_4) &=0x00000000); // all lights off

                    TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*400); // 400ms
                    TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
                    TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW
                    break;
            }
            // blinker mode done
            break;


        // battery monitor mode
        case 'm':
            voltage = AONBatMonBatteryVoltageGet();
            temperature = AONBatMonTemperatureGetDegC();

            short first_temp = temperature/10;
            short second_temp = temperature%10;

            short first_volt = voltage >> 8; // integer part of voltage is reserved with 3 bits
            short second_volt = ((voltage & 0xFF) * 100)/256;  // the fractional part for the voltage is 8 bits, and we want to scale it, then we can output it character by character i.e. (fractional * 100)/256

            short frac_volt1 = second_volt/10;
            short frac_volt2 = second_volt%10;



            UARTCharPut(UART0_BASE, (uint8_t) (first_temp + '0'));
            UARTCharPut(UART0_BASE, (uint8_t) (second_temp + '0'));
            UARTCharPut(UART0_BASE,'c');

            UARTCharPut(UART0_BASE, ' ');

            UARTCharPut(UART0_BASE, (uint8_t) (first_volt + '0'));
            UARTCharPut(UART0_BASE, '.');
            UARTCharPut(UART0_BASE, (uint8_t) (frac_volt1 + '0'));
            UARTCharPut(UART0_BASE, (uint8_t) (frac_volt2 + '0'));
            UARTCharPut(UART0_BASE,'v');

            UARTCharPut(UART0_BASE, '\n');
            UARTCharPut(UART0_BASE, '\r');

            TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*1000); // 1000ms
            TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
            TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW




            break;

        // TRNG mode
        case 'r':
            // wait until a rng is ready to be grabbed
            while (TRNGStatusGet() != TRNG_NUMBER_READY);

            // now actually get the random number
            random = TRNGNumberGet(TRNG_LOW_WORD);

            // loop and get every number character by character and store it, but it will be stored backwards
            int holder = 0;
            while(random > 0){
                random_str[holder] = random%10 + '0';
                random = random/10;
                holder ++;
            }



            // now we have the backwards random number, iterate and print it out
            // key idea !! int holder tells us how much we used of the list !! so we can use it to output the random number to the serial

            for (int j = holder - 1; j >= 0; j--){
                UARTCharPut(UART0_BASE, (uint8_t) (random_str[j]));
            }
            UARTCharPut(UART0_BASE, '\n');
            UARTCharPut(UART0_BASE, '\r');

            TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*1000); // 1000ms
            TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT); // enable interrupts for the timer
            TimerEnable(GPT0_BASE,TIMER_A); // enable the timer, ** STARTS COUNTING FROM NOW

            break;

        default:
            break;
    }

}

// set up general purpose timer
void setup_Timer(){

    //Power on peripheral domain
    PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);
    // wait until the power domain is done turning on
    while(PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) != PRCM_DOMAIN_POWER_ON);

    // Power on the TIMER0 peripheral
    PRCMPeripheralRunEnable(PRCM_PERIPH_TIMER0);
    // Enable TIMER0 to continue counting while the MCU sleeps
    PRCMPeripheralSleepEnable(PRCM_PERIPH_TIMER0);
    PRCMLoadSet();
    while ( !PRCMLoadGet() );


    // set the input clock to the Timer = CPUClock/16
    PRCMGPTimerClockDivisionSet(PRCM_CLOCK_DIV_16);
    PRCMLoadSet();
    // wait until the settings are done loading
    while (!PRCMLoadGet());


    // configure the new 1-shot timer
    TimerConfigure(GPT0_BASE,TIMER_CFG_ONE_SHOT);
    // we want our program to start in sleep mode instantly, so the initially configured 1 shot timer is set to 0 seconds
    TimerLoadSet(GPT0_BASE,TIMER_A, ONE_MS_32BIT_DIVIDER*0);

    // assign timer interrupt handler
    TimerIntRegister(GPT0_BASE, TIMER_A, Timer_Interrupt_Handler);

    // enable the interrupt
    TimerIntEnable(GPT0_BASE,TIMER_TIMA_TIMEOUT);


    // enable the timer, ** THIS STARTS COUNTING
    TimerEnable(GPT0_BASE,TIMER_A);
}


int main(void)
{
    setup_RNG();
    setup_GPIO();
    setup_UART();
    setup_Timer();

    while (1){
        PRCMSleep();
    }
}
