/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 */
//include: librerias que se van a usar 
#include <stdint.h>
#include "stm32f411xe.h"
#include "stm32f4xx.h"
//2. Definicion de variables (globables)


//cabeceras (prototipos) de funciones
void init_gpio();
void init_tim3();
void init_exti13();

//main() con su while (1) para el que el programa siga en bucle
int main(void)
{
//llamar a las funciones de configuracion
init_gpio();

// cargamos confi de l tim 3 interrupcion cada 0,25s
init_tim3();

// configuramos la interrupcion externa del pin PC13 (boton)
init_exti13();

while (1){

    } 
return 0; 
}
void init_gpio(){
uint16_t aux_value = 0; 

//todo esto se esta trabajando con PA5 Puesto 5 que es donde esta el led 
RCC->AHB1ENR &= ~RCC_AHB1ENR_GPIOAEN; //Limpia 
RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;

GPIOA->MODER &= ~(0b11 << GPIO_MODER_MODER5_Pos);
GPIOA->MODER |= (0b1 << GPIO_MODER_MODER5_Pos);

GPIOA->OTYPER &= ~GPIO_OTYPER_OT5;
GPIOA->OSPEEDR &= ~(0b11 << GPIO_OSPEEDR_OSPEED5_Pos);
GPIOA->PUPDR &= ~(0B11<< GPIO_PUPDR_PUPD5_Pos);

GPIOA->ODR |= (GPIO_ODR_OD5);

//============================//
//Señal de reloj para el GPIOC//
RCC->AHB1ENR &= ~ RCC_AHB1ENR_GPIOCEN; //Limpiando posicion 
RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;

//Configuracion pin 13
GPIOC->MODER &= ~(((1<<0)|(1<<1))<<GPIO_MODER_MODER13_Pos); //00 = entrada
GPIOC->PUPDR &= ~(((1<<0)|(1<<1))<<GPIO_PUPDR_PUPD13_Pos);  //
    }

void init_tim3(){
//señal de reloj para el tim3
RCC->APB1ENR &= ~RCC_APB1ENR_TIM3EN;//Limpia posicion 
RCC->APB1ENR |=RCC_APB1ENR_TIM3EN; // activar señal reloj

TIM3->CNT = 0; // reinicia el contador 
TIM3->PSC = (1600-1); //divisor por 1600
TIM3->ARR = (2500-1); //para generar un evento uptdata 0,25

TIM3->CR1 &= ~(0x3 << TIM_CR1_CKD_Pos); //limpiamos
TIM3->CR1 &= ~ (TIM_CR1_ARPE); //limpiamos
TIM3->CR1 |= (TIM_CR1_ARPE); //PRECARGA
TIM3->CR1 &= ~TIM_CR1_DIR; //LIMPIANDO CONTADOR ASENDIENTE 

TIM3->DIER |=TIM_DIER_UIE;//habilitamos la interrepcuion por event update

//configuramos el nvic 
NVIC_EnableIRQ(TIM3_IRQn); //REGISTRA LA INTERRUMPCION

TIM3->CR1|= TIM_CR1_CEN; //ACTIVAMOS EL CONTROLADOR 
}

void init_exti13(){
//Señal de reloj para el SYSCFG
// SYSCFG es el bloque que conecta cada pin fisico (PA13, PB13, PC13...) con
// una linea EXTI. Cuelga del bus APB2, por eso se activa en RCC->APB2ENR.
RCC->APB2ENR &= ~RCC_APB2ENR_SYSCFGEN; //Limpiando posicion
RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;  //Activar señal de reloj para SYSCFG

//Señal de reloj para el EXTI
// En el STM32F4 el EXTI NO tiene bit de habilitacion en el RCC (siempre esta
// activo), asi que no hay nada que escribir aqui.

//Conectar el pin PC13 con la linea EXTI13 
// La linea EXTI13 puede escuchar PA13, PB13, PC13... pero solo UNO a la vez.
// SYSCFG->EXTICR[3] (EXTICR4) controla las lineas 12 a 15; los bits [7:4]
// son los de la linea 13. Valor 0010 = puerto C.
SYSCFG->EXTICR[3] &= ~SYSCFG_EXTICR4_EXTI13;   //Limpiando posicion (0000 = puerto A)
SYSCFG->EXTICR[3] |= SYSCFG_EXTICR4_EXTI13_PC; //0010 = linea 13 conectada al puerto C

//Detectar solo flanco de bajada
// FTSR (Falling Trigger Selection Register): 1 en el bit 13 = la linea 13
// dispara cuando la señal pasa de 1 a 0. (El boton de la Nucleo en PC13 es
// activo en bajo: al presionarlo el pin baja a 0.)
EXTI->FTSR |= EXTI_FTSR_TR13;     // (1<<13) Activar flanco de bajada en linea 13

// RTSR (Rising Trigger Selection Register): flanco de subida desactivado.
// Solo limpio el bit 13 para no afectar otras lineas EXTI.
EXTI->RTSR &= ~EXTI_RTSR_TR13;    // Desactivar flanco de subida en linea 13

// SWIER (Software Interrupt Event Register): permite disparar la interrupcion
// por software escribiendo 1. Lo dejamos en 0 para que solo la dispare el boton.
EXTI->SWIER &= ~EXTI_SWIER_SWIER13; // Desactivar disparo por software en linea 13

//=========================================================//
//Habilitar la linea 13 (desenmascarar la interrupcion)
// IMR (Interrupt Mask Register): 1 = la solicitud de interrupcion de esa linea
// NO esta enmascarada, o sea, se deja pasar hacia el NVIC.
// (EMR es para eventos, no para interrupciones.)
EXTI->IMR |= EXTI_IMR_MR13;       // (1<<13) Habilitar interrupcion en linea 13

// PR (Pending Register): bandera de "llego una interrupcion". La limpiamos antes
// de habilitar el NVIC para que no quede una bandera vieja disparando la ISR.
EXTI->PR = EXTI_PR_PR13;          // Escribir 1 LIMPIA la bandera

//=========================================================//
//Registrar la EXTI13 en el NVIC
// Las lineas EXTI10 a EXTI15 comparten UNA sola interrupcion: EXTI15_10_IRQn.
NVIC_EnableIRQ(EXTI15_10_IRQn);   //Registra la interrupcion en el NVIC
}

/*RUTINA DE ATENCION A LA INTERRUPCION DE TIM3 */
void TIM3_IRQHandler (void){
//verificamos bandera 
if(TIM3->SR && TIM_SR_UIF){
TIM3->SR &= ~TIM_SR_UIF; //limpiando bandera
GPIOA->ODR ^= GPIO_ODR_OD5;
    }
}

/*RUTINA DE ATENCION A LA INTERRUPCION DE EXTI13
* Como las lineas 10 a 15 comparten vector, el nombre es EXTI15_10_IRQHandler
* (se toma de startup_stm32f411xe.s, no modificar ese archivo)*/
void EXTI15_10_IRQHandler (void){
//verificamos que la bandera de la linea 13 sea la que esta levantada
// (como el vector es compartido, pudo haberse activado otra linea 10-15)
if(EXTI->PR & EXTI_PR_PR13){
EXTI->PR = EXTI_PR_PR13; //limpiando bandera: se limpia escribiendo 1

//AQUI VA LO QUE DEBE HACER EL BOTON (pendiente)
    }
}