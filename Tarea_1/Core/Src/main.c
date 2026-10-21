/**
 ******************************************************************************
 * @file    : main.c
 * @brief   : Tarea 1 Simon Londoño- Contador de 4 digitos con photo-interruptores
 ******************************************************************************
 * DESCRIPCION GENERAL
 * Un display de 7 segmentos de 4 digitos muestra un numero. Un photo-interruptor
 * lo aumenta en 1 y otro lo disminuye en 1. Un LED de estado parpadea para
 * indicar que el micro funciona. Cada parte es independiente: solo comparten
 * la variable numero_mostrar en la RAM.
 *
 * HARDWARE
 * - NUCLEO-F411RE con placa adaptadora (reloj HSI 16 MHz, boton de RESET).
 * - Display 5641BS de anodo comun con un transistor PNP por digito
 *   (segmentos y digitos encienden con 0).
 * - 2 photo-interruptores H2010 con resistencia pull-up (libre = 0, tapado = 1).
 * - LED2 de la adaptadora como LED de estado.
 *
 * PINES
 * Segmentos: a=PB12 b=PA12 c=PC11 d=PC12 e=PC10 f=PA11 g=PD2
 * Digitos:   uni=PB7 dec=PC8 cent=PC6 umil=PC13   |   LED: PH1
 * Sensores:  PB0 (EXTI0, flanco de subida, suma) | PC1 (EXTI1, flanco de bajada, resta)
 *
 * FUNCIONAMIENTO POR PARTES
 * 1. Configuracion: init_gpio, init_timers e init_exti preparan pines,
 *    timers e interrupciones.
 * 2. Display (TIM2, cada 1 ms): enciende un digito a la vez. Primero apaga
 *    todos, crea el numero y despues prende el digito (sin fantasmas). Por el
 *    efecto POV se ven los 4 encendidos.
 * 3. LED de estado (TIM3, cada 0,25 s): blinky.
 * 4. Reloj (SysTick, cada 1 ms): cuenta el tiempo para el antirrebote.
 * 5. Sensores (EXTI0 y EXTI1): solo anotan el evento en RAM.
 * 6. Maquina de estado (en el while del main):
 *    IDLE -> STATE_DEBOUNCE (espera 50 ms y suma o resta)
 *         -> UPDATE_DISPLAY (separa el numero en cifras) -> IDLE
 ******************************************************************************
 */
//include: librerias que se van a usar
#include <stdint.h>            // Tipos de datos como uint8_t, uint16_t, uint32_t
#include "stm32f411xe.h"       // Nombres de los registros del micro (RCC, GPIOA, TIM2...)
#include "stm32f4xx.h"         // Funciones del nucleo como NVIC_EnableIRQ

// Estados de la maquina de estado
#define IDLE            0      // En reposo: no hay nada que hacer
#define STATE_DEBOUNCE  1      // Esperando que pase el rebote del sensor
#define UPDATE_DISPLAY  2      // Hay que separar el numero nuevo en cifras

// Sensores
#define SENSOR_SUBE     1      // Codigo para saber que disparo el sensor que suma
#define SENSOR_BAJA     2      // Codigo para saber que disparo el sensor que resta

#define TIEMPO_DEBOUNCE 50     // Milisegundos que espero para ignorar el rebote

//2. Definicion de variables (globales)
// volatile: porque se usan dentro de interrupciones y el compilador no debe "guardarlas"
volatile uint16_t numero_mostrar = 0;          // Numero que se ve en el display (16 bits: 0 a 65535)
volatile uint8_t  unidades = 0;                // Cifra de las unidades
volatile uint8_t  decenas = 0;                 // Cifra de las decenas
volatile uint8_t  centenas = 0;                // Cifra de las centenas
volatile uint8_t  umil = 0;                    // Cifra de las unidades de mil
volatile uint8_t  display_actual = 0;          // Que digito toca prender: 0=uni 1=dec 2=cent 3=umil

volatile uint32_t updateTicks = 0;             // Cuenta los ms que lleva el programa (lo sube el SysTick)
volatile uint8_t  fsm_Next_State = IDLE;       // Estado que tiene que ejecutar el main
volatile uint8_t  sensor_activo = 0;           // Guarda cual sensor fue el que disparo
volatile uint32_t tiempo_evento = 0;           // Guarda en que ms disparo el sensor

//cabeceras (prototipos) de funciones
void init_gpio(void);                  // Configura todos los pines
void init_timers(void);                // Configura TIM2, TIM3 y SysTick
void init_exti(void);                  // Configura las interrupciones de los sensores
void action_fsm(uint8_t state);        // Ejecuta el estado de la maquina de estado
void split_number(uint16_t number);    // Separa el numero en 4 cifras
void draw_number(uint8_t number);      // Pone un numero del 0 al 9 en los segmentos
void update_display(uint8_t display);  // Prende un digito con su numero
uint32_t getTicks(void);               // Devuelve los ms que han pasado

//main() con su while (1) para que el programa siga en bucle
int main(void)
{
    //llamar a las funciones de configuracion
    init_gpio();                       // Primero los pines
    split_number(numero_mostrar);      // Separo el numero inicial antes de que arranque el refresco
    init_timers();                     // Arrancan el refresco, el LED y el reloj
    init_exti();                       // Se activan los sensores

    /* Todo funciona por interrupciones: main solo ejecuta la maquina
     * de estado cuando una ISR la pidio */
    while (1){
        if(fsm_Next_State != IDLE){            // Si algun sensor pidio algo...
            action_fsm(fsm_Next_State);        // ...ejecuto ese estado
        }
    }
    return 0;
}

/* init_gpio: activa relojes y configura cada pin (segmentos, digitos, LED y sensores) */
void init_gpio(void){
    /* PASO 1: RELOJES DE LOS PUERTOS (RM0383 - 6.3.9)
     * AHB1ENR: cada bit le da reloj a un puerto; sin reloj el puerto no responde */
    RCC->AHB1ENR &= ~RCC_AHB1ENR_GPIOAEN;   // Limpio el bit del puerto A
    RCC->AHB1ENR |=  RCC_AHB1ENR_GPIOAEN;   // Le doy reloj al puerto A

    RCC->AHB1ENR &= ~RCC_AHB1ENR_GPIOBEN;   // Limpio el bit del puerto B
    RCC->AHB1ENR |=  RCC_AHB1ENR_GPIOBEN;   // Le doy reloj al puerto B

    RCC->AHB1ENR &= ~RCC_AHB1ENR_GPIOCEN;   // Limpio el bit del puerto C
    RCC->AHB1ENR |=  RCC_AHB1ENR_GPIOCEN;   // Le doy reloj al puerto C

    RCC->AHB1ENR &= ~RCC_AHB1ENR_GPIODEN;   // Limpio el bit del puerto D
    RCC->AHB1ENR |=  RCC_AHB1ENR_GPIODEN;   // Le doy reloj al puerto D (segmento g)

    RCC->AHB1ENR &= ~RCC_AHB1ENR_GPIOHEN;   // Limpio el bit del puerto H
    RCC->AHB1ENR |=  RCC_AHB1ENR_GPIOHEN;   // Le doy reloj al puerto H (LED de estado)

    /* PASO 2: SEGMENTOS COMO SALIDA (RM0383 - 8.4)
     * Anodo comun: encienden con 0, por eso primero se ponen en 1 */
    /* PB12: segmento a */
    GPIOB->ODR     |=  (GPIO_ODR_OD12);           // ODR: valor de salida. Lo pongo en 1 para que arranque apagado
    GPIOB->MODER   &= ~(GPIO_MODER_MODER12);      // MODER: modo del pin. Limpio los 2 bits (00)
    GPIOB->MODER   |=  (GPIO_MODER_MODER12_0);    // Pongo 01 = salida
    GPIOB->OTYPER  &= ~(GPIO_OTYPER_OT12);        // OTYPER: tipo de salida. 0 = push-pull (saca 1 y 0)
    GPIOB->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED12);   // OSPEEDR: velocidad. 00 = baja, alcanza para el display
    GPIOB->PUPDR   &= ~(GPIO_PUPDR_PUPD12);       // PUPDR: resistencias internas. 00 = ninguna

    /* PA12: segmento b */
    GPIOA->ODR     |=  (GPIO_ODR_OD12);           // Arranca en 1 = apagado
    GPIOA->MODER   &= ~(GPIO_MODER_MODER12);      // Limpio el modo
    GPIOA->MODER   |=  (GPIO_MODER_MODER12_0);    // 01 = salida
    GPIOA->OTYPER  &= ~(GPIO_OTYPER_OT12);        // Push-pull
    GPIOA->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED12);   // Velocidad baja
    GPIOA->PUPDR   &= ~(GPIO_PUPDR_PUPD12);       // Sin resistencias internas

    /* PC11: segmento c */
    GPIOC->ODR     |=  (GPIO_ODR_OD11);           // Arranca en 1 = apagado
    GPIOC->MODER   &= ~(GPIO_MODER_MODER11);      // Limpio el modo
    GPIOC->MODER   |=  (GPIO_MODER_MODER11_0);    // 01 = salida
    GPIOC->OTYPER  &= ~(GPIO_OTYPER_OT11);        // Push-pull
    GPIOC->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED11);   // Velocidad baja
    GPIOC->PUPDR   &= ~(GPIO_PUPDR_PUPD11);       // Sin resistencias internas

    /* PC12: segmento d */
    GPIOC->ODR     |=  (GPIO_ODR_OD12);           // Arranca en 1 = apagado
    GPIOC->MODER   &= ~(GPIO_MODER_MODER12);      // Limpio el modo
    GPIOC->MODER   |=  (GPIO_MODER_MODER12_0);    // 01 = salida
    GPIOC->OTYPER  &= ~(GPIO_OTYPER_OT12);        // Push-pull
    GPIOC->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED12);   // Velocidad baja
    GPIOC->PUPDR   &= ~(GPIO_PUPDR_PUPD12);       // Sin resistencias internas

    /* PC10: segmento e */
    GPIOC->ODR     |=  (GPIO_ODR_OD10);           // Arranca en 1 = apagado
    GPIOC->MODER   &= ~(GPIO_MODER_MODER10);      // Limpio el modo
    GPIOC->MODER   |=  (GPIO_MODER_MODER10_0);    // 01 = salida
    GPIOC->OTYPER  &= ~(GPIO_OTYPER_OT10);        // Push-pull
    GPIOC->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED10);   // Velocidad baja
    GPIOC->PUPDR   &= ~(GPIO_PUPDR_PUPD10);       // Sin resistencias internas

    /* PA11: segmento f */
    GPIOA->ODR     |=  (GPIO_ODR_OD11);           // Arranca en 1 = apagado
    GPIOA->MODER   &= ~(GPIO_MODER_MODER11);      // Limpio el modo
    GPIOA->MODER   |=  (GPIO_MODER_MODER11_0);    // 01 = salida
    GPIOA->OTYPER  &= ~(GPIO_OTYPER_OT11);        // Push-pull
    GPIOA->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED11);   // Velocidad baja
    GPIOA->PUPDR   &= ~(GPIO_PUPDR_PUPD11);       // Sin resistencias internas

    /* PD2: segmento g */
    GPIOD->ODR     |=  (GPIO_ODR_OD2);            // Arranca en 1 = apagado
    GPIOD->MODER   &= ~(GPIO_MODER_MODER2);       // Limpio el modo
    GPIOD->MODER   |=  (GPIO_MODER_MODER2_0);     // 01 = salida
    GPIOD->OTYPER  &= ~(GPIO_OTYPER_OT2);         // Push-pull
    GPIOD->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED2);    // Velocidad baja
    GPIOD->PUPDR   &= ~(GPIO_PUPDR_PUPD2);        // Sin resistencias internas

    /* PASO 3: DIGITOS (BASE DE LOS PNP) COMO SALIDA
     * El PNP conduce con 0, por eso primero se ponen en 1 */
    /* PB7: digito unidades */
    GPIOB->ODR     |=  (GPIO_ODR_OD7);            // 1 = transistor cortado = digito apagado
    GPIOB->MODER   &= ~(GPIO_MODER_MODER7);       // Limpio el modo
    GPIOB->MODER   |=  (GPIO_MODER_MODER7_0);     // 01 = salida
    GPIOB->OTYPER  &= ~(GPIO_OTYPER_OT7);         // Push-pull
    GPIOB->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED7);    // Velocidad baja
    GPIOB->PUPDR   &= ~(GPIO_PUPDR_PUPD7);        // Sin resistencias internas

    /* PC8: digito decenas */
    GPIOC->ODR     |=  (GPIO_ODR_OD8);            // 1 = digito apagado
    GPIOC->MODER   &= ~(GPIO_MODER_MODER8);       // Limpio el modo
    GPIOC->MODER   |=  (GPIO_MODER_MODER8_0);     // 01 = salida
    GPIOC->OTYPER  &= ~(GPIO_OTYPER_OT8);         // Push-pull
    GPIOC->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED8);    // Velocidad baja
    GPIOC->PUPDR   &= ~(GPIO_PUPDR_PUPD8);        // Sin resistencias internas

    /* PC6: digito centenas */
    GPIOC->ODR     |=  (GPIO_ODR_OD6);            // 1 = digito apagado
    GPIOC->MODER   &= ~(GPIO_MODER_MODER6);       // Limpio el modo
    GPIOC->MODER   |=  (GPIO_MODER_MODER6_0);     // 01 = salida
    GPIOC->OTYPER  &= ~(GPIO_OTYPER_OT6);         // Push-pull
    GPIOC->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED6);    // Velocidad baja
    GPIOC->PUPDR   &= ~(GPIO_PUPDR_PUPD6);        // Sin resistencias internas

    /* PC13: digito unidades de mil */
    GPIOC->ODR     |=  (GPIO_ODR_OD13);           // 1 = digito apagado
    GPIOC->MODER   &= ~(GPIO_MODER_MODER13);      // Limpio el modo
    GPIOC->MODER   |=  (GPIO_MODER_MODER13_0);    // 01 = salida
    GPIOC->OTYPER  &= ~(GPIO_OTYPER_OT13);        // Push-pull
    GPIOC->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED13);   // Velocidad baja (PC13 no aguanta mas de 2 MHz)
    GPIOC->PUPDR   &= ~(GPIO_PUPDR_PUPD13);       // Sin resistencias internas

    /* PASO 4: LED DE ESTADO COMO SALIDA */
    /* PH1: LED de estado (LED2 verde) */
    GPIOH->MODER   &= ~(GPIO_MODER_MODER1);       // Limpio el modo
    GPIOH->MODER   |=  (GPIO_MODER_MODER1_0);     // 01 = salida
    GPIOH->OTYPER  &= ~(GPIO_OTYPER_OT1);         // Push-pull
    GPIOH->OSPEEDR &= ~(GPIO_OSPEEDR_OSPEED1);    // Velocidad baja
    GPIOH->PUPDR   &= ~(GPIO_PUPDR_PUPD1);        // Sin resistencias internas
    GPIOH->ODR     |=  (GPIO_ODR_OD1);            // El LED arranca encendido

    /* PASO 5: SENSORES COMO ENTRADA */
    /* PB0: sensor que sube */
    GPIOB->MODER   &= ~(GPIO_MODER_MODER0);       // 00 = entrada (el pin solo lee)
    GPIOB->PUPDR   &= ~(GPIO_PUPDR_PUPD0);        // Sin resistencia interna (ya tiene la de 10k en la proto)

    /* PC1: sensor que baja */
    GPIOC->MODER   &= ~(GPIO_MODER_MODER1);       // 00 = entrada
    GPIOC->PUPDR   &= ~(GPIO_PUPDR_PUPD1);        // Sin resistencia interna (ya tiene la de 10k en la proto)
}

/* init_timers: TIM2 refresco cada 1 ms, TIM3 LED cada 0,25 s, SysTick reloj cada 1 ms */
void init_timers(void){
    /* TIM2: 16 MHz / 16 = 1 MHz ; 1 MHz / 1000 = 1 kHz (cada 1 ms, cada digito a 250 Hz) */
    RCC->APB1ENR &= ~RCC_APB1ENR_TIM2EN;     // El TIM2 esta en el bus APB1: limpio su bit
    RCC->APB1ENR |=  RCC_APB1ENR_TIM2EN;     // Le doy reloj al TIM2

    TIM2->CR1 &= ~TIM_CR1_CEN;               // CR1 bit CEN: apago el contador mientras lo configuro
    TIM2->CNT = 0;                           // CNT: el contador empieza en 0
    TIM2->PSC = (16 - 1);                    // PSC: divide el reloj por 16 -> cuenta 1 vez por microsegundo
    TIM2->ARR = (1000 - 1);                  // ARR: hasta donde cuenta -> 1000 us = 1 ms
    TIM2->CR1 &= ~(0x3 << TIM_CR1_CKD_Pos);  // CKD: sin division extra del reloj
    TIM2->CR1 |=  (TIM_CR1_ARPE);            // ARPE: el ARR se actualiza al terminar cada ciclo
    TIM2->CR1 &= ~TIM_CR1_DIR;               // DIR en 0: cuenta hacia arriba
    TIM2->DIER |= TIM_DIER_UIE;              // DIER bit UIE: genera interrupcion cuando llega al ARR

    NVIC_EnableIRQ(TIM2_IRQn);               // El NVIC deja pasar la interrupcion del TIM2
    TIM2->CR1 |= TIM_CR1_CEN;                // Arranca el contador

    /* TIM3: 16 MHz / 1600 = 10 kHz ; 10 kHz / 2500 = 4 Hz (cada 0,25 s) */
    RCC->APB1ENR &= ~RCC_APB1ENR_TIM3EN;     // El TIM3 tambien esta en APB1: limpio su bit
    RCC->APB1ENR |=  RCC_APB1ENR_TIM3EN;     // Le doy reloj al TIM3

    TIM3->CR1 &= ~TIM_CR1_CEN;               // Apago el contador mientras lo configuro
    TIM3->CNT = 0;                           // El contador empieza en 0
    TIM3->PSC = (1600 - 1);                  // Divide por 1600 -> cuenta 10000 veces por segundo
    TIM3->ARR = (2500 - 1);                  // Cuenta hasta 2500 -> 0,25 s
    TIM3->CR1 &= ~(0x3 << TIM_CR1_CKD_Pos);  // Sin division extra del reloj
    TIM3->CR1 |=  (TIM_CR1_ARPE);            // El ARR se actualiza al terminar cada ciclo
    TIM3->CR1 &= ~TIM_CR1_DIR;               // Cuenta hacia arriba
    TIM3->DIER |= TIM_DIER_UIE;              // Interrupcion cuando llega al ARR

    NVIC_EnableIRQ(TIM3_IRQn);               // El NVIC deja pasar la interrupcion del TIM3
    TIM3->CR1 |= TIM_CR1_CEN;                // Arranca el contador

    /* SysTick: reloj del sistema independiente, 16 MHz / 16000 = 1 kHz (cada 1 ms) */
    SysTick->LOAD = (16000 - 1);                    // LOAD: cuenta 16000 pulsos = 1 ms y vuelve a empezar
    SysTick->VAL  = 0;                              // VAL: valor actual del contador, lo reinicio
    SysTick->CTRL |= SysTick_CTRL_CLKSOURCE_Msk;    // CLKSOURCE: usa el reloj del procesador (16 MHz)
    SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;      // TICKINT: genera interrupcion al llegar a 0
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;       // ENABLE: arranca el SysTick
}

/* init_exti: PB0 -> EXTI0 (sube, flanco de subida), PC1 -> EXTI1 (baja, flanco de bajada) */
void init_exti(void){
    /* PASO 1: RELOJ DEL SYSCFG (conecta cada pin con su linea EXTI) */
    RCC->APB2ENR &= ~RCC_APB2ENR_SYSCFGEN;              // El SYSCFG esta en el bus APB2: limpio su bit
    RCC->APB2ENR |=  RCC_APB2ENR_SYSCFGEN;              // Le doy reloj al SYSCFG

    /* PASO 2: PB0 -> EXTI0 (sensor que sube) */
    SYSCFG->EXTICR[0] &= ~(SYSCFG_EXTICR1_EXTI0);       // EXTICR1: elige el puerto de la linea 0. Limpio
    SYSCFG->EXTICR[0] |=  (SYSCFG_EXTICR1_EXTI0_PB);    // La linea 0 escucha al puerto B (PB0)
    EXTI->RTSR  |=  (EXTI_RTSR_TR0);                    // RTSR: dispara en flanco de subida (al tapar)
    EXTI->FTSR  &= ~(EXTI_FTSR_TR0);                    // FTSR: no dispara en flanco de bajada
    EXTI->SWIER &= ~(EXTI_SWIER_SWIER0);                // SWIER: no la disparo por software
    EXTI->IMR   |=  (EXTI_IMR_MR0);                     // IMR: dejo pasar la interrupcion de la linea 0
    EXTI->PR    |=  (EXTI_PR_PR0);                      // PR: borro alguna bandera vieja (se borra con 1)
    NVIC_EnableIRQ(EXTI0_IRQn);                         // El NVIC deja pasar la interrupcion EXTI0

    /* PASO 3: PC1 -> EXTI1 (sensor que baja) */
    SYSCFG->EXTICR[0] &= ~(SYSCFG_EXTICR1_EXTI1);       // Limpio el puerto de la linea 1
    SYSCFG->EXTICR[0] |=  (SYSCFG_EXTICR1_EXTI1_PC);    // La linea 1 escucha al puerto C (PC1)
    EXTI->FTSR  |=  (EXTI_FTSR_TR1);                    // Dispara en flanco de bajada (al destapar)
    EXTI->RTSR  &= ~(EXTI_RTSR_TR1);                    // No dispara en flanco de subida
    EXTI->SWIER &= ~(EXTI_SWIER_SWIER1);                // No la disparo por software
    EXTI->IMR   |=  (EXTI_IMR_MR1);                     // Dejo pasar la interrupcion de la linea 1
    EXTI->PR    |=  (EXTI_PR_PR1);                      // Borro alguna bandera vieja
    NVIC_EnableIRQ(EXTI1_IRQn);                         // El NVIC deja pasar la interrupcion EXTI1
}

/* split_number: separa el numero en sus 4 cifras (solo RAM, no toca pines) */
void split_number(uint16_t number){
    unidades =  number % 10;           // El residuo de dividir por 10 es la ultima cifra
    decenas  = (number / 10) % 10;     // Quito una cifra y saco la ultima
    centenas = (number / 100) % 10;    // Quito dos cifras y saco la ultima
    umil     = (number / 1000) % 10;   // Quito tres cifras y saco la ultima
}

/* draw_number: apaga los 7 segmentos (1) y enciende con 0 los del numero */
void draw_number(uint8_t number){
    // Primero apago todos los segmentos (1 = apagado por ser anodo comun)
    GPIOA->ODR |=  (GPIO_ODR_OD11 | GPIO_ODR_OD12);                     // Apago f y b
    GPIOB->ODR |=  (GPIO_ODR_OD12);                                     // Apago a
    GPIOC->ODR |=  (GPIO_ODR_OD10 | GPIO_ODR_OD11 | GPIO_ODR_OD12);     // Apago e, c y d
    GPIOD->ODR |=  (GPIO_ODR_OD2);                                      // Apago g

    switch (number)                                                     // Segun el numero, prendo (0) sus segmentos
    {
        case 0: // a b c d e f
            GPIOB->ODR &= ~(GPIO_ODR_OD12);                             // Prendo a
            GPIOA->ODR &= ~(GPIO_ODR_OD12 | GPIO_ODR_OD11);             // Prendo b y f
            GPIOC->ODR &= ~(GPIO_ODR_OD11 | GPIO_ODR_OD12 | GPIO_ODR_OD10); // Prendo c, d y e
            break;

        case 1: // b c
            GPIOA->ODR &= ~(GPIO_ODR_OD12);                             // Prendo b
            GPIOC->ODR &= ~(GPIO_ODR_OD11);                             // Prendo c
            break;

        case 2: // a b d e g
            GPIOB->ODR &= ~(GPIO_ODR_OD12);                             // Prendo a
            GPIOA->ODR &= ~(GPIO_ODR_OD12);                             // Prendo b
            GPIOC->ODR &= ~(GPIO_ODR_OD12 | GPIO_ODR_OD10);             // Prendo d y e
            GPIOD->ODR &= ~(GPIO_ODR_OD2);                              // Prendo g
            break;

        case 3: // a b c d g
            GPIOB->ODR &= ~(GPIO_ODR_OD12);                             // Prendo a
            GPIOA->ODR &= ~(GPIO_ODR_OD12);                             // Prendo b
            GPIOC->ODR &= ~(GPIO_ODR_OD11 | GPIO_ODR_OD12);             // Prendo c y d
            GPIOD->ODR &= ~(GPIO_ODR_OD2);                              // Prendo g
            break;

        case 4: // b c f g
            GPIOA->ODR &= ~(GPIO_ODR_OD12 | GPIO_ODR_OD11);             // Prendo b y f
            GPIOC->ODR &= ~(GPIO_ODR_OD11);                             // Prendo c
            GPIOD->ODR &= ~(GPIO_ODR_OD2);                              // Prendo g
            break;

        case 5: // a c d f g
            GPIOB->ODR &= ~(GPIO_ODR_OD12);                             // Prendo a
            GPIOA->ODR &= ~(GPIO_ODR_OD11);                             // Prendo f
            GPIOC->ODR &= ~(GPIO_ODR_OD11 | GPIO_ODR_OD12);             // Prendo c y d
            GPIOD->ODR &= ~(GPIO_ODR_OD2);                              // Prendo g
            break;

        case 6: // a c d e f g
            GPIOB->ODR &= ~(GPIO_ODR_OD12);                             // Prendo a
            GPIOA->ODR &= ~(GPIO_ODR_OD11);                             // Prendo f
            GPIOC->ODR &= ~(GPIO_ODR_OD11 | GPIO_ODR_OD12 | GPIO_ODR_OD10); // Prendo c, d y e
            GPIOD->ODR &= ~(GPIO_ODR_OD2);                              // Prendo g
            break;

        case 7: // a b c
            GPIOB->ODR &= ~(GPIO_ODR_OD12);                             // Prendo a
            GPIOA->ODR &= ~(GPIO_ODR_OD12);                             // Prendo b
            GPIOC->ODR &= ~(GPIO_ODR_OD11);                             // Prendo c
            break;

        case 8: // a b c d e f g
            GPIOB->ODR &= ~(GPIO_ODR_OD12);                             // Prendo a
            GPIOA->ODR &= ~(GPIO_ODR_OD12 | GPIO_ODR_OD11);             // Prendo b y f
            GPIOC->ODR &= ~(GPIO_ODR_OD11 | GPIO_ODR_OD12 | GPIO_ODR_OD10); // Prendo c, d y e
            GPIOD->ODR &= ~(GPIO_ODR_OD2);                              // Prendo g
            break;

        case 9: // a b c d f g
            GPIOB->ODR &= ~(GPIO_ODR_OD12);                             // Prendo a
            GPIOA->ODR &= ~(GPIO_ODR_OD12 | GPIO_ODR_OD11);             // Prendo b y f
            GPIOC->ODR &= ~(GPIO_ODR_OD11 | GPIO_ODR_OD12);             // Prendo c y d
            GPIOD->ODR &= ~(GPIO_ODR_OD2);                              // Prendo g
            break;

        default:                                                        // Numero no valido
            break;                                                      // Se queda todo apagado
    }
}

/* update_display: apaga los 4 digitos, en el bloque del digito que toca primero
 * CREA el numero y despues lo PRENDE (asi no hay fantasmas), y pasa al siguiente */
void update_display(uint8_t display){
    GPIOB->ODR |=  (GPIO_ODR_OD7);                                      // Apago el digito de unidades
    GPIOC->ODR |=  (GPIO_ODR_OD8 | GPIO_ODR_OD6 | GPIO_ODR_OD13);       // Apago decenas, centenas y u. de mil

    switch (display)                                                    // Miro que digito toca
    {
        case 0: // Unidades
            draw_number(unidades);                                      // Creo el numero en los segmentos
            GPIOB->ODR &= ~(GPIO_ODR_OD7);                              // Prendo el digito (PB7 = 0, el PNP conduce)
            break;

        case 1: // Decenas
            draw_number(decenas);                                       // Creo el numero
            GPIOC->ODR &= ~(GPIO_ODR_OD8);                              // Prendo el digito (PC8 = 0)
            break;

        case 2: // Centenas
            draw_number(centenas);                                      // Creo el numero
            GPIOC->ODR &= ~(GPIO_ODR_OD6);                              // Prendo el digito (PC6 = 0)
            break;

        case 3: // Unidades de mil
            draw_number(umil);                                          // Creo el numero
            GPIOC->ODR &= ~(GPIO_ODR_OD13);                             // Prendo el digito (PC13 = 0)
            break;

        default:                                                        // Valor raro
            break;                                                      // No prendo nada
    }

    if(display_actual == 3){                                            // Si ya mostre el ultimo digito...
        display_actual = 0;                                             // ...vuelvo al primero
    }
    else{
        display_actual++;                                               // Si no, paso al siguiente
    }
}

/* action_fsm: ejecuta el estado que toca */
void action_fsm(uint8_t state){
    switch (state)                                                  // Miro en que estado estoy
    {
        case STATE_DEBOUNCE:                                        // Estado: esperar el rebote
            if((getTicks() - tiempo_evento) >= TIEMPO_DEBOUNCE){    // Ya pasaron los 50 ms desde que disparo?
                if(sensor_activo == SENSOR_SUBE){                   // Fue el sensor que suma
                    if((GPIOB->IDR & GPIO_IDR_ID0) != 0){           // IDR lee el pin: si PB0 sigue en 1 (tapado) fue real
                        numero_mostrar++;                           // Sumo 1 (sin limite: de 65535 pasa a 0)
                        fsm_Next_State = UPDATE_DISPLAY;            // Siguiente: actualizar las cifras
                    }
                    else{
                        fsm_Next_State = IDLE;                      // Fue ruido: no cuento y vuelvo a reposo
                    }
                }
                else if(sensor_activo == SENSOR_BAJA){              // Fue el sensor que resta
                    if((GPIOC->IDR & GPIO_IDR_ID1) == 0){           // Si PC1 sigue en 0 (destapado) fue real
                        numero_mostrar--;                           // Resto 1 (sin limite: de 0 pasa a 65535)
                        fsm_Next_State = UPDATE_DISPLAY;            // Siguiente: actualizar las cifras
                    }
                    else{
                        fsm_Next_State = IDLE;                      // Fue ruido: no cuento
                    }
                }
                else{
                    fsm_Next_State = IDLE;                          // No se sabe que sensor fue: vuelvo a reposo
                }
            }
            break;

        case UPDATE_DISPLAY:                                        // Estado: actualizar cifras
            split_number(numero_mostrar);                           // Separo el numero nuevo (el TIM2 lo muestra solo)
            fsm_Next_State = IDLE;                                  // Termine: vuelvo a reposo
            break;

        default:                                                    // Estado raro
            fsm_Next_State = IDLE;                                  // Por seguridad vuelvo a reposo
            break;
    }
}

/* getTicks: devuelve el tiempo en ms desde que arranco el programa */
uint32_t getTicks(void){
    return updateTicks;                        // El SysTick lo va subiendo cada 1 ms
}

/* =========================================================================
 * RUTINAS DE ATENCION A LAS INTERRUPCIONES (ISR)
 * ========================================================================= */

/* TIM2: refresco del display, cada 1 ms */
void TIM2_IRQHandler(void){
    if(TIM2->SR & TIM_SR_UIF){               // SR bit UIF: reviso que la interrupcion sea por update
        TIM2->SR &= ~TIM_SR_UIF;             // Borro la bandera (en este registro se borra con 0)
        update_display(display_actual);      // Muestro el digito que toca
    }
}

/* SysTick: reloj del sistema, cada 1 ms */
void SysTick_Handler(void){
    updateTicks++;                           // Paso 1 ms mas
}

/* TIM3: blinky del LED de estado, cada 0,25 s */
void TIM3_IRQHandler(void){
    if(TIM3->SR & TIM_SR_UIF){               // Reviso la bandera de update
        TIM3->SR &= ~TIM_SR_UIF;             // Borro la bandera
        GPIOH->ODR ^= GPIO_ODR_OD1;          // ^= invierte el pin: si estaba prendido se apaga y al reves
    }
}

/* EXTI0: sensor que sube (PB0). Solo anota el evento en RAM y pide el antirrebote */
void EXTI0_IRQHandler(void){
    if(EXTI->PR & EXTI_PR_PR0){              // PR: reviso que la bandera de la linea 0 este en 1
        EXTI->PR |= EXTI_PR_PR0;             // Borro la bandera (en este registro se borra con 1)

        if(fsm_Next_State == IDLE){          // Solo si no hay otro evento en curso (si no, es rebote)
            sensor_activo  = SENSOR_SUBE;    // Anoto que fue el sensor que suma
            tiempo_evento  = getTicks();     // Anoto en que ms paso
            fsm_Next_State = STATE_DEBOUNCE; // Le pido al main que haga el antirrebote
        }
    }
}

/* EXTI1: sensor que baja (PC1). Solo anota el evento en RAM y pide el antirrebote */
void EXTI1_IRQHandler(void){
    if(EXTI->PR & EXTI_PR_PR1){              // Reviso que la bandera de la linea 1 este en 1
        EXTI->PR |= EXTI_PR_PR1;             // Borro la bandera (se borra con 1)

        if(fsm_Next_State == IDLE){          // Solo si no hay otro evento en curso
            sensor_activo  = SENSOR_BAJA;    // Anoto que fue el sensor que resta
            tiempo_evento  = getTicks();     // Anoto en que ms paso
            fsm_Next_State = STATE_DEBOUNCE; // Le pido al main que haga el antirrebote
        }
    }
}