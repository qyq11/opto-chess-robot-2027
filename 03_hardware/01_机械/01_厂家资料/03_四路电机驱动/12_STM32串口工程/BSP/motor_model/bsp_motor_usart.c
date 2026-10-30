#include "bsp_motor_usart.h"


//电机串口初始化	Motor serial port initialization
void Motor_Usart_init (void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;
	// 打开串口GPIO的时钟	Turn on the serial GPIO clock
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

	// 打开串口外设的时钟	Enable the clock of the serial port peripheral
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

	// 将USART Tx的GPIO配置为推挽复用模式		Configure the GPIO of USART Tx to push-pull multiplexing mode
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	// 将USART Rx的GPIO配置为浮空输入模式		Configure the GPIO of USART Rx to floating input mode
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

	//Usart2 NVIC 配置	Usart2 NVIC Configuration
	NVIC_InitStructure.NVIC_IRQChannel = USART2_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1; //抢占优先级	Preemption priority
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;		  //子优先级		Subpriority
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;			  //IRQ通道使能	IRQ channel enable
	NVIC_Init(&NVIC_InitStructure);							  //根据指定的参数初始化NVIC寄存器	Initializes the NVIC registers according to the specified parameters

	
	// 配置波特率	Configuring the baud rate
	USART_InitStructure.USART_BaudRate = 115200;
	// 配置 针数据字长	Configuration Pin Data Word Length
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;
	// 配置停止位	Configuring stop bits
	USART_InitStructure.USART_StopBits = USART_StopBits_1;
	// 配置校验位	Configuring the check digit
	USART_InitStructure.USART_Parity = USART_Parity_No;
	// 配置硬件流控制	Configuring Hardware Flow Control
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	// 配置工作模式，收发一起		Configure the working mode, send and receive together
	USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;

	// 完成串口的初始化配置	Complete the initial configuration of the serial port
	USART_Init(USART2, &USART_InitStructure);

	//开启串口接收中断	Enable serial port receive interrupt
	USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
	// 使能串口	Enable the serial port
	USART_Cmd(USART2, ENABLE);

}



/************************************************
函数名称 ： Send_Motor_U8		Function name: Send_Motor_U8
功    能 ： UART2发送一个字符	Function: UART2 sends a character
参    数 ： Data --- 数据		Parameter: Data --- data
返 回 值 ： 无					Return value: None
*************************************************/
void Send_Motor_U8(uint8_t Data)
{
	while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET)
		;
	USART_SendData(USART2, Data);
}

/************************************************
函数名称 ： Send_Motor_ArrayU8	Function name: Send_Motor_ArrayU8
功    能 ： 串口1发送N个字符		Function: Serial port 1 sends N characters
参    数 ： pData ---- 字符串	Parameter: pData ---- string
            Length --- 长度		Length --- length
返 回 值 ： 无					Return value: None
*************************************************/
void Send_Motor_ArrayU8(uint8_t *pData, uint16_t Length)
{
	while (Length--)
	{
		Send_Motor_U8(*pData);
		pData++;
	}
}


/*  串口中断接收处理 */
/* Serial port interrupt reception processing */
void USART2_IRQHandler(void)
{
	uint8_t Rx2_Temp = 0;
	/* 读数据会清空中断位 */
	/*Reading data will clear the interrupt bit*/
	while (USART_GetFlagStatus(USART2, USART_FLAG_RXNE) == RESET);
	
	Rx2_Temp = USART_ReceiveData(USART2);
	
	//处理	deal with
	Deal_Control_Rxtemp(Rx2_Temp);

}






